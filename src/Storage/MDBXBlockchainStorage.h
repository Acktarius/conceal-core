// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "IBlockchainStorage.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"

#include <mdbx.h>
#include <mutex>
#include <string>
#include <vector>

namespace CryptoNote
{
  class MDBXBlockchainStorage : public IBlockchainStorage
  {
  public:
    explicit MDBXBlockchainStorage(const std::string &dataDir, bool bulkSyncMode = false, uint64_t sizeLimitBytes = 0);
    ~MDBXBlockchainStorage() override;

    // ──────────────────────────────────────────────
    // Block existence and retrieval
    // ──────────────────────────────────────────────
    bool blockExists(const crypto::Hash &hash) const override;
    bool getBlock(const crypto::Hash &hash, cn::Block &block) const override;
    uint32_t getBlockHeight(const crypto::Hash &hash) const override;
    crypto::Hash getBlockHash(uint32_t height) const override;

    // ──────────────────────────────────────────────
    // Atomic block write
    // ──────────────────────────────────────────────
    void pushCompleteBlock(uint32_t height,
                           const crypto::Hash &hash,
                           const cn::BinaryArray &serializedEntry,
                           const cn::BlockHeaderPOD &hdr) override;

    // ──────────────────────────────────────────────
    // Legacy single-operation writes (reorg support)
    // ──────────────────────────────────────────────
    void addBlock(const cn::Block &block, const crypto::Hash &hash, uint32_t height) override;
    void removeBlock(const crypto::Hash &hash) override;

    // ──────────────────────────────────────────────
    // Atomic block removal
    // ──────────────────────────────────────────────
    void removeCompleteBlock(uint32_t height, const crypto::Hash &hash) override;

    // ──────────────────────────────────────────────
    // Header management
    // ──────────────────────────────────────────────
    void pushBlockHeader(uint32_t height, const cn::BlockHeaderPOD &hdr) override;
    bool getBlockHeader(uint32_t height, cn::BlockHeaderPOD &hdr) const override;
    void getBlockHeadersRange(uint32_t startHeight, uint32_t count,
                              std::vector<cn::BlockHeaderPOD> &out) const override;
    void removeBlockHeader(uint32_t height);

    // ──────────────────────────────────────────────
    // Block entry management
    // ──────────────────────────────────────────────
    void pushBlockEntry(uint32_t height, const cn::BinaryArray &serializedEntry) override;
    bool getBlockEntry(uint32_t height, cn::BinaryArray &serializedEntry) const override;
    void popBlockEntry(uint32_t height) override;

    // ──────────────────────────────────────────────
    // Transaction storage
    // ──────────────────────────────────────────────
    void pushTransaction(const crypto::Hash &txHash, const cn::BinaryArray &serializedTx) override;
    bool getTransaction(const crypto::Hash &txHash, cn::Transaction &tx) const override;
    bool transactionExists(const crypto::Hash &txHash) const override;
    void removeTransaction(const crypto::Hash &txHash) override;

    // ──────────────────────────────────────────────
    // Transaction pool persistence
    // ──────────────────────────────────────────────
    void storePoolState(const std::vector<cn::BinaryArray> &serializedTxs,
                        const std::vector<crypto::KeyImage> &spentKeyImages) override;
    std::vector<cn::BinaryArray> loadPoolTransactions() const override;
    std::vector<crypto::KeyImage> loadPoolSpentKeyImages() const override;

    // ──────────────────────────────────────────────
    // Spent key images (double-spend protection)
    // ──────────────────────────────────────────────
    bool isSpentKeyImage(const crypto::KeyImage &keyImage) const override;
    void markKeyImageSpent(const crypto::KeyImage &keyImage) override;
    void markKeyImagesSpent(const std::vector<crypto::KeyImage> &keyImages) override;
    bool areAllKeyImagesUnspent(const std::vector<crypto::KeyImage> &keyImages) const override;

    // ──────────────────────────────────────────────
    // Timestamp index
    // ──────────────────────────────────────────────
    void addTimestampIndex(uint64_t timestamp, uint32_t height) override;
    uint32_t getBlockHeightByTimestamp(uint64_t timestamp) const override;
    void removeTimestampIndex(uint64_t timestamp, uint32_t height) override;

