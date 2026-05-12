// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2020 Karbo developers
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <atomic>
#include <chrono>

#include <parallel_hashmap/phmap.h>

#include "Common/ObserverManager.h"
#include "Common/Util.h"
#include "CryptoNoteCore/BlockIndex.h"
#include "CryptoNoteCore/Checkpoints.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/DepositIndex.h"
#include "CryptoNoteCore/IBlockchainStorageObserver.h"
#include "CryptoNoteCore/ITransactionValidator.h"
#include "CryptoNoteCore/SwappedVector.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteCore/TransactionPool.h"
#include "CryptoNoteCore/BlockchainIndices.h"
#include "CryptoNoteCore/UpgradeDetector.h"

#include "CryptoNoteCore/MessageQueue.h"
#include "CryptoNoteCore/BlockchainMessages.h"
#include "CryptoNoteCore/IntrusiveLinkedList.h"

#include <Logging/LoggerRef.h>

// Guarded MDBX — cant mix SwappedVector
#ifdef HAVE_MDBX
#include "Storage/MDBXBlockchainStorage.h"
#endif

#undef ERROR
using phmap::parallel_flat_hash_map;
namespace cn
{
  // Forward declarations for P2P and RPC request/response structures
  struct NOTIFY_REQUEST_GET_OBJECTS_request;
  struct NOTIFY_RESPONSE_GET_OBJECTS_request;
  struct COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS_request;
  struct COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS_response;
  struct COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS_outs_for_amount;

  using cn::BlockInfo;

  // Core blockchain class — manages the chain state, block storage, transaction validation, and reorgs
  class Blockchain : public cn::ITransactionValidator
  {
  public:
    // Constructs the blockchain with a currency config, transaction pool, logger, and optional MDBX backend
    Blockchain(const Currency &currency, tx_memory_pool &tx_pool, logging::ILogger &logger, bool blockchainIndexesEnabled, bool blockchainAutosaveEnabled, bool useMdbx = false);

    // Maps a transaction hash to its block height and position within that block
    struct TransactionIndex
    {
      uint32_t block;       // Height of the block containing this transaction
      uint16_t transaction; // Index of this transaction within the block

      void serialize(ISerializer &s)
      {
        s(block, "block");
        s(transaction, "tx");
      }
    };

    // A transaction stored alongside the global output indexes it produced
    struct TransactionEntry
    {
      Transaction tx;                                // The full transaction
      std::vector<uint32_t> m_global_output_indexes; // Global indexes for each output (for ring signature mixin selection)

      void serialize(ISerializer &s)
      {
        s(tx, "tx");
        s(m_global_output_indexes, "indexes");
      }
    };

    // A complete block record: the raw block, its height, cumulative difficulty, generated coins, and all transactions
    struct BlockEntry
    {
      Block bl;                                   // The raw block (header + base transaction + transaction hashes)
      uint32_t height;                            // Height of this block in the main chain
      uint64_t block_cumulative_size;             // Sum of all block sizes up to this point
      difficulty_type cumulative_difficulty;      // Total accumulated difficulty from genesis to this block
      uint64_t already_generated_coins;           // Total coins emitted up to and including this block
      std::vector<TransactionEntry> transactions; // All transactions in this block (miner tx is index 0)

      void serialize(ISerializer &s)
      {
        s(bl, "block");
        s(height, "height");
        s(block_cumulative_size, "block_cumulative_size");
        s(cumulative_difficulty, "cumulative_difficulty");
        s(already_generated_coins, "already_generated_coins");
        s(transactions, "transactions");
      }
    };

    // Observer pattern — notifies listeners when blocks are added, removed, or the chain is reorganized
    bool addObserver(IBlockchainStorageObserver *observer);
    bool removeObserver(IBlockchainStorageObserver *observer);

    // Rebuilds the in‑memory index caches (outputs, spent keys, transaction map) by scanning the chain
    bool rebuildCache();
    // Rebuilds the blocks vector from disk (used after a crash or when switching backends)
    bool rebuildBlocks();
    // Serialises all in‑memory caches to the storage layer (called on shutdown or periodically)
    bool storeCache();

    // ITransactionValidator interface
    // Checks that all inputs of a transaction reference unspent outputs and valid key images
    bool checkTransactionInputs(const cn::Transaction &tx, BlockInfo &maxUsedBlock) override;
    bool checkTransactionInputs(const cn::Transaction &tx, BlockInfo &maxUsedBlock, BlockInfo &lastFailed) override;
    // Checks whether any of the transaction's key images have already been spent
    bool haveSpentKeyImages(const cn::Transaction &tx) override;
    // Validates that the transaction's serialised size is within the allowed limit
    bool checkTransactionSize(size_t blobSize) override;

