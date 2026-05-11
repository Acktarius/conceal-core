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

    // Block existence and retrieval
    bool blockExists(const crypto::Hash &hash) const override;
    // Now uses a single read transaction internally
    bool getBlock(const crypto::Hash &hash, cn::Block &block) const override;
    uint32_t getBlockHeight(const crypto::Hash &hash) const override;
    crypto::Hash getBlockHash(uint32_t height) const override;

    // Atomic block write (replaces addBlock for new blocks)
    // Stores the serialized block entry, header, bi‑directional height mapping, and updates top
    // height in one explicit MDBX transaction - no half‑written blocks
    void pushCompleteBlock(uint32_t height,
                           const crypto::Hash &hash,
                           const cn::BinaryArray &serializedEntry,
                           const cn::BlockHeaderPOD &hdr);

    // Legacy single‑operation writes (still used for reorgs)
    void addBlock(const cn::Block &block, const crypto::Hash &hash, uint32_t height) override;
    void removeBlock(const crypto::Hash &hash) override;

    // Atomic block removal (header + entry + height mapping + top height decrement)
    void removeCompleteBlock(uint32_t height, const crypto::Hash &hash);

    // Header helpers
    // Called during reorg cleanup
    void removeBlockHeader(uint32_t height);
    // Removes both block entry and header atomically
    void cleanupBlockData(uint32_t height);
    // Deprecated in favour of pushCompleteBlock; kept for legacy
    void pushBlockHeader(uint32_t height, const cn::BlockHeaderPOD &hdr);
    // Single read txn fetch
    bool getBlockHeader(uint32_t height, cn::BlockHeaderPOD &hdr) const;

    // Batch header retrieval (avoids N separate read txns during sync)
    // Opens one cursor and scans headers sequentially
    void getBlockHeadersRange(uint32_t startHeight, uint32_t count,
                              std::vector<cn::BlockHeaderPOD> &out) const;

    // Spent key images (double‑spend protection)
    bool isSpentKeyImage(const crypto::KeyImage &keyImage) const override;
    void markKeyImageSpent(const crypto::KeyImage &keyImage) override;

    // Global state
    // Reads from meta DB — always accurate
    uint32_t topBlockHeight() const override;
    // Forces immediate commit to persist the new height
    void setTopBlockHeight(uint32_t height) override;

    // Persistence & lifecycle
    void flush() override;
    void close() override;

    // Block entry serialisation (still available, but prefer pushCompleteBlock)
    void pushBlockEntry(uint32_t height, const cn::BinaryArray &serializedEntry) override;
    bool getBlockEntry(uint32_t height, cn::BinaryArray &serializedEntry) const override;
    void popBlockEntry(uint32_t height) override;

    // Initialisation flag (used once on first run)
    void setInitialized();
    bool isInitialized() const;

    // Generic metadata store (key‑value blobs)
    void putMeta(const std::string &key, const std::vector<uint8_t> &value);
    bool getMeta(const std::string &key, std::vector<uint8_t> &value) const;

    // Checkpoint storage with zero‑padded keys
    void storeCheckpoint(uint32_t height, const crypto::Hash &hash);
    // Cursor‑based scan over the checkpoints database
    std::vector<std::pair<uint32_t, crypto::Hash>> getCheckpoints() const;

    // Error handling
    void abortWriteTxn(); // Discards the current write transaction without committing

    // Diagnostics
    // Returns human‑readable statistics for each database
    std::string printDatabaseStats() const;

    // Migration helper (one‑time use)
    // Converts all "be_10" keys to "be_00000010" etc. in a single transaction
    void migrateToPaddedKeys();

  private:
    // Environment & database setup
    // Creates MDBX_env, sets geometry and flags, opens databases
    void openEnvironment(const std::string &path);
    // Opens all six named databases inside a provided transaction
    void openDatabases(MDBX_txn *txn);

    // Write transaction management
    // Commits the active write txn if any ops have been buffered
    void commitWriteTransaction(bool force = false);
    // Starts a new write transaction if one isn’t already active
    void ensureWriteTxn();
    // Writes the height to the meta DB without committing (caller must commit)
    void setTopBlockHeightInternal(uint32_t height);

    // Zero‑padded key helpers (lexicographic ordering -> MDBX_APPEND)
    // Returns "be_00000042" for height 42
    static std::string blockEntryKey(uint32_t height);
    // Returns "hdr_00000042"
    static std::string blockHeaderKey(uint32_t height);
    // Returns "checkpoint_0000042" — standardised to 7 digits (existing format kept for compatibility)
    std::string checkpointKey(uint32_t height);

    // Supports up to 99,999,999 blocks (~190 years at 60s blocks)
    static constexpr int kHeightKeyWidth = 8;

    // Safe MDBX_val constructors (eliminates mdbx_cast)
    // Builds an MDBX_val from any raw pointer+length
    static MDBX_val to_val(const void *data, size_t len);
    // Convenience for string data
    static MDBX_val to_val(const std::string &s);
    // Convenience for hash keys/values
    static MDBX_val to_val(const crypto::Hash &h);
    // Convenience for header values
    static MDBX_val to_val(const cn::BlockHeaderPOD &hdr);

    // MDBX handles
    MDBX_env *m_env = nullptr;
    // height > hash
    MDBX_dbi m_dbiHeights;
    // hash > height
    MDBX_dbi m_dbiBlockHeights;
    // generic string>blob metadata
    MDBX_dbi m_dbiMeta;
    // "be_<padded_height>" > serialised block
    MDBX_dbi m_dbiBlockEntries;
    // "hdr_<padded_height>" > BlockHeaderPOD
    MDBX_dbi m_dbiBlockHeaders;
    // "checkpoint_<padded_height>" > hash
    MDBX_dbi m_dbiCheckpoints;

    // Thread safety & write batching
    // Serialises all reads and writes across threads
    mutable std::mutex m_txMutex;
    // Currently open write transaction (nullptr when none)
    MDBX_txn *m_writeTxn = nullptr;
    // Counter for auto‑commit batching
    size_t m_opsSinceLastCommit = 0;

    // Commit every 1000 ops in normal mode
    static constexpr size_t kCommitBatchSize = 1000;
    // Commit every 50k ops in bulk‑sync mode
    static constexpr size_t kCommitBatchSizeBulk = 50000;

    // In‑memory cache to avoid repeated reads during sync
    uint32_t m_cachedTopHeight = 0;

    // Configuration
    // Path to the MDBX data directory
    std::string m_dataDir;
    // If true, uses MDBX_SAFE_NOSYNC and larger batch sizes
    bool m_bulkSyncMode = false;
    // Optional size limit for the database file
    uint64_t m_sizeLimitBytes = 0;
  };
} // namespace CryptoNote