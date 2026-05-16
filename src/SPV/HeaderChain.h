// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <map>
#include "crypto/hash.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"

namespace SPV
{
  struct BlockHeader
  {
    uint32_t height;
    crypto::Hash hash;
    crypto::Hash prev_hash;
    uint64_t timestamp;
    uint32_t nonce;
    uint64_t difficulty;
    crypto::Hash merkle_root;
    uint8_t major_version;
    uint8_t minor_version;
  };

  struct TransactionProof
  {
    crypto::Hash tx_hash;
    uint32_t block_height;
    crypto::Hash block_hash;
    crypto::Hash merkle_root;
    uint32_t tx_index;
    std::vector<crypto::Hash> merkle_branch;
    bool verified;
  };

  class HeaderChain
  {
  public:
    HeaderChain();
    ~HeaderChain();

    // Header chain management
    bool addHeader(const BlockHeader &header);
    bool getHeader(uint32_t height, BlockHeader &header) const;
    uint32_t getHeight() const { return m_height; }

    // Persistence
    bool load(const std::string &filename);
    bool save(const std::string &filename);
    void setFilename(const std::string &filename) { m_filename = filename; }
    const std::string &getFilename() const { return m_filename; }

    // SPV verification
    bool verifyTransaction(const TransactionProof &proof) const;

    // Checkpoint verification
    bool verifyCheckpoint(uint32_t height, const crypto::Hash &expectedHash);
    bool verifyAllCheckpoints();

  private:
    std::vector<BlockHeader> m_headers;
    uint32_t m_height;
    std::string m_filename;

    static std::map<uint32_t, crypto::Hash> getCheckpoints();
  };
}