    // Initialises the blockchain: loads or creates the database, rebuilds caches, sets up genesis
    bool init() { return init(tools::getDefaultDataDirectory(), true, m_testnet); }
    bool init(const std::string &config_folder, bool load_existing, bool testnet);
    // Shuts down the blockchain: flushes caches, closes the database, frees memory
    bool deinit();

    // Binary‑searches the chain to find the first block at or after a given timestamp
    bool getLowerBound(uint64_t timestamp, uint64_t startOffset, uint32_t &height);
    // Returns up to maxCount block hashes starting from startHeight (used for sync header chains)
    std::vector<crypto::Hash> getBlockIds(uint32_t startHeight, uint32_t maxCount);

    // Sets the checkpoints container (moved in to avoid copying)
    void setCheckpoints(Checkpoints &&chk_pts) { m_checkpoints = std::move(chk_pts); }
    // Retrieves a range of blocks and their transactions (used by the P2P sync protocol)
    bool getBlocks(uint32_t start_offset, uint32_t count, std::list<Block> &blocks, std::list<Transaction> &txs);
    bool getBlocks(uint32_t start_offset, uint32_t count, std::list<Block> &blocks);
    // Returns all blocks currently held as alternative (orphan) chains
    bool getAlternativeBlocks(std::list<Block> &blocks);
    // Fetches transactions by their hashes, along with each output's global index
    bool getTransactionsWithOutputGlobalIndexes(const std::vector<crypto::Hash> &txs_ids, std::list<crypto::Hash> &missed_txs, std::vector<std::pair<Transaction, std::vector<uint32_t>>> &txs);
    // Returns the number of blocks in alternative chains
    uint32_t getAlternativeBlocksCount();
    // Height‑to‑hash lookup
    crypto::Hash getBlockIdByHeight(uint32_t height);
    // Hash‑to‑block lookup (deserialises and returns the full block)
    bool getBlockByHash(const crypto::Hash &h, Block &blk);
    // Hash‑to‑height lookup
    bool getBlockHeight(const crypto::Hash &blockId, uint32_t &blockHeight);

    // Serialisation hook (used by the legacy binary cache dump/load)
    template <class archive_t>
    void serialize(archive_t &ar, const unsigned int version);

    // Checks whether a transaction hash exists anywhere in the main chain
    bool haveTransaction(const crypto::Hash &id);
    // Checks whether all key images in a transaction are already marked as spent
    bool haveTransactionKeyImagesAsSpent(const Transaction &tx);

    // Returns the current chain height (number of blocks)
    uint32_t getCurrentBlockchainHeight();
    // Returns the hash of the genesis block (tail of the chain)
    crypto::Hash getTailId();
    crypto::Hash getTailId(uint32_t &height);
    // Calculates the required difficulty for the next block to be mined
    difficulty_type getDifficultyForNextBlock();
    // Returns the timestamp of the block at the given height
    uint64_t getBlockTimestamp(uint32_t height);
    // Returns the total number of coins ever emitted up to the current tip
    uint64_t getCoinsInCirculation();
    // Returns the major block version expected at a given height (for upgrade scheduling)
    uint8_t get_block_major_version_for_height(uint64_t height) const;

    // Attempts to add a newly received block to the chain — validates and either extends main chain or holds as alternative
    bool addNewBlock(const Block &bl_, block_verification_context &bvc);
    // Wipes all state and sets the genesis block (used during initialisation or recovery)
    bool resetAndSetGenesisBlock(const Block &b);
    // Checks whether a block with the given hash exists in the main chain
    bool haveBlock(const crypto::Hash &id);
    // Returns the total number of transactions across all blocks in the main chain
    size_t getTotalTransactions();

    // Optimized sync methods with reduced lock contention
    // Builds a sparse chain (checkpoints + recent blocks) for efficient sync header comparison
    std::vector<crypto::Hash> buildSparseChain();
    std::vector<crypto::Hash> buildSparseChain(const crypto::Hash &startBlockId);
    // Finds where our chain diverges from a remote peer's chain
    uint32_t findBlockchainSupplement(const std::vector<crypto::Hash> &qblock_ids);
    std::vector<crypto::Hash> findBlockchainSupplement(const std::vector<crypto::Hash> &remoteBlockIds, size_t maxCount,
                                                       uint32_t &totalBlockCount, uint32_t &startBlockIndex);

    // Cache management for performance
    // Invalidates the cached sparse chain so the next call rebuilds it from scratch
    void invalidateSparseChainCache();
    // Returns the currently cached sparse chain (may be stale if invalidated)
    std::vector<crypto::Hash> getCachedSparseChain();

    // Returns the major block version active at a given height
    uint8_t getBlockMajorVersionForHeight(uint32_t height) const;
    uint8_t blockMajorVersion; // The currently active major block version (set during init)

