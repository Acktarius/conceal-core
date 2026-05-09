// BftConsensus implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BftConsensus.h"
#include "BoltDex.h"
#include "SidechainStorage.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Common/StringTools.h"
#include "Common/MemoryInputStream.h"
#include "Serialization/BinaryInputStreamSerializer.h"
#include "Serialization/BinaryOutputStreamSerializer.h"
#include "crypto/hash.h"
#include <Common/StringOutputStream.h>

#include <iostream>
#include <chrono>
#include <sstream>

namespace Sidechain
{
  BftConsensus::BftConsensus(SidechainStorage &storage,
                             const ValidatorInfo &self,
                             GossipManager &gossip)
      : m_storage(storage), m_self(self), m_gossip(gossip)
  {
    m_currentHeight = storage.topBlockHeight();
    m_previousBlockHash = crypto::Hash{};
    m_gossip.onMessage([this](const std::vector<uint8_t> &data, const std::string &fromIp)
                       { handleMessage(data, fromIp); });
  }

  void BftConsensus::onBlockCommitted(BlockCommittedCallback callback)
  {
    m_onBlockCommitted = callback;
  }

  bool BftConsensus::proposeBlock(const std::vector<Transaction> &transactions, Block &outBlock)
  {
    if (transactions.empty())
      return false;

    std::cout << "BftConsensus::proposeBlock called with " << transactions.size() << " txs" << std::endl;

    Block block;
    block.header.height = m_currentHeight + 1;
    block.header.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    block.header.validatorId = m_self.id;

    if (m_currentHeight > 0)
      block.header.previousBlockHash = m_previousBlockHash;

    block.transactions = transactions;
    for (size_t i = 0; i < block.transactions.size(); ++i)
      block.transactions[i].id = i;

    std::cout << "BftConsensus::proposeBlock: " << block.transactions.size()
              << " tx, first tx type=" << static_cast<int>(block.transactions[0].type) << std::endl;

    std::cout << "BftConsensus::proposeBlock: calling toBinaryArray..." << std::endl;
    cn::BinaryArray blockBytes = cn::toBinaryArray(block);
    std::cout << "BftConsensus::proposeBlock: toBinaryArray returned " << blockBytes.size() << " bytes" << std::endl;

    crypto::Hash blockHash;

    std::cout << "BftConsensus::proposeBlock: calling cn_fast_hash..." << std::endl;
    crypto::cn_fast_hash(blockBytes.data(), blockBytes.size(), blockHash);

    std::cout << "BftConsensus::proposeBlock: hash done" << std::endl;
    block.header.blockHash = blockHash;

    std::cout << "BftConsensus::proposeBlock: hash done, signing..." << std::endl;
    crypto::generate_signature(blockHash, m_self.publicKey, m_self.secretKey, block.header.validatorSignature);
    std::cout << "BftConsensus::proposeBlock: signing complete" << std::endl;

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      PendingBlock pending;
      pending.block = block;
      pending.votes.push_back(block.header.validatorSignature);
      m_pendingBlocks[blockHash] = pending;
    }

    std::cout << "BftConsensus: about to broadcast proposal, height=" << block.header.height << std::endl;
    broadcastProposal(block);
    std::cout << "BftConsensus: broadcast done, threshold=" << SidechainConfig::BFT_BLOCK_THRESHOLD << std::endl;

    if (SidechainConfig::BFT_BLOCK_THRESHOLD <= 1)
    {
      std::cout << "BftConsensus: threshold <= 1, committing immediately" << std::endl;
      checkCommitCondition(blockHash);
      outBlock = block;
      return true;
    }

    auto startTime = std::chrono::steady_clock::now();
    while (true)
    {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - startTime)
                         .count();

      if (elapsed > SidechainConfig::BFT_VOTE_TIMEOUT_MS)
        break;

