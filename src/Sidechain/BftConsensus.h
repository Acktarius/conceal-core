// BftConsensus.h — block proposal, voting, and commit logic
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "SidechainTypes.h"
#include "SidechainConfig.h"
#include "GossipManager.h"
#include <functional>
#include <vector>
#include <mutex>
#include <atomic>
#include <unordered_map>

namespace Sidechain
{
  class SidechainStorage;

  class BftConsensus
  {
  public:
    using BlockCommittedCallback = std::function<void(const Block &)>;

    BftConsensus(SidechainStorage &storage,
                 const ValidatorInfo &self,
                 GossipManager &gossip);

    // Propose a new block with the given transactions
    // Returns true if block was committed, false if proposal failed
    bool proposeBlock(const std::vector<Transaction> &transactions, Block &outBlock);

    // Register callback for committed blocks
    void onBlockCommitted(BlockCommittedCallback callback);

    // Handle incoming gossip message (called from GossipManager)
    void handleMessage(const std::vector<uint8_t> &data, const std::string &fromIp);

    // Get current consensus height
    uint64_t getHeight() const { return m_currentHeight; }

    // Sync validator set from seed on first connect
    void syncValidators(const std::string &seedHost, uint16_t seedPort);

    // Handle validator sync messages
    bool handleSyncResponse(const std::vector<uint8_t> &data);

    // Request validator sync from seed
    void requestValidatorSync(const std::string &seedHost, uint16_t seedPort);

    // Broadcast a new validator to the network
    void broadcastNewValidator(const ValidatorInfo &validator);

  private:
    void broadcastProposal(const Block &block);
    void broadcastVote(const crypto::Hash &blockHash, const crypto::Signature &signature);
    void checkCommitCondition(const crypto::Hash &blockHash);

    SidechainStorage &m_storage;
    ValidatorInfo m_self;
    GossipManager &m_gossip;
    BlockCommittedCallback m_onBlockCommitted;

    std::atomic<uint64_t> m_currentHeight{0};
    bool m_validatorsSynced = false;

    struct PendingBlock
    {
      Block block;
      std::vector<crypto::Signature> votes;
      bool committed = false;
    };

    std::unordered_map<crypto::Hash, PendingBlock> m_pendingBlocks;
    mutable std::mutex m_mutex;
    crypto::Hash m_previousBlockHash;
    
  };
}