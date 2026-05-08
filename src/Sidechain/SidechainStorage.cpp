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

  // Token operations
  bool SidechainStorage::addToken(const TokenInfo &token)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    cn::BinaryArray ba = cn::toBinaryArray(token);
    std::string key = "token_" + std::to_string(token.id);
    std::cout << "addToken: key=" << key << " id=" << token.id << " name=" << token.name << " size=" << ba.size() << std::endl;
    m_storage.putMeta(key, ba);
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

  std::vector<TokenInfo> SidechainStorage::getAllTokens() const
  {
    std::vector<TokenInfo> result;
    uint64_t id = 1; // tokens start at 1 (0 is native SCCX)
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

  // Rate limiting for token creation
  bool SidechainStorage::canCreateToken(const crypto::PublicKey &address, uint64_t currentHeight) const
  {
    std::string key = "create_limit_" + common::podToHex(address);
    std::vector<uint8_t> value;
    if (!m_storage.getMeta(key, value) || value.size() < sizeof(uint64_t))
      return true; // never created before, allowed

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

    switch (tx.type)
    {
    case TransactionType::CreateToken:
    {
      std::string extraStr(tx.extra.begin(), tx.extra.end());
      std::cout << "applyTransaction CreateToken: extra=" << extraStr << std::endl;

      size_t firstColon = extraStr.find(':');
      size_t secondColon = extraStr.find(':', firstColon + 1);

      std::cout << "applyTransaction CreateToken: firstColon=" << firstColon
                << " secondColon=" << secondColon << std::endl;

      if (firstColon != std::string::npos && secondColon != std::string::npos)
      {
        std::string name = extraStr.substr(0, firstColon);
        std::string symbol = extraStr.substr(firstColon + 1, secondColon - firstColon - 1);

        size_t thirdColon = extraStr.find(':', secondColon + 1);
        uint8_t backingModel = 0;
        uint8_t decimals = 6;

        if (thirdColon != std::string::npos)
        {
          backingModel = std::stoi(extraStr.substr(secondColon + 1, thirdColon - secondColon - 1));
          decimals = std::stoi(extraStr.substr(thirdColon + 1));
        }
        else
        {
          // Old format: only 3 fields (backward compatible)
          backingModel = std::stoi(extraStr.substr(secondColon + 1));
        }

        std::cout << "applyTransaction CreateToken: name=" << name
                  << " symbol=" << symbol << " model=" << (int)backingModel
                  << " decimals=" << (int)decimals << std::endl;

        TokenInfo newToken;
        newToken.id = nextTokenId();
        newToken.name = name;
        newToken.symbol = symbol;
        newToken.totalSupply = tx.amount;
        newToken.maxSupply = tx.amount * 10;
        newToken.decimals = decimals;
        newToken.backingModel = static_cast<TokenBackingModel>(backingModel);
        addToken(newToken);
        setBalance(tx.from, newToken.id, tx.amount);

        std::cout << "applyTransaction CreateToken: token created successfully" << std::endl;
      }
      else
      {
        std::cout << "applyTransaction CreateToken: PARSE FAILED" << std::endl;
      }
      break;
    }
    case TransactionType::Transfer:
    case TransactionType::Mint:
    case TransactionType::Burn:
      getBalance(tx.from, tx.tokenId, fromBalance);
      getBalance(tx.to, tx.tokenId, toBalance);

      if (tx.type == TransactionType::Burn)
      {
        if (fromBalance < tx.amount + tx.fee)
          return false;
        setBalance(tx.from, tx.tokenId, fromBalance - tx.amount - tx.fee);
      }
      else
      {
        if (fromBalance < tx.amount + tx.fee)
          return false;
        setBalance(tx.from, tx.tokenId, fromBalance - tx.amount - tx.fee);
        setBalance(tx.to, tx.tokenId, toBalance + tx.amount);
      }
      break;

    default:
      break;
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