    // Handles legacy P2P getObjects requests (block/transaction retrieval for syncing)
    bool handleGetObjects(NOTIFY_REQUEST_GET_OBJECTS_request &arg, NOTIFY_RESPONSE_GET_OBJECTS_request &rsp);
    // Selects random unspent outputs of a given amount for ring signature mixins
    bool getRandomOutsByAmount(const COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS_request &req, COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS_response &res);
    // Returns the sizes of the last `count` blocks starting from `from_height` going backwards
    bool getBackwardBlocksSize(size_t from_height, std::vector<size_t> &sz, size_t count);
    // Retrieves the global output indexes produced by a specific transaction
    bool getTransactionOutputGlobalIndexes(const crypto::Hash &tx_id, std::vector<uint32_t> &indexs);
    // Looks up a multisignature output by its amount and global index
    bool get_out_by_msig_gindex(uint64_t amount, uint64_t gindex, MultisignatureOutput &out);
    // Validates a transaction's inputs, returns the max used block height and its hash
    bool checkTransactionInputs(const Transaction &tx, uint32_t &pmax_used_block_height, crypto::Hash &max_used_block_id, BlockInfo *tail = nullptr);
    // Returns the current cumulative block size limit (for miners)
    uint64_t getCurrentCumulativeBlocksizeLimit() const;
    // Returns the difficulty of the block at index i
    uint64_t blockDifficulty(size_t i);
    // Finds which block contains a given transaction hash
    bool getBlockContainingTransaction(const crypto::Hash &txId, crypto::Hash &blockId, uint32_t &blockHeight);
    // Returns the total coins generated up to and including the block with the given hash
    bool getAlreadyGeneratedCoins(const crypto::Hash &hash, uint64_t &generatedCoins);
    // Returns the serialised size of the block with the given hash
    bool getBlockSize(const crypto::Hash &hash, size_t &size);
    // Resolves a multisignature input to the actual output it references
    bool getMultisigOutputReference(const MultisignatureInput &txInMultisig, std::pair<crypto::Hash, size_t> &outputReference);
    // Returns the number of coinbase transactions generated up to a given height
    bool getGeneratedTransactionsNumber(uint32_t height, uint64_t &generatedTransactions);
    // Returns all orphan (alternative) block hashes at a given height
    bool getOrphanBlockIdsByHeight(uint32_t height, std::vector<crypto::Hash> &blockHashes);
    // Finds block hashes whose timestamps fall within a given range
    bool getBlockIdsByTimestamp(uint64_t timestampBegin, uint64_t timestampEnd, uint32_t blocksNumberLimit, std::vector<crypto::Hash> &hashes, uint32_t &blocksNumberWithinTimestamps);
    // Finds all transaction hashes that carry a specific payment ID
    bool getTransactionIdsByPaymentId(const crypto::Hash &paymentId, std::vector<crypto::Hash> &transactionHashes);
    // Checks whether a block hash belongs to the main chain
    bool isBlockInMainChain(const crypto::Hash &blockId) const;
    // Returns the total deposit amount currently locked (sum of all deposit outputs)
    uint64_t fullDepositAmount() const;
    // Returns the total deposit amount at a specific height
    uint64_t depositAmountAtHeight(size_t height) const;
    // Returns the total deposit interest accrued at a specific height
    uint64_t depositInterestAtHeight(size_t height) const;
    // Returns the total coins emitted up to a given height
    uint64_t coinsEmittedAtHeight(uint64_t height);
    // Returns the network difficulty at a given height
    uint64_t difficultyAtHeight(uint64_t height);
    // Checks whether the given height falls within the checkpoint‑protected zone
    bool isInCheckpointZone(const uint32_t height) const;

    // Block storage access
    // Returns the number of blocks in the main chain
    size_t blocksSize() const;
    // Returns true if the main chain has no blocks
    bool blocksEmpty() const;
    // Provides read‑only access to a BlockEntry by index
    BlockEntry blocksAt(size_t i) const;
    // Provides mutable access to a BlockEntry by index
    BlockEntry blocksAt(size_t i);
    // Returns a copy of the last BlockEntry in the chain
    BlockEntry blocksBack() const;
    // Clears the in‑memory block vector (used during shutdown or reset)
    void blocksClear();

    // Retrieves a lightweight block header (from MDBX header DB when available, otherwise falls back to full block deserialisation)
    BlockHeaderPOD getBlockHeader(uint32_t height) const;

    // Scans the unspent output set for the key images referenced by a KeyInput, invoking the visitor on each match
    template <class visitor_t>
    bool scanOutputKeysForIndexes(const KeyInput &tx_in_to_key, visitor_t &vis, uint32_t *pmax_related_block_height = nullptr);

