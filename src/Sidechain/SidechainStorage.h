// SidechainStorage.h — MDBX-backed block and state storage
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "SidechainTypes.h"
#include "Storage/MDBXBlockchainStorage.h"
#include <vector>
#include <mutex>

namespace Sidechain
{
  class SidechainStorage
  {
  public:
    explicit SidechainStorage(const std::string &dataDir);
    ~SidechainStorage();

    // Block operations
    bool addBlock(const Block &block, const crypto::Hash &hash);
    bool getBlock(uint64_t height, Block &block) const;
    bool getBlock(const crypto::Hash &hash, Block &block) const;
    uint64_t topBlockHeight() const;
    crypto::Hash getBlockHash(uint64_t height) const;

    // Token operations
    bool addToken(const TokenInfo &token);
    bool updateToken(const TokenInfo &token);
    bool getToken(uint64_t tokenId, TokenInfo &token) const;
    std::vector<TokenInfo> getAllTokens() const;
    uint64_t nextTokenId() const;

    // Rate limiting for token creation
    bool canCreateToken(const crypto::PublicKey &address, uint64_t currentHeight) const;
    void recordTokenCreation(const crypto::PublicKey &address, uint64_t currentHeight);

    // Account operations
    bool getBalance(const crypto::PublicKey &address, uint64_t tokenId, uint64_t &balance) const;
    bool setBalance(const crypto::PublicKey &address, uint64_t tokenId, uint64_t balance);
    bool applyTransaction(const Transaction &tx);

    // Validator operations
    bool addValidator(const ValidatorInfo &validator);
    std::vector<ValidatorInfo> getActiveValidators() const;

    // Meta
    void flush();
    bool getMeta(const std::string &key, std::vector<uint8_t> &value) const;
    void putMeta(const std::string &key, const std::vector<uint8_t> &value);

  private:
    CryptoNote::MDBXBlockchainStorage m_storage;
    mutable std::recursive_mutex m_mutex;
  };
}