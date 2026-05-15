// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstdint>
#include <vector>
#include "crypto/hash.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"

namespace CryptoNote
{

  class IBlockchainStorage
  {
  public:
    virtual ~IBlockchainStorage() = default;

    // Block-level access
    virtual bool blockExists(const crypto::Hash &hash) const = 0;
    virtual bool getBlock(const crypto::Hash &hash, cn::Block &block) const = 0;
    virtual uint32_t getBlockHeight(const crypto::Hash &hash) const = 0;
    virtual crypto::Hash getBlockHash(uint32_t height) const = 0;

    // Atomic complete block write
    virtual void pushCompleteBlock(uint32_t height,
                                   const crypto::Hash &hash,
                                   const cn::BinaryArray &serializedEntry,
                                   const cn::BlockHeaderPOD &hdr) = 0;

    // Atomic complete block removal (reorg cleanup)
    virtual void removeCompleteBlock(uint32_t height, const crypto::Hash &hash) = 0;

    // Batch write operations (legacy — still used during reorgs)
    virtual void addBlock(const cn::Block &block, const crypto::Hash &hash, uint32_t height) = 0;
    virtual void removeBlock(const crypto::Hash &hash) = 0;

    // Block header retrieval
    virtual void getBlockHeadersRange(uint32_t startHeight, uint32_t count,
                                      std::vector<cn::BlockHeaderPOD> &out) const = 0;
    virtual bool getBlockHeader(uint32_t height, cn::BlockHeaderPOD &hdr) const = 0;
    virtual void pushBlockHeader(uint32_t height, const cn::BlockHeaderPOD &hdr) = 0;

    // Block entry storage
    virtual void pushBlockEntry(uint32_t height, const cn::BinaryArray &serializedEntry) = 0;
    virtual bool getBlockEntry(uint32_t height, cn::BinaryArray &serializedEntry) const = 0;
    virtual void popBlockEntry(uint32_t height) = 0;

    // Transaction storage
    virtual void pushTransaction(const crypto::Hash &txHash, const cn::BinaryArray &serializedTx) = 0;
    virtual bool getTransaction(const crypto::Hash &txHash, cn::Transaction &tx) const = 0;
    virtual bool transactionExists(const crypto::Hash &txHash) const = 0;
    virtual void removeTransaction(const crypto::Hash &txHash) = 0;

    // Transaction pool persistence
    virtual void storePoolState(const std::vector<cn::BinaryArray> &serializedTxs,
                                const std::vector<crypto::KeyImage> &spentKeyImages) = 0;
    virtual std::vector<cn::BinaryArray> loadPoolTransactions() const = 0;
    virtual std::vector<crypto::KeyImage> loadPoolSpentKeyImages() const = 0;

    // Spent key images (double-spend protection)
    virtual bool isSpentKeyImage(const crypto::KeyImage &keyImage) const = 0;
    virtual void markKeyImageSpent(const crypto::KeyImage &keyImage) = 0;
    virtual void markKeyImagesSpent(const std::vector<crypto::KeyImage> &keyImages) = 0;
    virtual bool areAllKeyImagesUnspent(const std::vector<crypto::KeyImage> &keyImages) const = 0;

    // Block index by timestamp
    virtual void addTimestampIndex(uint64_t timestamp, uint32_t height) = 0;
    virtual uint32_t getBlockHeightByTimestamp(uint64_t timestamp) const = 0;
    virtual void removeTimestampIndex(uint64_t timestamp, uint32_t height) = 0;

    // Global state
    virtual uint32_t topBlockHeight() const = 0;
    virtual void setTopBlockHeight(uint32_t height) = 0;

    // Cumulative difficulty
    virtual void setCumulativeDifficulty(uint32_t height, uint64_t cumulativeDifficulty) = 0;
    virtual uint64_t getCumulativeDifficulty(uint32_t height) const = 0;
    virtual void removeCumulativeDifficulty(uint32_t height) = 0;
    virtual uint64_t getCurrentCumulativeDifficulty() const = 0;

    // Persistence & lifecycle
    virtual void flush() = 0;
    virtual void close() = 0;

    // Generic metadata store
    virtual void putMeta(const std::string &key, const std::vector<uint8_t> &value) = 0;
    virtual bool getMeta(const std::string &key, std::vector<uint8_t> &value) const = 0;
    virtual void removeMeta(const std::string &key) = 0;

    // Initialisation flag
    virtual void setInitialized() = 0;
    virtual bool isInitialized() const = 0;

    // Checkpoint storage
    virtual void storeCheckpoint(uint32_t height, const crypto::Hash &hash) = 0;
    virtual std::vector<std::pair<uint32_t, crypto::Hash>> getCheckpoints() const = 0;
    virtual void removeCheckpointsAbove(uint32_t height) = 0;

    // Migration
    virtual void migrateToPaddedKeys() = 0;
  };

} // namespace CryptoNote