    // Message queue system — allows external components to receive blockchain events (new blocks, reorgs, etc.)
    bool addMessageQueue(MessageQueue<BlockchainMessage> &messageQueue);
    bool removeMessageQueue(MessageQueue<BlockchainMessage> &messageQueue);

    // Validates all outputs of a transaction at a given height (checks key formats, multisig rules, amounts)
    bool check_tx_outputs(const Transaction &tx, uint32_t height) const;

    // Template helper: given a list of block IDs, returns the corresponding blocks (and logs missed ones)
    template <class t_ids_container, class t_blocks_container, class t_missed_container>
    bool getBlocks(const t_ids_container &block_ids, t_blocks_container &blocks, t_missed_container &missed_bs)
    {
      std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);

      for (const auto &bl_id : block_ids)
      {
        uint32_t height = 0;

#ifdef HAVE_MDBX
        // When using MDBX, use the fast in‑memory hash>height map
        if (m_useMdbx)
        {
          auto it = m_hashToHeight.find(bl_id);
          if (it == m_hashToHeight.end())
          {
            missed_bs.push_back(bl_id);
            continue;
          }
          height = it->second;
        }
        else
#endif
          // When using the legacy backend, use the block index
          if (!m_blockIndex.getBlockHeight(bl_id, height))
          {
            missed_bs.push_back(bl_id);
            continue;
          }

#ifdef HAVE_MDBX
        if (m_useMdbx)
        {
          if (!(height < blocksSize()))
          {
            logger(logging::ERROR, logging::BRIGHT_RED) << "Internal error: bl_id=" << common::podToHex(bl_id)
                                                        << " have index record with offset=" << height << ", bigger then blocksSize()=" << blocksSize();
            return false;
          }
          blocks.push_back(blocksAt(height).bl);
        }
        else
#endif
        {
          if (!(height < m_blocks.size()))
          {
            logger(logging::ERROR, logging::BRIGHT_RED) << "Internal error: bl_id=" << common::podToHex(bl_id)
                                                        << " have index record with offset=" << height << ", bigger then m_blocks.size()=" << m_blocks.size();
            return false;
          }
          blocks.push_back(m_blocks[height].bl);
        }
      }

      return true;
    }

    // Template helper: given a list of transaction IDs, returns the corresponding transactions from the main chain
    template <class t_ids_container, class t_tx_container, class t_missed_container>
    void getBlockchainTransactions(const t_ids_container &txs_ids, t_tx_container &txs, t_missed_container &missed_txs)
    {
      std::lock_guard<std::recursive_mutex> bcLock(m_blockchain_lock);

      for (const auto &tx_id : txs_ids)
      {
        auto it = m_transactionMap.find(tx_id);
        if (it == m_transactionMap.end())
        {
          missed_txs.push_back(tx_id);
        }
        else
        {
          txs.push_back(transactionByIndex(it->second).tx);
        }
      }
    }

    // Template helper: looks up transactions in both the main chain and (optionally) the transaction pool
    template <class t_ids_container, class t_tx_container, class t_missed_container>
    void getTransactions(const t_ids_container &txs_ids, t_tx_container &txs, t_missed_container &missed_txs, bool checkTxPool = false)
    {
      if (checkTxPool)
      {
        std::lock_guard<decltype(m_tx_pool)> txLock(m_tx_pool);

        getBlockchainTransactions(txs_ids, txs, missed_txs);

        auto poolTxIds = std::move(missed_txs);
        missed_txs.clear();
        m_tx_pool.getTransactions(poolTxIds, txs, missed_txs);
      }
      else
      {
        getBlockchainTransactions(txs_ids, txs, missed_txs);
      }
    }

    // Debug functions
    // Prints a range of blocks to the log (heights, hashes, sizes)
    void print_blockchain(uint64_t start_index, uint64_t end_index);
    // Prints the block index data structure to the log
    void print_blockchain_index(bool print_all);
    // Writes the global output indexes to a file (for debugging ring signature selection)
    void print_blockchain_outs(const std::string &file);

    // Rolls back the main chain to a given height, removing all blocks above it
    bool rollbackBlockchainTo(uint32_t height);
    // Checks whether a specific key image has been spent
    bool have_tx_keyimg_as_spent(const crypto::KeyImage &key_im);

    // Returns a human‑readable summary of the storage backend (MDBX database statistics)
    std::string printDatabaseStats() const;

    // Returns a list of checkpoints between startHeight and endHeight
    CheckpointList getCheckpointList(uint32_t startHeight, uint32_t endHeight) const { return m_checkpoints.getCheckpointList(startHeight, endHeight); }
    // Adds a new checkpoint at the given height
    bool addCheckpoint(uint32_t height, const std::string &hash) { return m_checkpoints.add_checkpoint(height, hash); }

