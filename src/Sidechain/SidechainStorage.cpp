// SidechainStorage implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SidechainStorage.h"
#include "SidechainConfig.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Common/StringTools.h"
#include "Serialization/SerializationTools.h"
#include <boost/filesystem.hpp>
#include <iostream>

namespace Sidechain
{
  namespace
  {
    std::string ensureDataDir(const std::string &dataDir)
    {
      std::string dbPath = dataDir + "/" + SidechainConfig::DATABASE_NAME;
      boost::filesystem::create_directories(dbPath);
      return dbPath;
    }
  }

  SidechainStorage::SidechainStorage(const std::string &dataDir)
      : m_storage(ensureDataDir(dataDir), false, 0)
  {
    if (topBlockHeight() == 0)
    {
      Block genesis;
      genesis.header.height = 0;
      genesis.header.timestamp = SidechainConfig::GENESIS_TIMESTAMP;
      crypto::Hash hash = cn::getObjectHash(genesis);
      genesis.header.blockHash = hash;
      addBlock(genesis, hash);
    }
  }

  SidechainStorage::~SidechainStorage()
  {
    flush();
  }

  bool SidechainStorage::addBlock(const Block &block, const crypto::Hash &hash)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    cn::BinaryArray ba = cn::toBinaryArray(block);
    uint64_t height = block.header.height;
    m_storage.pushBlockEntry(static_cast<uint32_t>(height), ba);
    m_storage.addBlock(cn::Block(), hash, static_cast<uint32_t>(height));
    m_storage.setTopBlockHeight(static_cast<uint32_t>(height));
    m_storage.flush();
    return true;
  }

  bool SidechainStorage::getBlock(uint64_t height, Block &block) const
  {
    cn::BinaryArray ba;
    if (!m_storage.getBlockEntry(static_cast<uint32_t>(height), ba))
      return false;
    return cn::fromBinaryArray(block, ba);
  }

  bool SidechainStorage::getBlock(const crypto::Hash &hash, Block &block) const
  {
    uint32_t height = m_storage.getBlockHeight(hash);
    if (height == 0)
      return false;
    return getBlock(height, block);
  }

  uint64_t SidechainStorage::topBlockHeight() const
  {
    return m_storage.topBlockHeight();
  }

  crypto::Hash SidechainStorage::getBlockHash(uint64_t height) const
  {
    return m_storage.getBlockHash(static_cast<uint32_t>(height));
  }

  // Fingerprint generation
  std::string SidechainStorage::generateFingerprint(
      const std::string &sourceChain,
      const std::string &sourceAsset,
      const crypto::PublicKey &bridgeOperator) const
  {
    std::string input = sourceChain + ":" + sourceAsset + ":" + common::podToHex(bridgeOperator);
    crypto::Hash hash;
    crypto::cn_fast_hash(input.data(), input.size(), hash);
    return common::podToHex(hash);
  }

  // Token operations
  bool SidechainStorage::addToken(const TokenInfo &token)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    cn::BinaryArray ba = cn::toBinaryArray(token);
    std::string key = "token_" + std::to_string(token.id);
    std::cout << "addToken: key=" << key << " id=" << token.id
              << " name=" << token.name << " symbol=" << token.symbol
              << " fingerprint=" << token.fingerprint
              << " model=" << static_cast<int>(token.backingModel)
              << " lockedCCX=" << token.lockedCCXAmount
              << " size=" << ba.size() << std::endl;
    m_storage.putMeta(key, ba);

    // Index by fingerprint if set
    if (!token.fingerprint.empty())
    {
      std::string fpKey = "token_fp_" + token.fingerprint;
      std::vector<uint8_t> fpValue(sizeof(uint64_t));
      uint64_t id = token.id;
      memcpy(fpValue.data(), &id, sizeof(uint64_t));
      m_storage.putMeta(fpKey, fpValue);
    }

    m_storage.flush();
    return true;
  }

  bool SidechainStorage::updateToken(const TokenInfo &token)
  {
    return addToken(token);
  }

  bool SidechainStorage::getToken(uint64_t tokenId, TokenInfo &token) const
  {
    std::string key = "token_" + std::to_string(tokenId);
    std::vector<uint8_t> value;
    if (!m_storage.getMeta(key, value))
      return false;
    return cn::fromBinaryArray(token, value);
  }

  bool SidechainStorage::getTokenByFingerprint(const std::string &fingerprint, TokenInfo &token) const
  {
    std::string fpKey = "token_fp_" + fingerprint;
    std::vector<uint8_t> value;
    if (!m_storage.getMeta(fpKey, value) || value.size() < sizeof(uint64_t))
      return false;
    uint64_t tokenId;
    memcpy(&tokenId, value.data(), sizeof(uint64_t));
    return getToken(tokenId, token);
  }

  std::vector<TokenInfo> SidechainStorage::getAllTokens() const
  {
    std::vector<TokenInfo> result;
    uint64_t id = 1;
    TokenInfo token;
    while (getToken(id, token))
    {
      result.push_back(token);
      ++id;
    }
    return result;
  }

  uint64_t SidechainStorage::nextTokenId() const
  {
    uint64_t id = 1;
    TokenInfo token;
    while (getToken(id, token))
      ++id;
    return id;
  }

  // Rate limiting
  bool SidechainStorage::canCreateToken(const crypto::PublicKey &address, uint64_t currentHeight) const
  {
    std::string key = "create_limit_" + common::podToHex(address);
    std::vector<uint8_t> value;
    if (!m_storage.getMeta(key, value) || value.size() < sizeof(uint64_t))
      return true;

    uint64_t lastCreateHeight;
    memcpy(&lastCreateHeight, value.data(), sizeof(uint64_t));

    return (currentHeight - lastCreateHeight) >= SidechainConfig::TOKEN_CREATE_COOLDOWN_BLOCKS;
  }

  void SidechainStorage::recordTokenCreation(const crypto::PublicKey &address, uint64_t currentHeight)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string key = "create_limit_" + common::podToHex(address);
    std::vector<uint8_t> value(sizeof(uint64_t));
    memcpy(value.data(), &currentHeight, sizeof(uint64_t));
    m_storage.putMeta(key, value);
    m_storage.flush();
  }

  // Asset registry operations
  bool SidechainStorage::registerAsset(const std::string &sourceChain,
                                       const std::string &sourceAsset,
                                       const crypto::PublicKey &bridgeOperator,
                                       uint64_t tokenId,
                                       bool verified)
  {
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    std::string fingerprint = generateFingerprint(sourceChain, sourceAsset, bridgeOperator);
    std::string equivalenceClass = sourceChain + ":" + sourceAsset;

    // Register by compound key: sourceChain + sourceAsset + bridgeOperator
    std::string sourceKey = "asset_src_" + sourceChain + "_" + sourceAsset + "_" + common::podToHex(bridgeOperator);
    std::vector<uint8_t> sourceValue(sizeof(uint64_t));
    memcpy(sourceValue.data(), &tokenId, sizeof(uint64_t));
    m_storage.putMeta(sourceKey, sourceValue);

    // Register by token ID
    std::string tokenKey = "asset_tok_" + std::to_string(tokenId);
    AssetRegistryEntry entry;
    entry.tokenId = tokenId;
    entry.fingerprint = fingerprint;
    entry.sourceChain = sourceChain;
    entry.sourceAsset = sourceAsset;
    entry.bridgeOperator = bridgeOperator;
    entry.equivalenceClass = equivalenceClass;
    entry.verified = verified;
    cn::BinaryArray ba = cn::toBinaryArray(entry);
    m_storage.putMeta(tokenKey, ba);

    // Update the token's fingerprint
    TokenInfo token;
    if (getToken(tokenId, token))
    {
      token.fingerprint = fingerprint;
      updateToken(token);
    }

    // Add to equivalence group
    addToEquivalenceGroup(equivalenceClass, tokenId);

    m_storage.flush();
    return true;
  }

  bool SidechainStorage::getAssetBySource(const std::string &sourceChain,
                                          const std::string &sourceAsset,
                                          const crypto::PublicKey &bridgeOperator,
                                          uint64_t &tokenId) const
  {
    std::string key = "asset_src_" + sourceChain + "_" + sourceAsset + "_" + common::podToHex(bridgeOperator);
    std::vector<uint8_t> value;
    if (!m_storage.getMeta(key, value) || value.size() < sizeof(uint64_t))
      return false;
    memcpy(&tokenId, value.data(), sizeof(uint64_t));
    return true;
  }

  bool SidechainStorage::getAssetByTokenId(uint64_t tokenId,
                                           AssetRegistryEntry &entry) const
  {
    std::string key = "asset_tok_" + std::to_string(tokenId);
    std::vector<uint8_t> value;
    if (!m_storage.getMeta(key, value))
      return false;
    return cn::fromBinaryArray(entry, value);
  }

  std::vector<AssetRegistryEntry> SidechainStorage::getAllAssets() const
  {
    std::vector<AssetRegistryEntry> result;
    std::vector<TokenInfo> tokens = getAllTokens();
    for (const auto &token : tokens)
    {
      AssetRegistryEntry entry;
      if (getAssetByTokenId(token.id, entry))
        result.push_back(entry);
    }
    return result;
  }

  // Equivalence group operations
  std::string SidechainStorage::getEquivalenceClass(const std::string &sourceChain,
                                                    const std::string &sourceAsset) const
  {
    return sourceChain + ":" + sourceAsset;
  }

  std::vector<uint64_t> SidechainStorage::getTokensByEquivalenceClass(const std::string &equivalenceClass) const
  {
    std::vector<uint64_t> result;
    std::string key = "equiv_" + equivalenceClass;
    std::vector<uint8_t> value;
    if (!m_storage.getMeta(key, value))
      return result;

    // Format: count (4 bytes) followed by token IDs (8 bytes each)
    if (value.size() < sizeof(uint32_t))
      return result;

    uint32_t count;
    memcpy(&count, value.data(), sizeof(uint32_t));
    const uint8_t *ptr = value.data() + sizeof(uint32_t);

    for (uint32_t i = 0; i < count && (ptr + sizeof(uint64_t)) <= (value.data() + value.size()); ++i)
    {
      uint64_t tokenId;
      memcpy(&tokenId, ptr, sizeof(uint64_t));
      ptr += sizeof(uint64_t);
      result.push_back(tokenId);
    }
    return result;
  }

  bool SidechainStorage::addToEquivalenceGroup(const std::string &equivalenceClass, uint64_t tokenId)
  {
    std::lock_guard<std::recursive_mutex> guard(m_mutex);
    std::string key = "equiv_" + equivalenceClass;

    auto existing = getTokensByEquivalenceClass(equivalenceClass);

    // Check if already in group
    for (uint64_t existingId : existing)
    {
      if (existingId == tokenId)
        return true;
    }

    existing.push_back(tokenId);

    std::vector<uint8_t> value;
    uint32_t count = static_cast<uint32_t>(existing.size());
    value.insert(value.end(), (uint8_t *)&count, (uint8_t *)&count + sizeof(count));
    for (uint64_t id : existing)
    {
      value.insert(value.end(), (uint8_t *)&id, (uint8_t *)&id + sizeof(id));
    }

    m_storage.putMeta(key, value);
    m_storage.flush();
    return true;
  }

  // Bridge operations
  bool SidechainStorage::addBridgeLock(const crypto::PublicKey &userAddress,
                                       uint64_t tokenId,
                                       uint64_t amount,
                                       const crypto::Hash &mainChainTxHash,
                                       uint64_t mainChainBlockHeight)
  {
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    BridgeLockEntry entry;
    entry.id = nextBridgeLockId();
    entry.userAddress = userAddress;
    entry.tokenId = tokenId;
    entry.amount = amount;
    entry.mainChainTxHash = mainChainTxHash;
    entry.mainChainBlockHeight = mainChainBlockHeight;
    entry.unlocked = false;

    cn::BinaryArray ba = cn::toBinaryArray(entry);
    std::string key = "bridge_lock_" + std::to_string(entry.id);
    m_storage.putMeta(key, ba);
    m_storage.flush();
    return true;
  }

  bool SidechainStorage::getBridgeLock(uint64_t lockId, BridgeLockEntry &lock) const
  {
    std::string key = "bridge_lock_" + std::to_string(lockId);
    std::vector<uint8_t> value;
    if (!m_storage.getMeta(key, value))
      return false;
    return cn::fromBinaryArray(lock, value);
  }

  bool SidechainStorage::markBridgeLockUnlocked(uint64_t lockId)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    BridgeLockEntry entry;
    if (!getBridgeLock(lockId, entry))
      return false;
    entry.unlocked = true;
    cn::BinaryArray ba = cn::toBinaryArray(entry);
    std::string key = "bridge_lock_" + std::to_string(lockId);
    m_storage.putMeta(key, ba);
    m_storage.flush();
    return true;
  }

  std::vector<BridgeLockEntry> SidechainStorage::getPendingUnlocks() const
  {
    std::vector<BridgeLockEntry> result;
    uint64_t id = 1;
    while (true)
    {
      BridgeLockEntry entry;
      if (!getBridgeLock(id, entry))
        break;
      if (!entry.unlocked)
        result.push_back(entry);
      ++id;
    }
    return result;
  }

  uint64_t SidechainStorage::getTotalLockedForToken(uint64_t tokenId) const
  {
    uint64_t total = 0;
    uint64_t id = 1;
    while (true)
    {
      BridgeLockEntry entry;
      if (!getBridgeLock(id, entry))
        break;
      if (!entry.unlocked && entry.tokenId == tokenId)
        total += entry.amount;
      ++id;
    }
    return total;
  }

  uint64_t SidechainStorage::nextBridgeLockId() const
  {
    uint64_t id = 1;
    BridgeLockEntry entry;
    while (getBridgeLock(id, entry))
      ++id;
    return id;
  }

  // Account operations
  bool SidechainStorage::getBalance(const crypto::PublicKey &address, uint64_t tokenId, uint64_t &balance) const
  {
    std::string key = "bal_" + common::podToHex(address) + "_" + std::to_string(tokenId);
    std::vector<uint8_t> value;
    if (!m_storage.getMeta(key, value) || value.size() < sizeof(uint64_t))
    {
      balance = 0;
      return true;
    }
    memcpy(&balance, value.data(), sizeof(uint64_t));
    return true;
  }

  bool SidechainStorage::setBalance(const crypto::PublicKey &address, uint64_t tokenId, uint64_t balance)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string key = "bal_" + common::podToHex(address) + "_" + std::to_string(tokenId);
    std::vector<uint8_t> value(sizeof(uint64_t));
    memcpy(value.data(), &balance, sizeof(uint64_t));
    m_storage.putMeta(key, value);
    m_storage.flush();
    return true;
  }

  bool SidechainStorage::applyTransaction(const Transaction &tx)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    uint64_t fromBalance, toBalance;
    TokenInfo token;

    switch (tx.type)
    {
    case TransactionType::CreateToken:
    {
      std::string extraStr(tx.extra.begin(), tx.extra.end());
      std::cout << "applyTransaction CreateToken: extra=" << extraStr << std::endl;

      size_t firstColon = extraStr.find(':');
      size_t secondColon = extraStr.find(':', firstColon + 1);

      if (firstColon == std::string::npos || secondColon == std::string::npos)
      {
        std::cout << "applyTransaction CreateToken: PARSE FAILED" << std::endl;
        return false;
      }

      std::string name = extraStr.substr(0, firstColon);
      std::string symbol = extraStr.substr(firstColon + 1, secondColon - firstColon - 1);

      size_t thirdColon = extraStr.find(':', secondColon + 1);
      size_t fourthColon = std::string::npos;
      uint8_t backingModel = 0;
      uint8_t decimals = 6;
      uint64_t backingRatio = 0;
      uint64_t lockedCCXAmount = 0;

      if (thirdColon != std::string::npos)
      {
        backingModel = static_cast<uint8_t>(std::stoi(extraStr.substr(secondColon + 1, thirdColon - secondColon - 1)));
        fourthColon = extraStr.find(':', thirdColon + 1);

        if (fourthColon != std::string::npos)
        {
          decimals = static_cast<uint8_t>(std::stoi(extraStr.substr(thirdColon + 1, fourthColon - thirdColon - 1)));

          size_t fifthColon = extraStr.find(':', fourthColon + 1);
          if (fifthColon != std::string::npos)
          {
            backingRatio = std::stoull(extraStr.substr(fourthColon + 1, fifthColon - fourthColon - 1));

            size_t sixthColon = extraStr.find(':', fifthColon + 1);
            if (sixthColon != std::string::npos)
            {
              lockedCCXAmount = std::stoull(extraStr.substr(fifthColon + 1, sixthColon - fifthColon - 1));
            }
            else
            {
              lockedCCXAmount = std::stoull(extraStr.substr(fifthColon + 1));
            }
          }
          else
          {
            backingRatio = std::stoull(extraStr.substr(fourthColon + 1));
          }
        }
        else
        {
          decimals = static_cast<uint8_t>(std::stoi(extraStr.substr(thirdColon + 1)));
        }
      }
      else
      {
        backingModel = static_cast<uint8_t>(std::stoi(extraStr.substr(secondColon + 1)));
      }

      // Validate backing requirements
      if (backingModel == static_cast<uint8_t>(TokenBackingModel::Backed) ||
          backingModel == static_cast<uint8_t>(TokenBackingModel::Hybrid))
      {
        if (lockedCCXAmount == 0)
        {
          std::cout << "applyTransaction CreateToken: backed/hybrid token requires locked CCX" << std::endl;
          return false;
        }
        if (backingRatio == 0)
          backingRatio = SidechainConfig::DEFAULT_BACKING_RATIO;
      }

      TokenInfo newToken;
      newToken.id = nextTokenId();
      newToken.name = name;
      newToken.symbol = symbol;
      newToken.totalSupply = tx.amount;
      newToken.maxSupply = tx.amount * 10;
      newToken.decimals = decimals;
      newToken.backingModel = static_cast<TokenBackingModel>(backingModel);
      newToken.backingRatio = backingRatio;
      newToken.lockedCCXAmount = lockedCCXAmount;

      if (tx.txHash != crypto::Hash())
        newToken.backingTxHash = tx.txHash;

      addToken(newToken);
      setBalance(tx.from, newToken.id, tx.amount);

      if (backingModel == static_cast<uint8_t>(TokenBackingModel::Backed) ||
          backingModel == static_cast<uint8_t>(TokenBackingModel::Hybrid))
      {
        // User-created backed tokens use their own public key as bridge operator
        registerAsset("conceal", "native", tx.from, newToken.id, false);
      }

      std::cout << "applyTransaction CreateToken: token created successfully id=" << newToken.id << std::endl;
      break;
    }

    case TransactionType::Transfer:
    {
      getBalance(tx.from, tx.tokenId, fromBalance);
      getBalance(tx.to, tx.tokenId, toBalance);

      if (fromBalance < tx.amount + tx.fee)
        return false;

      setBalance(tx.from, tx.tokenId, fromBalance - tx.amount - tx.fee);
      setBalance(tx.to, tx.tokenId, toBalance + tx.amount);
      break;
    }

    case TransactionType::Mint:
    {
      std::string extraStr(tx.extra.begin(), tx.extra.end());
      bool isNewAsset = (extraStr.find(":new_asset") != std::string::npos);

      // Use a local token ID that can be overridden for auto-creation
      uint64_t mintTokenId = tx.tokenId;

      if (isNewAsset && mintTokenId == 0)
      {
        // Auto-create canonical CCX-backed token
        TokenInfo newToken;
        newToken.id = nextTokenId();
        newToken.name = "CCX (Bridged)";
        newToken.symbol = "bCCX";
        newToken.totalSupply = 0;
        newToken.maxSupply = 0;
        newToken.decimals = 6;
        newToken.backingModel = TokenBackingModel::Backed;
        newToken.backingRatio = 100;
        newToken.lockedCCXAmount = 0;

        // Generate fingerprint: conceal:native:bridgeOperatorPubKey
        newToken.fingerprint = generateFingerprint("conceal", "native", tx.from);

        addToken(newToken);

        // Register in asset registry with bridge operator key
        registerAsset("conceal", "native", tx.from, newToken.id, true);

        mintTokenId = newToken.id;
        token = newToken;

        std::cout << "applyTransaction Mint: auto-created canonical CCX token id="
                  << newToken.id << " fingerprint=" << newToken.fingerprint << std::endl;
      }
      else if (!getToken(mintTokenId, token))
      {
        std::cout << "applyTransaction Mint: token not found id=" << mintTokenId << std::endl;
        return false;
      }

      if (token.backingModel != TokenBackingModel::Backed &&
          token.backingModel != TokenBackingModel::Hybrid)
      {
        std::cout << "applyTransaction Mint: token is not backed/hybrid" << std::endl;
        return false;
      }

      token.totalSupply += tx.amount;
      token.lockedCCXAmount += tx.amount;

      addBridgeLock(tx.to, mintTokenId, tx.amount, tx.txHash, 0);

      updateToken(token);

      getBalance(tx.to, mintTokenId, toBalance);
      setBalance(tx.to, mintTokenId, toBalance + tx.amount);

      std::cout << "applyTransaction Mint: minted " << tx.amount
                << " of token " << mintTokenId
                << " new supply=" << token.totalSupply
                << " lockedCCX=" << token.lockedCCXAmount << std::endl;
      break;
    }

    case TransactionType::Burn:
    {
      if (!getToken(tx.tokenId, token))
      {
        std::cout << "applyTransaction Burn: token not found id=" << tx.tokenId << std::endl;
        return false;
      }

      getBalance(tx.from, tx.tokenId, fromBalance);

      if (fromBalance < tx.amount + tx.fee)
      {
        std::cout << "applyTransaction Burn: insufficient balance" << std::endl;
        return false;
      }

      if (token.totalSupply < tx.amount)
      {
        std::cout << "applyTransaction Burn: amount exceeds supply" << std::endl;
        return false;
      }

      token.totalSupply -= tx.amount;
      updateToken(token);
      setBalance(tx.from, tx.tokenId, fromBalance - tx.amount - tx.fee);

      if (token.backingModel == TokenBackingModel::Backed ||
          token.backingModel == TokenBackingModel::Hybrid)
      {
        if (token.lockedCCXAmount >= tx.amount)
          token.lockedCCXAmount -= tx.amount;
        else
          token.lockedCCXAmount = 0;
        updateToken(token);

        uint64_t remainingToUnlock = tx.amount;
        uint64_t lockId = 1;
        BridgeLockEntry lockEntry;
        while (remainingToUnlock > 0 && getBridgeLock(lockId, lockEntry))
        {
          if (!lockEntry.unlocked && lockEntry.tokenId == tx.tokenId &&
              lockEntry.userAddress == tx.from)
          {
            uint64_t unlockAmount = std::min(remainingToUnlock, lockEntry.amount);
            if (unlockAmount == lockEntry.amount)
              markBridgeLockUnlocked(lockId);
            remainingToUnlock -= unlockAmount;
          }
          ++lockId;
        }

        std::cout << "applyTransaction Burn: burned " << tx.amount
                  << " of token " << tx.tokenId
                  << " remaining supply=" << token.totalSupply
                  << " queued CCX unlock for " << common::podToHex(tx.from).substr(0, 16) << std::endl;
      }

      break;
    }

    default:
      break;
    }

    if (tx.fee > 0)
    {
      getBalance(tx.from, 0, fromBalance);
      if (fromBalance < tx.fee)
        return false;
      setBalance(tx.from, 0, fromBalance - tx.fee);
    }

    m_storage.flush();
    return true;
  }

  // Validator operations
  bool SidechainStorage::addValidator(const ValidatorInfo &validator)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    cn::BinaryArray ba = cn::toBinaryArray(validator);
    std::string key = "validator_" + std::to_string(validator.id);
    m_storage.putMeta(key, ba);
    m_storage.flush();
    return true;
  }

  std::vector<ValidatorInfo> SidechainStorage::getActiveValidators() const
  {
    std::vector<ValidatorInfo> result;
    uint32_t id = 0;
    while (true)
    {
      std::string key = "validator_" + std::to_string(id);
      std::vector<uint8_t> value;
      if (!m_storage.getMeta(key, value))
        break;
      ValidatorInfo validator;
      if (cn::fromBinaryArray(validator, value) && validator.active)
        result.push_back(validator);
      ++id;
    }
    return result;
  }

  void SidechainStorage::flush()
  {
    m_storage.flush();
  }

  bool SidechainStorage::getMeta(const std::string &key, std::vector<uint8_t> &value) const
  {
    return m_storage.getMeta(key, value);
  }

  void SidechainStorage::putMeta(const std::string &key, const std::vector<uint8_t> &value)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_storage.putMeta(key, value);
    m_storage.flush();
  }
}