    // ──────────────────────────────────────────────
    // Global state
    // ──────────────────────────────────────────────
    uint32_t topBlockHeight() const override;
    void setTopBlockHeight(uint32_t height) override;

    // ──────────────────────────────────────────────
    // Cumulative difficulty
    // ──────────────────────────────────────────────
    void setCumulativeDifficulty(uint32_t height, uint64_t cumulativeDifficulty) override;
    uint64_t getCumulativeDifficulty(uint32_t height) const override;
    void removeCumulativeDifficulty(uint32_t height) override;
    uint64_t getCurrentCumulativeDifficulty() const override;

    // ──────────────────────────────────────────────
    // Persistence & lifecycle
    // ──────────────────────────────────────────────
    void flush() override;
    void close() override;

    // ──────────────────────────────────────────────
    // Generic metadata store
    // ──────────────────────────────────────────────
    void putMeta(const std::string &key, const std::vector<uint8_t> &value) override;
    bool getMeta(const std::string &key, std::vector<uint8_t> &value) const override;
    void removeMeta(const std::string &key) override;

    // ──────────────────────────────────────────────
    // Initialisation flag
    // ──────────────────────────────────────────────
    void setInitialized() override;
    bool isInitialized() const override;

    // ──────────────────────────────────────────────
    // Checkpoint storage
    // ──────────────────────────────────────────────
    void storeCheckpoint(uint32_t height, const crypto::Hash &hash) override;
    std::vector<std::pair<uint32_t, crypto::Hash>> getCheckpoints() const override;
    void removeCheckpointsAbove(uint32_t height) override;

    // ──────────────────────────────────────────────
    // Error handling & diagnostics
    // ──────────────────────────────────────────────
    void abortWriteTxn();
    std::string printDatabaseStats() const;

    // ──────────────────────────────────────────────
    // Migration
    // ──────────────────────────────────────────────
    void migrateToPaddedKeys() override;

  private:
    // Environment & database setup
    void openEnvironment(const std::string &path);
    void openDatabases(MDBX_txn *txn);

    // Write transaction management
    void commitWriteTransaction(bool force = false);
    void ensureWriteTxn();
    void setTopBlockHeightInternal(uint32_t height);

    // Zero-padded key helpers
    static std::string blockEntryKey(uint32_t height);
    static std::string blockHeaderKey(uint32_t height);
    static std::string checkpointKey(uint32_t height);
    static std::string timestampKey(uint64_t timestamp, uint32_t height);
    static std::string difficultyKey(uint32_t height);

    static constexpr int kHeightKeyWidth = 8;
    static constexpr int kTimestampKeyWidth = 20; // Supports timestamps up to ~10^20 (year 5138)
    static constexpr int kDifficultyKeyWidth = 8;

    // Safe MDBX_val constructors
    static MDBX_val to_val(const void *data, size_t len);
    static MDBX_val to_val(const std::string &s);
    static MDBX_val to_val(const crypto::Hash &h);
    static MDBX_val to_val(const cn::BlockHeaderPOD &hdr);

    // MDBX handles
    MDBX_env *m_env = nullptr;
    MDBX_dbi m_dbiHeights;
    MDBX_dbi m_dbiBlockHeights;
    MDBX_dbi m_dbiMeta;
    MDBX_dbi m_dbiBlockEntries;
    MDBX_dbi m_dbiBlockHeaders;
    MDBX_dbi m_dbiCheckpoints;
    MDBX_dbi m_dbiTransactions;   // tx_hash → serialized transaction
    MDBX_dbi m_dbiSpentKeyImages; // key_image → (empty value, presence = spent)
    MDBX_dbi m_dbiTimestampIndex; // "ts_<padded_timestamp>_<padded_height>" → height
    MDBX_dbi m_dbiDifficulty;     // "diff_<padded_height>" → uint64_t

    // Thread safety & write batching
    mutable std::mutex m_txMutex;
    MDBX_txn *m_writeTxn = nullptr;
    size_t m_opsSinceLastCommit = 0;

    static constexpr size_t kCommitBatchSize = 1000;
    static constexpr size_t kCommitBatchSizeBulk = 50000;

    // In-memory cache
    uint32_t m_cachedTopHeight = 0;

    // Configuration
    std::string m_dataDir;
    bool m_bulkSyncMode = false;
    uint64_t m_sizeLimitBytes = 0;
  };
} // namespace CryptoNote