    // Callback type invoked whenever a new checkpoint is generated
    using CheckpointGeneratedCallback = std::function<void(uint32_t height, const crypto::Hash &hash)>;
    // Registers a callback that fires every time a new checkpoint is created
    void setCheckpointGeneratedCallback(CheckpointGeneratedCallback callback);

  private:
    bool m_testnet = false; // True if this node is running on the testnet

    // Tracks whether a multisignature output has been spent
    struct MultisignatureOutputUsage
    {
      TransactionIndex transactionIndex; // Location of the transaction that created this output
      uint16_t outputIndex;              // Index of this output within that transaction
      bool isUsed;                       // True if this output has been spent

      void serialize(ISerializer &s)
      {
        s(transactionIndex, "txindex");
        s(outputIndex, "outindex");
        s(isUsed, "used");
      }
    };

    // Maps a key image to the block height where it was spent (for double‑spend detection)
    using key_images_container = parallel_flat_hash_map<crypto::KeyImage, uint32_t>;
    // Maps a block hash to its full BlockEntry (used for alternative/orphan chains)
    using blocks_ext_by_hash = parallel_flat_hash_map<crypto::Hash, BlockEntry>;
    // Maps an output amount to a list of (TransactionIndex, outputIndex) pairs — the global output index
    using outputs_container = parallel_flat_hash_map<uint64_t, std::vector<std::pair<TransactionIndex, uint16_t>>>;
    // Maps an amount to a list of multisignature outputs at that denomination
    using MultisignatureOutputsContainer = parallel_flat_hash_map<uint64_t, std::vector<MultisignatureOutputUsage>>;

    // Core members
    const Currency &m_currency;                                           // The currency configuration (emission, difficulty rules, etc.)
    tx_memory_pool &m_tx_pool;                                            // Reference to the transaction pool (mempool)
    mutable std::recursive_mutex m_blockchain_lock;                       // Global lock protecting all blockchain state
    crypto::cn_context m_cn_context;                                      // Cryptographic context (scratchpad for slow hash)
    tools::ObserverManager<IBlockchainStorageObserver> m_observerManager; // Manages blockchain event observers

    // In‑memory caches
    key_images_container m_spent_keys;         // All spent key images > block height
    size_t m_current_block_cumul_sz_limit = 0; // Current maximum cumulative block size
    blocks_ext_by_hash m_alternative_chains;   // Alternative (orphan) chains keyed by block hash
    outputs_container m_outputs;               // Global output index: amount > list of (tx, output)

    std::string m_config_folder;               // Path to the node's configuration directory
    Checkpoints m_checkpoints;                 // Hardcoded checkpoint list (prevents deep reorgs)
    std::atomic<bool> m_is_in_checkpoint_zone; // True while the tip is in the checkpoint‑protected region

    // Main chain storage types
    using Blocks = SwappedVector<BlockEntry>;                                      // The main chain as a vector of BlockEntries
    using BlockMap = parallel_flat_hash_map<crypto::Hash, uint32_t>;               // Hash > height (legacy backend)
    using TransactionMap = parallel_flat_hash_map<crypto::Hash, TransactionIndex>; // Transaction hash > (block, tx index)

    friend class BlockCacheSerializer;        // Allowed to directly access private state for serialisation
    friend class BlockchainIndicesSerializer; // Allowed to directly access indices for serialisation

    Blocks m_blocks;             // The main chain — all blocks indexed by height
    cn::BlockIndex m_blockIndex; // Legacy block index (used when MDBX is disabled)

    CheckpointGeneratedCallback m_checkpointGeneratedCallback; // Fires when a new checkpoint is created

#ifdef HAVE_MDBX
    // MDBX backend members
    std::unique_ptr<CryptoNote::MDBXBlockchainStorage> m_mdbxStorage; // The MDBX storage backend (nullptr when disabled)
    bool m_useMdbx = false;                                           // Whether MDBX mode is active

    // LRU cache of recently accessed deserialised BlockEntries (avoids repeated DB reads)
    static constexpr size_t MDBX_CACHE_SIZE = 256;
    struct CachedEntry
    {
      size_t height;    // Block height (used to validate cache hits)
      BlockEntry entry; // The fully deserialised block entry
    };
    mutable std::vector<CachedEntry> m_cachedEntries; // Fixed‑size circular buffer for the LRU cache
    mutable size_t m_cacheIndex = 0;                  // Next slot to write in the circular buffer

    std::vector<crypto::Hash> m_blockHashes;                       // In‑memory list of all block hashes (for fast sparse chain building)
    parallel_flat_hash_map<crypto::Hash, uint32_t> m_hashToHeight; // Hash > height map (populated at startup for fast lookups)
#endif