      {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pendingBlocks.find(blockHash);
        if (it != m_pendingBlocks.end() && it->second.committed)
        {
          outBlock = it->second.block;
          return true;
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "BftConsensus: proposal timed out, height=" << block.header.height << std::endl;
    return false;
  }

  void BftConsensus::handleMessage(const std::vector<uint8_t> &data, const std::string &fromIp)
  {
    if (data.empty())
      return;

    uint8_t msgType = data[0];

    // Sync request from a new validator (0x03)
    if (msgType == 0x03) // Sync request from a new validator
    {
      std::cout << "BftConsensus: received sync request from " << fromIp << std::endl;

      // Extract the requester's gossip port from the message (bytes 1-2)
      uint16_t requesterPort = SidechainConfig::DEFAULT_RPC_BIND_PORT + SidechainConfig::GOSSIP_PORT_OFFSET; // fallback
      if (data.size() >= 3)
      {
        memcpy(&requesterPort, &data[1], sizeof(uint16_t));
      }

      std::cout << "BftConsensus: requester gossip port is " << requesterPort << std::endl;

      // Build response with all active validators
      auto activeValidators = m_storage.getActiveValidators();

      std::string serializedData;
      common::StringOutputStream stream(serializedData);
      cn::BinaryOutputStreamSerializer serializer(stream);

      uint32_t count = static_cast<uint32_t>(activeValidators.size());
      serializer(count, "count");
      for (const auto &v : activeValidators)
      {
        serializer(const_cast<ValidatorInfo &>(v), "validator");
      }

      std::vector<uint8_t> message;
      message.push_back(0x04); // Sync response type
      message.insert(message.end(), serializedData.begin(), serializedData.end());

      // Extract IP from fromIp (format: "IP:port")
      std::string peerIp = fromIp.substr(0, fromIp.find(':'));

      m_gossip.sendTo(peerIp, requesterPort, message);
      std::cout << "BftConsensus: sent validator sync response to " << peerIp << ":" << requesterPort << std::endl;
      return;
    }

    // Sync response (0x04)
    if (msgType == 0x04)
    {
      std::cout << "BftConsensus: received sync response from " << fromIp << std::endl;
      handleSyncResponse(data);
      return;
    }

    // New validator announcement (0x05)
    if (msgType == 0x05)
    {
      std::cout << "BftConsensus: received new validator announcement from " << fromIp << std::endl;
      try
      {
        std::string dataStr(data.begin() + 1, data.end());
        common::MemoryInputStream stream(dataStr.data(), dataStr.size());
        cn::BinaryInputStreamSerializer serializer(stream);

        ValidatorInfo newValidator;
        serializer(newValidator, "validator");
        m_storage.addValidator(newValidator);
        std::cout << "BftConsensus: added new validator id=" << newValidator.id << std::endl;
      }
      catch (const std::exception &e)
      {
        std::cout << "BftConsensus: failed to deserialize new validator: " << e.what() << std::endl;
      }
      return;
    }

    // Block proposal (0x01)
    if (msgType == 0x01)
    {
      std::cout << "BftConsensus: received block proposal from " << fromIp << std::endl;

      Block proposedBlock;
      try
      {
        std::vector<uint8_t> blockData(data.begin() + 1, data.end());
        if (!cn::fromBinaryArray(proposedBlock, blockData))
        {
          std::cout << "BftConsensus: failed to deserialize block" << std::endl;
          return;
        }
      }
      catch (const std::exception &e)
      {
        std::cout << "BftConsensus: failed to deserialize block: " << e.what() << std::endl;
        return;
      }

      crypto::Hash blockHash = proposedBlock.header.blockHash;
      std::cout << "BftConsensus: proposal height=" << proposedBlock.header.height
                << " currentHeight=" << m_currentHeight << std::endl;

      auto validators = m_storage.getActiveValidators();
      const ValidatorInfo *proposer = nullptr;
      for (const auto &v : validators)
      {
        if (v.id == proposedBlock.header.validatorId)
        {
          proposer = &v;
          break;
        }
      }

      if (!proposer)
      {
        std::cout << "BftConsensus: unknown proposer " << proposedBlock.header.validatorId << std::endl;
        return;
      }

      bool valid = crypto::check_signature(blockHash, proposer->publicKey, proposedBlock.header.validatorSignature);
      if (!valid)
      {
        std::cout << "BftConsensus: invalid proposer signature" << std::endl;
        return;
      }

      if (proposedBlock.header.height != m_currentHeight + 1)
      {
        std::cout << "BftConsensus: wrong block height " << proposedBlock.header.height
                  << " (expected " << (m_currentHeight + 1) << ")" << std::endl;
        return;
      }

      if (m_currentHeight > 0)
      {
        crypto::Hash expectedPrev = m_storage.getBlockHash(m_currentHeight);
        if (proposedBlock.header.previousBlockHash != expectedPrev)
        {
          std::cout << "BftConsensus: wrong previous block hash" << std::endl;
          return;
        }
      }

      crypto::Signature myVote;
      crypto::generate_signature(blockHash, m_self.publicKey, m_self.secretKey, myVote);

      {
        std::lock_guard<std::mutex> lock(m_mutex);
        PendingBlock &pending = m_pendingBlocks[blockHash];
        pending.block = proposedBlock;
        pending.votes.push_back(myVote);
      }

      broadcastVote(blockHash, myVote);

      std::cout << "BftConsensus: block accepted, votes=" << 1 << std::endl;

      if (SidechainConfig::BFT_BLOCK_THRESHOLD <= 1)
      {
        std::cout << "BftConsensus: committing received block height=" << proposedBlock.header.height << std::endl;
        checkCommitCondition(blockHash);
      }
    }
    // Vote (0x02)
    else if (msgType == 0x02)
    {
      std::cout << "BftConsensus: received vote from " << fromIp << std::endl;

      if (data.size() < 1 + sizeof(crypto::Hash) + sizeof(crypto::Signature))
        return;

      crypto::Hash blockHash;
      memcpy(blockHash.data, &data[1], sizeof(crypto::Hash));

      crypto::Signature sig;
      memcpy(&sig, &data[1 + sizeof(crypto::Hash)], sizeof(crypto::Signature));

      bool shouldCheck = false;
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pendingBlocks.find(blockHash);
        if (it != m_pendingBlocks.end())
        {
          it->second.votes.push_back(sig);
          shouldCheck = true;
        }
      }
      // Mutex released here, safe to call checkCommitCondition
      if (shouldCheck)
        checkCommitCondition(blockHash);
    }
  }