    // Deposit tracking
    cn::DepositIndex m_depositIndex; // Tracks all deposit outputs for interest/term calculations

    // Transaction and multisig indices
    TransactionMap m_transactionMap;                        // Hash > (block height, tx index) for all main‑chain transactions
    MultisignatureOutputsContainer m_multisignatureOutputs; // Tracks multisig outputs by denomination

    // Upgrade detectors (one per hard fork)
    BasicUpgradeDetector m_upgradeDetectorV2; // Detects when to activate block version 2 rules
    BasicUpgradeDetector m_upgradeDetectorV3; // Detects when to activate block version 3 rules
    BasicUpgradeDetector m_upgradeDetectorV4; // Detects when to activate block version 4 rules
    BasicUpgradeDetector m_upgradeDetectorV7; // Detects when to activate block version 7 rules
    BasicUpgradeDetector m_upgradeDetectorV8; // Detects when to activate block version 8 rules

    // Optional blockchain indexes
    bool m_blockchainIndexesEnabled;                         // Whether additional indexes (payment ID, timestamp, etc.) are enabled
    bool m_blockchainAutosaveEnabled;                        // Whether the cache is automatically saved to disk periodically
    PaymentIdIndex m_paymentIdIndex;                         // Index: payment ID > list of transaction hashes
    TimestampBlocksIndex m_timestampIndex;                   // Index: timestamp range > list of block hashes
    GeneratedTransactionsIndex m_generatedTransactionsIndex; // Index: height > number of coinbase transactions
    OrphanBlocksIndex m_orthanBlocksIndex;                   // Index: height > list of orphan block hashes

    // Message queue (for notifying external components)
    IntrusiveLinkedList<MessageQueue<BlockchainMessage>> m_messageQueueList; // List of registered message queues

    logging::LoggerRef logger; // Logger instance

    // Sparse chain cache (reduces rebuilds during sync)
    mutable std::mutex m_sparseChainCacheMutex;                            // Protects the cached sparse chain
    mutable std::vector<crypto::Hash> m_cachedSparseChain;                 // The cached sparse chain
    mutable uint32_t m_cachedSparseChainHeight;                            // The chain height when the cache was built
    mutable std::chrono::steady_clock::time_point m_lastSparseChainUpdate; // When the cache was last updated
    mutable bool m_sparseChainCacheValid;                                  // True if the cache is still fresh

    // Cache validity thresholds
    static constexpr uint32_t SPARSE_CHAIN_CACHE_DURATION_SECONDS = 10; // Cache expires after 10 seconds
    static constexpr uint32_t SPARSE_CHAIN_CACHE_BLOCK_DELTA = 100;     // Cache is invalidated after 100 new blocks

    // Private methods
    // Switches the main chain to an alternative chain (reorg)
    bool switch_to_alternative_blockchain(const std::list<crypto::Hash> &alt_chain, bool discard_disconnected_chain);
    // Handles a block that doesn't extend the main chain — stores it as a potential alternative
    bool handle_alternative_block(const Block &b, const crypto::Hash &id, block_verification_context &bvc, bool sendNewAlternativeBlockMessage = true);
    // Calculates the next difficulty for a block that would extend an alternative chain
    difficulty_type get_next_difficulty_for_alternative_chain(const std::list<crypto::Hash> &alt_chain, const BlockEntry &bei);
    // Adds deposit outputs from a block to the deposit index
    void pushToDepositIndex(const BlockEntry &block, uint64_t interest);
    // Early validation of a miner (coinbase) transaction before full block processing
    bool prevalidate_miner_transaction(const Block &b, uint32_t height) const;
    // Full validation of a miner transaction's reward and emission
    bool validate_miner_transaction(const Block &b, uint32_t height, size_t cumulativeBlockSize, uint64_t alreadyGeneratedCoins, uint64_t fee, uint64_t &reward, int64_t &emissionChange);
    // Rolls back the chain during a failed alternative chain switch
    bool rollback_blockchain_switching(const std::list<Block> &original_chain, size_t rollback_height);
    // Retrieves the sizes of the last N blocks (for calculating the dynamic block size limit)
    bool get_last_n_blocks_sizes(std::vector<size_t> &sz, size_t count);
    // Adds a single output to the random output selection pool
    bool add_out_to_get_random_outs(std::vector<std::pair<TransactionIndex, uint16_t>> &amount_outs, COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS_outs_for_amount &result_outs, uint64_t amount, size_t i);
    // Checks whether a transaction's unlock time has passed
    bool is_tx_spendtime_unlocked(uint64_t unlock_time);
    // Finds the highest output index that is allowed to be spent (respecting unlock times)
    size_t find_end_of_allowed_index(const std::vector<std::pair<TransactionIndex, uint16_t>> &amount_outs);
    // Validates that a new block's timestamp is within the allowed window
    bool check_block_timestamp_main(const Block &b);
    bool check_block_timestamp(std::vector<uint64_t> timestamps, const Block &b);
    // Returns the current network‑adjusted time
    uint64_t get_adjusted_time() const;
    // Fills a vector with timestamps of the last N blocks (for median timestamp calculation)
    bool complete_timestamps_vector(uint64_t start_height, std::vector<uint64_t> &timestamps);
    // Validates the block version against the upgrade schedule
    bool checkBlockVersion(const Block &b, const crypto::Hash &blockHash);
    // Validates that a block's cumulative size doesn't exceed the limit
    bool checkCumulativeBlockSize(const crypto::Hash &blockId, size_t cumulativeBlockSize, uint64_t height);
    // Builds a sparse chain (internal, needs lock held)
    std::vector<crypto::Hash> doBuildSparseChain(const crypto::Hash &startBlockId) const;
    std::vector<crypto::Hash> doBuildSparseChainUnlocked(const crypto::Hash &startBlockId) const;
    // Calculates the cumulative block size for a block
    bool getBlockCumulativeSize(const Block &block, size_t &cumulativeSize);
    // Recalculates the dynamic cumulative block size limit
    bool update_next_comulative_size_limit();
    // Validates a single KeyInput's signature and output reference
    bool check_tx_input(const KeyInput &txin, const crypto::Hash &tx_prefix_hash, const std::vector<crypto::Signature> &sig, uint32_t *pmax_related_block_height = nullptr);
    // Validates all transaction inputs against the global output index
    bool checkTransactionInputs(const Transaction &tx, const crypto::Hash &tx_prefix_hash, uint32_t *pmax_used_block_height = nullptr);
    bool checkTransactionInputs(const Transaction &tx, uint32_t *pmax_used_block_height = nullptr);
    // Internal implementation of findBlockchainSupplement
    uint32_t findBlockchainSupplementInternal(const std::vector<crypto::Hash> &qblock_ids) const;

    // Looks up a TransactionEntry by its TransactionIndex (resolves from m_blocks)
    const TransactionEntry &transactionByIndex(TransactionIndex index);

    // Builds a BlockHeaderPOD from a fully deserialised BlockEntry
    // Used as a fallback when the header DB lacks data, and as the primary path for the non‑MDBX backend
    static cn::BlockHeaderPOD headerFromBlockEntry(const BlockEntry &entry);

    // Pushes a raw block onto the main chain (legacy path — used when transactions are already known)
    bool pushBlock(const Block &blockData, const crypto::Hash &id, block_verification_context &bvc, uint32_t height);
    // Pushes a block along with its transactions (used during sync when loading from disk)
    bool pushBlock(const Block &blockData, const std::vector<Transaction> &transactions, const crypto::Hash &id, block_verification_context &bvc);
    // Pushes a fully constructed BlockEntry onto the chain — THE primary method for adding blocks
    bool pushBlock(const BlockEntry &block);

    // When MDBX is enabled, writes a complete block atomically (entry + header + height mappings + top height in one transaction)
    // Called by pushBlock(BlockEntry) instead of the separate legacy pushBlockEntry/addBlock/setTopBlockHeight/pushBlockHeader calls
    bool pushBlockMdbx(const BlockEntry &block);

    // Removes the top block from the chain (pop operation for failed additions)
    void popBlock(const crypto::Hash &blockHash);

    // When MDBX is enabled, removes a complete block atomically during reorgs (header + entry + both height mappings + top height in one transaction)
    void popBlockMdbx(const crypto::Hash &blockHash);

    // Removes the last block from the chain (rolls back by one)
    bool removeLastBlock();

    // Records a transaction in the indices and output map
    bool pushTransaction(BlockEntry &block, const crypto::Hash &transactionHash, TransactionIndex transactionIndex);
    // Removes a single transaction from the indices
    void popTransaction(const Transaction &transaction, const crypto::Hash &transactionHash);
    // Removes all transactions from a block (during a pop operation)
    void popTransactions(const BlockEntry &block, const crypto::Hash &minerTransactionHash);
    // Validates a multisignature input's signatures and key set
    bool validateInput(const MultisignatureInput &input, const crypto::Hash &transactionHash, const crypto::Hash &transactionPrefixHash, const std::vector<crypto::Signature> &transactionSignatures);
    // Validates that all checkpoints are still present and haven't been reorged out
    bool checkCheckpoints(uint32_t &lastValidCheckpointHeight);
    // Saves the blockchain indices (payment ID, timestamp, etc.) to disk
    bool storeBlockchainIndices();
    // Loads the blockchain indices from disk
    bool loadBlockchainIndices();