  // Validator sync
  void BftConsensus::syncValidators(const std::string &seedHost, uint16_t seedPort)
  {
    if (m_validatorsSynced)
      return;

    requestValidatorSync(seedHost, seedPort);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    m_validatorsSynced = true;
  }

  void BftConsensus::requestValidatorSync(const std::string &seedHost, uint16_t seedPort)
  {
    std::vector<uint8_t> message;
    message.push_back(0x03); // Sync request type

    // Include our gossip port so the seed knows where to respond
    uint16_t myGossipPort = m_self.port + SidechainConfig::GOSSIP_PORT_OFFSET;
    message.insert(message.end(), (uint8_t *)&myGossipPort, (uint8_t *)&myGossipPort + sizeof(myGossipPort));

    m_gossip.sendTo(seedHost, seedPort, message);
    std::cout << "BftConsensus: requested validator sync from " << seedHost << ":" << seedPort
              << " (my gossip port: " << myGossipPort << ")" << std::endl;
  }

  bool BftConsensus::handleSyncResponse(const std::vector<uint8_t> &data)
  {
    if (data.size() < 2)
      return false;

    std::vector<uint8_t> validatorsData(data.begin() + 1, data.end());
    std::string dataStr(validatorsData.begin(), validatorsData.end());
    common::MemoryInputStream stream(dataStr.data(), dataStr.size());
    cn::BinaryInputStreamSerializer serializer(stream);

    uint32_t count;
    serializer(count, "count");

    for (uint32_t i = 0; i < count; ++i)
    {
      ValidatorInfo v;
      serializer(v, "validator");
      m_storage.addValidator(v);
      std::cout << "BftConsensus: synced validator id=" << v.id
                << " pubkey=" << common::podToHex(v.publicKey).substr(0, 16) << "..." << std::endl;
    }

    std::cout << "BftConsensus: validator sync complete, total=" << count << std::endl;
    return true;
  }

  void BftConsensus::broadcastNewValidator(const ValidatorInfo &validator)
  {
    std::vector<uint8_t> message;
    message.push_back(0x05);

    std::string serializedData;
    common::StringOutputStream stream(serializedData);
    cn::BinaryOutputStreamSerializer serializer(stream);
    serializer(const_cast<ValidatorInfo &>(validator), "validator");

    std::string data = serializedData;
    message.insert(message.end(), data.begin(), data.end());

    m_gossip.broadcast(message);
    std::cout << "BftConsensus: broadcast new validator id=" << validator.id << std::endl;
  }