    // Loads transactions from disk for a given block
    bool loadTransactions(const Block &block, std::vector<Transaction> &transactions, uint32_t height);
    // Saves transactions to disk for a given block
    void saveTransactions(const std::vector<Transaction> &transactions, uint32_t height);

    // Sends a message to all registered message queues
    void sendMessage(const BlockchainMessage &message);

    friend class LockedBlockchainStorage; // Allows LockedBlockchainStorage to access the private mutex
  };

  // RAII helper that locks the blockchain mutex for the lifetime of the object
  class LockedBlockchainStorage : private boost::noncopyable
  {
  public:
    explicit LockedBlockchainStorage(Blockchain &bc)
        : m_bc(bc), m_lock(bc.m_blockchain_lock) {}

    Blockchain *operator->()
    {
      return &m_bc;
    }

  private:
    Blockchain &m_bc;
    std::lock_guard<std::recursive_mutex> m_lock;
  };

  // Scans the global output index for a given amount, invoking the visitor on each matching output
  template <class visitor_t>
  bool Blockchain::scanOutputKeysForIndexes(const KeyInput &tx_in_to_key, visitor_t &vis, uint32_t *pmax_related_block_height)
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
    auto it = m_outputs.find(tx_in_to_key.amount);
    if (it == m_outputs.end() || !tx_in_to_key.outputIndexes.size())
      return false;

    // Convert relative output offsets to absolute indexes within the outputs vector
    std::vector<uint32_t> absolute_offsets = relative_output_offsets_to_absolute(tx_in_to_key.outputIndexes);
    std::vector<std::pair<TransactionIndex, uint16_t>> &amount_outs_vec = it->second;
    size_t count = 0;
    for (uint64_t i : absolute_offsets)
    {
      if (i >= amount_outs_vec.size())
      {
        logger(logging::INFO) << "Wrong index in transaction inputs: " << i << ", expected maximum " << amount_outs_vec.size() - 1;
        return false;
      }

      const TransactionEntry &tx = transactionByIndex(amount_outs_vec[i].first);

      // Validate that the output index points to a real output in the transaction
      if (!(amount_outs_vec[i].second < tx.tx.outputs.size()))
      {
        logger(logging::ERROR, logging::BRIGHT_RED)
            << "Wrong index in transaction outputs: "
            << amount_outs_vec[i].second << ", expected less then "
            << tx.tx.outputs.size();
        return false;
      }

      // Let the visitor inspect this output (e.g. to verify a ring signature)
      if (!vis.handle_output(tx.tx, tx.tx.outputs[amount_outs_vec[i].second], amount_outs_vec[i].second))
      {
        logger(logging::INFO) << "Failed to handle_output for output no = " << count << ", with absolute offset " << i;
        return false;
      }

      // Track the highest block height used by any input (for rangeproof validation)
      if (count++ == absolute_offsets.size() - 1 && pmax_related_block_height && *pmax_related_block_height < amount_outs_vec[i].first.block)
      {
        *pmax_related_block_height = amount_outs_vec[i].first.block;
      }
    }

    return true;
  }

  // Visitor that validates a transaction's outputs at a given height
  class check_tx_outputs_visitor : public boost::static_visitor<bool>
  {
  private:
    const Transaction &m_tx;
    uint32_t m_height;
    uint64_t m_amount;
    const Currency &m_currency;
    std::string &m_error;

  public:
    check_tx_outputs_visitor(const Transaction &tx, uint32_t height, uint64_t amount, const Currency &currency, std::string &error) : m_tx(tx), m_height(height), m_amount(amount), m_currency(currency), m_error(error) {}
    // Validates a standard (non‑multisig) output
    bool operator()(const KeyOutput &out) const
    {
      if (m_amount == 0)
      {
        m_error = "zero amount output";
        return false;
      }

      if (!check_key(out.key))
      {
        m_error = "output with invalid key";
        return false;
      }

      return true;
    }
    // Validates a multisignature output
    bool operator()(const MultisignatureOutput &out) const
    {
      if (m_tx.version < TRANSACTION_VERSION_2)
      {
        m_error = "contains multisignature output but have version " + m_tx.version;
        return false;
      }

      if (!m_currency.validateOutput(m_amount, out, m_height))
      {
        m_error = "contains invalid multisignature output";
        return false;
      }

      if (out.requiredSignatureCount > out.keys.size())
      {
        m_error = "contains multisignature with invalid required signature count";
        return false;
      }

      if (std::any_of(out.keys.begin(), out.keys.end(), [](const crypto::PublicKey &key)
                      { return !check_key(key); }))
      {
        m_error = "contains multisignature output with invalid public key";
        return false;
      }

      return true;
    }
  };
} // namespace cn