  // Broadcast helpers
  void BftConsensus::broadcastProposal(const Block &block)
  {
    std::vector<uint8_t> message;
    message.push_back(0x01);
    cn::BinaryArray blockData = cn::toBinaryArray(block);
    message.insert(message.end(), blockData.begin(), blockData.end());
    m_gossip.broadcast(message);
  }

  void BftConsensus::broadcastVote(const crypto::Hash &blockHash, const crypto::Signature &signature)
  {
    std::vector<uint8_t> message;
    message.push_back(0x02);
    message.insert(message.end(), blockHash.data, blockHash.data + sizeof(crypto::Hash));
    const uint8_t *sigBytes = reinterpret_cast<const uint8_t *>(&signature);
    message.insert(message.end(), sigBytes, sigBytes + sizeof(crypto::Signature));
    m_gossip.broadcast(message);
  }

  // Commit logic
  void BftConsensus::checkCommitCondition(const crypto::Hash &blockHash)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_pendingBlocks.find(blockHash);
    if (it == m_pendingBlocks.end() || it->second.committed)
      return;

    std::cout << "BftConsensus::checkCommitCondition: votes=" << it->second.votes.size()
              << " threshold=" << SidechainConfig::BFT_BLOCK_THRESHOLD << std::endl;

    if (it->second.votes.size() >= SidechainConfig::BFT_BLOCK_THRESHOLD)
    {
      it->second.committed = true;

      m_storage.addBlock(it->second.block, blockHash);

      // Apply all transactions and collect real transfer fees
      uint64_t totalFees = 0;
      for (const auto &tx : it->second.block.transactions)
      {
        m_storage.applyTransaction(tx);

        // Only count fees from transfers, mints, and burns (real economic activity)
        // CreateToken fees are always 0 and never counted
        if (tx.type == TransactionType::Transfer ||
            tx.type == TransactionType::Mint ||
            tx.type == TransactionType::Burn)
        {
          totalFees += tx.fee;
        }

        // Record token creation for rate limiting
        if (tx.type == TransactionType::CreateToken)
        {
          m_storage.recordTokenCreation(tx.from, it->second.block.header.height);
        }
      }

      // Proposer earns block reward + real transfer fees
      uint64_t totalReward = SidechainConfig::BLOCK_REWARD + totalFees;

      if (totalReward > 0)
      {
        auto activeValidators = m_storage.getActiveValidators();

        const ValidatorInfo *proposer = nullptr;
        for (const auto &v : activeValidators)
        {
          if (v.id == it->second.block.header.validatorId)
          {
            proposer = &v;
            break;
          }
        }

        if (proposer)
        {
          const crypto::PublicKey &rewardTarget = m_hasRewardKey ? m_rewardKey : proposer->publicKey;
          uint64_t currentBalance = 0;
          m_storage.getBalance(rewardTarget, 0, currentBalance);
          m_storage.setBalance(rewardTarget, 0, currentBalance + totalReward);

          std::cout << "Block reward: validator " << proposer->id
                    << " earned " << totalReward << " SCCX"
                    << " (block=" << SidechainConfig::BLOCK_REWARD
                    << " fees=" << totalFees
                    << " balance=" << (currentBalance + totalReward) << ")"
                    << std::endl;
        }
        else
        {
          std::cout << "Block proposer not found, reward burned" << std::endl;
        }
      }

      // Process DEX deposits from this block
      if (m_dexEngine)
      {
        m_dexEngine->processBlock(it->second.block);
        m_dexEngine->processSettlements();
      }

      m_currentHeight = it->second.block.header.height;
      m_previousBlockHash = it->second.block.header.blockHash;

      if (m_onBlockCommitted)
        m_onBlockCommitted(it->second.block);

      std::cout << "Block committed at height " << it->second.block.header.height
                << " with " << it->second.votes.size() << " votes" << std::endl;
    }
  }

  // Reward Validators
  void BftConsensus::setRewardKey(const crypto::PublicKey &rewardKey)
  {
    m_rewardKey = rewardKey;
    m_hasRewardKey = true;
  }
}