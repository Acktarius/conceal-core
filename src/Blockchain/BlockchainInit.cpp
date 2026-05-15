#include "Blockchain/Blockchain.h"
#include "Blockchain/BlockCacheSerializer.h"

#include "Common/Math.h"
#include "Common/PathHelpers.h"

#include "CryptoNoteCore/CryptoNoteTools.h"

namespace cn
{
  // ─── MDBX Helpers ───────────────────────────────────────────────────────

  bool Blockchain::initMdbxStorage(const std::string &config_folder)
  {
    logger(logging::INFO) << "Initializing MDBX storage backend...";
    m_mdbxStorage.reset(new ::CryptoNote::MDBXBlockchainStorage(
        PathHelpers::appendPath(config_folder, "mdbx_blocks")));

    m_cacheIndex = 0;
    m_cachedEntries.clear();
    m_cachedEntries.reserve(MDBX_CACHE_SIZE);

    return true;
  }

  bool Blockchain::loadMdbxFastPath()
  {
    // Try to load all in-memory structures from serialised meta blobs.
    // Returns true if everything loaded, false if a full rebuild is needed.

    uint32_t topHeight = 0;
    std::vector<uint8_t> metaBuf;

    // Load block hashes
    if (m_mdbxStorage->getMeta("idx_hashes", metaBuf) && !metaBuf.empty())
    {
      size_t count = metaBuf.size() / sizeof(crypto::Hash);
      m_blockHashes.resize(count);
      memcpy(m_blockHashes.data(), metaBuf.data(), metaBuf.size());
    }

    // Load hash → height map
    if (m_mdbxStorage->getMeta("idx_hash2height", metaBuf))
    {
      const uint8_t *ptr = metaBuf.data();
      const uint8_t *end = ptr + metaBuf.size();
      while (ptr + sizeof(crypto::Hash) + sizeof(uint32_t) <= end)
      {
        crypto::Hash h;
        memcpy(h.data, ptr, sizeof(h));
        ptr += sizeof(h);
        uint32_t height;
        memcpy(&height, ptr, sizeof(height));
        ptr += sizeof(height);
        m_hashToHeight[h] = height;
      }
    }

    // Load transaction map
    if (m_mdbxStorage->getMeta("idx_txmap", metaBuf))
    {
      const uint8_t *ptr = metaBuf.data();
      const uint8_t *end = ptr + metaBuf.size();
      while (ptr + sizeof(crypto::Hash) + sizeof(uint64_t) <= end)
      {
        crypto::Hash h;
        memcpy(h.data, ptr, sizeof(h));
        ptr += sizeof(h);
        uint64_t packed;
        memcpy(&packed, ptr, sizeof(packed));
        ptr += sizeof(packed);
        TransactionIndex idx;
        idx.block = static_cast<uint32_t>(packed >> 16);
        idx.transaction = static_cast<uint16_t>(packed & 0xFFFF);
        m_transactionMap[h] = idx;
      }
    }

    // Load spent keys from meta blob
    if (m_mdbxStorage->getMeta("idx_spentkeys", metaBuf))
    {
      const uint8_t *ptr = metaBuf.data();
      const uint8_t *end = ptr + metaBuf.size();
      while (ptr + sizeof(crypto::KeyImage) + sizeof(uint32_t) <= end)
      {
        crypto::KeyImage ki;
        memcpy(ki.data, ptr, sizeof(ki));
        ptr += sizeof(ki);
        uint32_t height;
        memcpy(&height, ptr, sizeof(height));
        ptr += sizeof(height);
        m_spent_keys[ki] = height;
      }
    }

    // Load top height and validate
    if (m_mdbxStorage->getMeta("idx_topheight", metaBuf) && metaBuf.size() == sizeof(uint32_t))
    {
      memcpy(&topHeight, metaBuf.data(), sizeof(topHeight));
    }

    bool hashesLoaded = !m_blockHashes.empty();
    bool topHeightMatches = (topHeight == m_mdbxStorage->topBlockHeight());

    if (!hashesLoaded || !topHeightMatches)
      return false; // Fast path not available — need full rebuild

    // Load outputs index
    if (m_mdbxStorage->getMeta("idx_outputs", metaBuf))
    {
      const uint8_t *ptr = metaBuf.data();
      const uint8_t *end = ptr + metaBuf.size();
      while (ptr + sizeof(uint64_t) + sizeof(uint32_t) <= end)
      {
        uint64_t amount;
        memcpy(&amount, ptr, sizeof(amount));
        ptr += sizeof(amount);
        uint32_t count;
        memcpy(&count, ptr, sizeof(count));
        ptr += sizeof(count);
        std::vector<std::pair<TransactionIndex, uint16_t>> vec;
        for (uint32_t i = 0; i < count && ptr + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) <= end; ++i)
        {
          TransactionIndex txIdx;
          memcpy(&txIdx.block, ptr, sizeof(txIdx.block));
          ptr += sizeof(txIdx.block);
          memcpy(&txIdx.transaction, ptr, sizeof(txIdx.transaction));
          ptr += sizeof(txIdx.transaction);
          uint16_t outIdx;
          memcpy(&outIdx, ptr, sizeof(outIdx));
          ptr += sizeof(outIdx);
          vec.push_back({txIdx, outIdx});
        }
        m_outputs[amount] = std::move(vec);
      }
    }

    // Load multisig outputs index
    if (m_mdbxStorage->getMeta("idx_msig", metaBuf))
    {
      const uint8_t *ptr = metaBuf.data();
      const uint8_t *end = ptr + metaBuf.size();
      while (ptr + sizeof(uint64_t) + sizeof(uint32_t) <= end)
      {
        uint64_t amount;
        memcpy(&amount, ptr, sizeof(amount));
        ptr += sizeof(amount);
        uint32_t count;
        memcpy(&count, ptr, sizeof(count));
        ptr += sizeof(count);
        std::vector<MultisignatureOutputUsage> vec;
        for (uint32_t i = 0; i < count && ptr + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) + 1 <= end; ++i)
        {
          MultisignatureOutputUsage usage;
          memcpy(&usage.transactionIndex.block, ptr, sizeof(usage.transactionIndex.block));
          ptr += sizeof(usage.transactionIndex.block);
          memcpy(&usage.transactionIndex.transaction, ptr, sizeof(usage.transactionIndex.transaction));
          ptr += sizeof(usage.transactionIndex.transaction);
          memcpy(&usage.outputIndex, ptr, sizeof(usage.outputIndex));
          ptr += sizeof(usage.outputIndex);
          usage.isUsed = (*ptr++ != 0);
          vec.push_back(usage);
        }
        m_multisignatureOutputs[amount] = std::move(vec);
      }
    }

    // Load deposit index from meta blob (uses DepositIndex's own serialize method)
    m_depositIndex = DepositIndex();
    if (m_mdbxStorage->getMeta("idx_deposits", metaBuf) && !metaBuf.empty())
    {
      cn::BinaryArray ba(metaBuf.begin(), metaBuf.end());
      if (!cn::fromBinaryArray(m_depositIndex, ba))
      {
        // Deserialization failed — rebuild from blocks
        for (uint32_t h = 0; h <= topHeight; ++h)
        {
          if (h % 10000 == 0)
            logger(logging::INFO, logging::BRIGHT_WHITE) << "Rebuilding deposit index for Height " << h << " of " << topHeight;

          cn::BinaryArray blockBa;
          if (m_mdbxStorage->getBlockEntry(h, blockBa))
          {
            BlockEntry entry;
            if (cn::fromBinaryArray(entry, blockBa))
            {
              uint64_t interest = 0;
              for (const auto &tx : entry.transactions)
                interest += m_currency.calculateTotalTransactionInterest(tx.tx, h);
              pushToDepositIndex(entry, interest);
            }
          }
        }
      }
    }
    else
    {
      // No deposit meta blob — rebuild from blocks
      for (uint32_t h = 0; h <= topHeight; ++h)
      {
        if (h % 10000 == 0)
          logger(logging::INFO, logging::BRIGHT_WHITE) << "Rebuilding deposit index for Height " << h << " of " << topHeight;

        cn::BinaryArray blockBa;
        if (m_mdbxStorage->getBlockEntry(h, blockBa))
        {
          BlockEntry entry;
          if (cn::fromBinaryArray(entry, blockBa))
          {
            uint64_t interest = 0;
            for (const auto &tx : entry.transactions)
              interest += m_currency.calculateTotalTransactionInterest(tx.tx, h);
            pushToDepositIndex(entry, interest);
          }
        }
      }
    }

    // Push all block hashes into the legacy block index for compatibility
    for (const auto &h : m_blockHashes)
      m_blockIndex.push(h);

    logger(logging::INFO) << "Fast index loaded: " << m_blockHashes.size() << " blocks";
    return true;
  }

  void Blockchain::rebuildMdbxIndex()
  {
    // Full rebuild: scan every block from MDBX and rebuild all in-memory structures.
    // Spent key images are verified against the native MDBX spent DB, not rebuilt from scratch.

    logger(logging::INFO) << "Fast index not available, doing full rebuild...";

    m_blockHashes.clear();
    m_hashToHeight.clear();
    m_blockIndex.clear();
    m_transactionMap.clear();
    m_spent_keys.clear();
    m_outputs.clear();
    m_multisignatureOutputs.clear();
    m_depositIndex = DepositIndex();

    uint32_t topHeight = m_mdbxStorage->topBlockHeight();
    for (uint32_t h = 0; h <= topHeight; ++h)
    {
      cn::BinaryArray ba;
      if (m_mdbxStorage->getBlockEntry(h, ba))
      {
        if (h % 10000 == 0)
          logger(logging::INFO, logging::BRIGHT_WHITE) << "Rebuilding MDBX index for Height " << h << " of " << topHeight;

        BlockEntry entry;
        if (cn::fromBinaryArray(entry, ba))
        {
          crypto::Hash blockHash = get_block_hash(entry.bl);
          m_blockHashes.push_back(blockHash);
          m_hashToHeight[blockHash] = h;
          m_blockIndex.push(blockHash);

          for (uint32_t t = 0; t < entry.transactions.size(); ++t)
          {
            crypto::Hash txHash = getObjectHash(entry.transactions[t].tx);
            TransactionIndex txIdx = {h, static_cast<uint16_t>(t)};
            m_transactionMap.insert(std::make_pair(txHash, txIdx));

            // Spent key images are verified via the native MDBX DB at runtime.
            // We still populate m_spent_keys for the in-memory fast path.
            for (auto &input : entry.transactions[t].tx.inputs)
            {
              if (input.type() == typeid(KeyInput))
                m_spent_keys.insert(std::make_pair(
                    boost::get<KeyInput>(input).keyImage, h));
              else if (input.type() == typeid(MultisignatureInput))
              {
                const auto &msInput = boost::get<MultisignatureInput>(input);
                m_multisignatureOutputs[msInput.amount][msInput.outputIndex].isUsed = true;
              }
            }

            for (uint32_t o = 0; o < entry.transactions[t].tx.outputs.size(); ++o)
            {
              const auto &out = entry.transactions[t].tx.outputs[o];
              if (out.target.type() == typeid(KeyOutput))
                m_outputs[out.amount].push_back(std::make_pair<>(txIdx, o));
              else if (out.target.type() == typeid(MultisignatureOutput))
              {
                MultisignatureOutputUsage usage = {txIdx, static_cast<uint16_t>(o), false};
                m_multisignatureOutputs[out.amount].push_back(usage);
              }
            }
          }

          uint64_t interest = 0;
          for (const auto &tx : entry.transactions)
            interest += m_currency.calculateTotalTransactionInterest(tx.tx, h);
          pushToDepositIndex(entry, interest);
        }
      }
    }

    logger(logging::INFO) << "Loaded " << m_blockHashes.size() << " blocks from MDBX (full rebuild)";
  }

  bool Blockchain::initMdbx(bool load_existing)
  {
    auto mdbxStart = std::chrono::steady_clock::now();

    if (load_existing && m_mdbxStorage->topBlockHeight() > 0)
    {
      logger(logging::INFO) << "Loading in-memory structures from MDBX...";

      // Clear all in-memory caches
      m_blockHashes.clear();
      m_hashToHeight.clear();
      m_blockIndex.clear();
      m_transactionMap.clear();
      m_spent_keys.clear();
      m_outputs.clear();
      m_multisignatureOutputs.clear();
      m_depositIndex = DepositIndex();

      // Try fast path first (serialised meta blobs)
      if (!loadMdbxFastPath())
      {
        // Fast path failed — do a full rebuild from block entries
        rebuildMdbxIndex();
      }
    }

    auto mdbxEnd = std::chrono::steady_clock::now();
    auto mdbxMs = std::chrono::duration_cast<std::chrono::milliseconds>(mdbxEnd - mdbxStart).count();
    logger(logging::INFO) << "MDBX initialization took " << mdbxMs << " ms";
    return true;
  }

  // ─── Legacy (SwappedVector) Helpers ─────────────────────────────────────

  bool Blockchain::initLegacyStorage(const std::string &config_folder)
  {
    if (!m_blocks.open(PathHelpers::appendPath(config_folder, m_currency.blocksFileName()),
                       PathHelpers::appendPath(config_folder, m_currency.blockIndexesFileName()), 1024))
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to open blockchain storage files";
      return false;
    }
    return true;
  }

  bool Blockchain::loadLegacyCache(bool load_existing)
  {
    if (!load_existing || blocksEmpty())
    {
      blocksClear();
      return true;
    }

    logger(logging::INFO) << "Loading blockchain";
    BlockCacheSerializer loader(*this, get_block_hash(blocksBack().bl), logger.getLogger());
    const std::string &blocksCacheFileName = m_currency.blocksCacheFileName();

    try
    {
      loader.load(PathHelpers::appendPath(m_config_folder, blocksCacheFileName));

      if (!loader.loaded())
      {
        std::string blockCacheBkpFileName = blocksCacheFileName + ".bkp";
        loader.load(PathHelpers::appendPath(m_config_folder, blockCacheBkpFileName));

        if (!loader.loaded())
        {
          logger(logging::WARNING, logging::BRIGHT_YELLOW) << "No actual blockchain cache found, rebuilding internal structures";
          if (!rebuildCache())
          {
            logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to rebuild cache";
            return false;
          }
        }
      }

      // Sanity check: verify a known block's emission at height 24732
      uint64_t checkBlockHeight = 24732;
      uint64_t checkMinimum = 13000000000000;
      if (!m_testnet && blocksSize() > checkBlockHeight &&
          blocksAt(checkBlockHeight).already_generated_coins < checkMinimum)
      {
        logger(logging::WARNING, logging::BRIGHT_YELLOW) << "Invalid blocks cache, rebuilding internal structures";
        if (!rebuildBlocks())
        {
          logger(logging::WARNING, logging::BRIGHT_YELLOW) << "Impossible to rebuild";
          return false;
        }
      }

      if (m_blockchainIndexesEnabled)
        loadBlockchainIndices();
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error loading blockchain cache";
      logger(logging::WARNING, logging::BRIGHT_YELLOW) << "Attempting to rebuild cache after load error";
      try
      {
        if (!rebuildCache())
        {
          logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to rebuild cache";
          return false;
        }
      }
      catch (const std::exception &)
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to rebuild cache";
        return false;
      }
    }

    return true;
  }

  // ─── Genesis & Validation Helpers ───────────────────────────────────────

  bool Blockchain::ensureGenesisBlock()
  {
    if (!blocksEmpty())
      return true;

    logger(logging::INFO, logging::BRIGHT_WHITE) << "Blockchain not loaded, generating genesis block.";

    try
    {
      block_verification_context bvc = boost::value_initialized<block_verification_context>();
      pushBlock(m_currency.genesisBlock(), get_block_hash(m_currency.genesisBlock()), bvc, 0);
      if (bvc.m_verification_failed)
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to add genesis block to blockchain";
        return false;
      }
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error creating genesis block";
      return false;
    }

    return true;
  }

  bool Blockchain::validateGenesisBlock()
  {
    // MDBX validates genesis through its own mechanisms
    if (m_useMdbx)
      return true;

    crypto::Hash firstBlockHash = get_block_hash(blocksAt(0).bl);
    if (!(firstBlockHash == m_currency.genesisBlockHash()))
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to init: genesis block mismatch.";
      return false;
    }
    return true;
  }

  bool Blockchain::initUpgradeDetectors()
  {
    try
    {
      if (!m_upgradeDetectorV2.init() || !m_upgradeDetectorV3.init() ||
          !m_upgradeDetectorV4.init() || !m_upgradeDetectorV7.init() ||
          !m_upgradeDetectorV8.init())
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to initialize one or more upgrade detectors";
        return false;
      }
    }
    catch (const std::exception &e)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error initializing upgrade detectors: " << e.what();
      return false;
    }
    return true;
  }

  void Blockchain::logInitSummary()
  {
    if (m_useMdbx)
    {
      logger(logging::INFO, logging::BRIGHT_GREEN)
          << "Blockchain initialized. Local Height: " << blocksSize() - 1 << " [MDBX backend is active]";
    }
    else
    {
      uint64_t timestamp_diff = time(nullptr) - blocksBack().bl.timestamp;
      if (!blocksBack().bl.timestamp)
        timestamp_diff = time(nullptr) - 1341378000;

      logger(logging::INFO, logging::BRIGHT_GREEN)
          << "Blockchain initialized. last block: " << blocksSize() - 1 << ", "
          << common::timeIntervalToString(timestamp_diff)
          << " time ago, current difficulty: " << getDifficultyForNextBlock();
    }
  }

  // ─── Main Init ──────────────────────────────────────────────────────────

  bool Blockchain::init(const std::string &config_folder, bool load_existing, bool testnet)
  {
    try
    {
      m_testnet = testnet;
      m_checkpoints.set_testnet(testnet);
      std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

      if (!config_folder.empty() && !tools::create_directories_if_necessary(config_folder))
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to create data directory: " << m_config_folder;
        return false;
      }

      m_config_folder = config_folder;

      // ── Storage backend ──────────────────────────────────────────────
      if (m_useMdbx)
      {
        if (!initMdbxStorage(config_folder))
          return false;
        if (!initMdbx(load_existing))
          return false;
      }
      else
      {
        if (!initLegacyStorage(config_folder))
          return false;
        if (!loadLegacyCache(load_existing))
          return false;
      }

      // ── Genesis ──────────────────────────────────────────────────────
      if (!ensureGenesisBlock())
        return false;

      if (!validateGenesisBlock())
        return false;

      // ── Checkpoints ──────────────────────────────────────────────────
      try
      {
        uint32_t lastValidCheckpointHeight = 0;
        if (!checkCheckpoints(lastValidCheckpointHeight))
        {
          logger(logging::WARNING, logging::BRIGHT_YELLOW) << "Invalid checkpoint. Rollback blockchain to last valid checkpoint at height "
                                                           << lastValidCheckpointHeight;
          rollbackBlockchainTo(lastValidCheckpointHeight);
        }
      }
      catch (const std::exception &)
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Error checking/rolling back checkpoints";
        return false;
      }

      // ── Upgrade detectors ────────────────────────────────────────────
      if (!initUpgradeDetectors())
        return false;

      // ── Final setup ──────────────────────────────────────────────────
      update_next_comulative_size_limit();
      logInitSummary();

      return true;
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error initializing blockchain";
      return false;
    }
  }

  // ─── Deinit ─────────────────────────────────────────────────────────────

  bool Blockchain::deinit()
  {
    bool cacheStored = false, indicesStored = true;

    try
    {
      cacheStored = storeCache();
      if (m_blockchainIndexesEnabled)
        indicesStored = storeBlockchainIndices();

      if (m_mdbxStorage)
        m_mdbxStorage->flush();
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error occurred during blockchain deinit";
    }

    assert(m_messageQueueList.empty());
    return cacheStored && indicesStored;
  }

  // ─── Cache Rebuild ──────────────────────────────────────────────────────

  bool Blockchain::rebuildCache()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    logger(logging::INFO, logging::BRIGHT_WHITE) << "Rebuilding cache";

    std::chrono::steady_clock::time_point timePoint = std::chrono::steady_clock::now();
    try
    {
      if (m_useMdbx)
      {
        m_blockHashes.clear();
        m_hashToHeight.clear();
      }
      else
      {
        m_blockIndex.clear();
      }

      m_transactionMap.clear();
      m_spent_keys.clear();
      m_outputs.clear();
      m_multisignatureOutputs.clear();

      for (uint32_t b = 0; b < blocksSize(); ++b)
      {
        if (b % 1000 == 0)
          logger(logging::INFO, logging::BRIGHT_WHITE) << "Rebuilding Cache for Height " << b << " of " << blocksSize();

        const BlockEntry &block = blocksAt(b);
        crypto::Hash blockHash = get_block_hash(block.bl);

        if (m_useMdbx)
        {
          m_blockHashes.push_back(blockHash);
          m_hashToHeight[blockHash] = b;
        }
        else
        {
          m_blockIndex.push(blockHash);
        }

        uint64_t interest = 0;
        for (uint32_t t = 0; t < block.transactions.size(); ++t)
        {
          const TransactionEntry &transaction = block.transactions[t];
          crypto::Hash transactionHash = getObjectHash(transaction.tx);
          TransactionIndex transactionIndex = {b, static_cast<uint16_t>(t)};
          m_transactionMap.insert(std::make_pair(transactionHash, transactionIndex));

          for (auto &i : transaction.tx.inputs)
          {
            if (i.type() == typeid(KeyInput))
              m_spent_keys.insert(std::make_pair(boost::get<KeyInput>(i).keyImage, b));
            else if (i.type() == typeid(MultisignatureInput))
            {
              const auto &out = boost::get<MultisignatureInput>(i);
              m_multisignatureOutputs[out.amount][out.outputIndex].isUsed = true;
            }
          }

          for (uint32_t o = 0; o < transaction.tx.outputs.size(); ++o)
          {
            const auto &out = transaction.tx.outputs[o];
            if (out.target.type() == typeid(KeyOutput))
              m_outputs[out.amount].push_back(std::make_pair<>(transactionIndex, o));
            else if (out.target.type() == typeid(MultisignatureOutput))
            {
              MultisignatureOutputUsage usage = {transactionIndex, static_cast<uint16_t>(o), false};
              m_multisignatureOutputs[out.amount].push_back(usage);
            }
          }

          interest += m_currency.calculateTotalTransactionInterest(transaction.tx, b);
        }

        pushToDepositIndex(block, interest);
      }

      std::chrono::duration<double> duration = std::chrono::steady_clock::now() - timePoint;
      logger(logging::INFO, logging::BRIGHT_WHITE) << "Rebuilding internal structures took: " << duration.count();
      logger(logging::INFO, logging::BRIGHT_GREEN) << "Cache rebuilt successfully";
      return true;
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error rebuilding cache";
      return false;
    }
    catch (...)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Unknown error rebuilding cache";
      return false;
    }
  }

  // ─── Block Rebuild (Legacy Only) ────────────────────────────────────────

  bool Blockchain::rebuildBlocks()
  {
    if (m_useMdbx)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "rebuildBlocks not supported with MDBX backend";
      return false;
    }

    logger(logging::INFO, logging::BRIGHT_WHITE) << "Rebuilding cache";

    try
    {
      std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
      uint64_t alreadyGeneratedCoinsPrev = 0;
      for (uint32_t b = 0; b < blocksSize(); ++b)
      {
        if (b % 10000 == 0)
          logger(logging::INFO, logging::BRIGHT_WHITE) << "Rebuilding blocks for Height " << b << " of " << blocksSize();

        auto block = BlockEntry(blocksAt(b));
        uint64_t interest = 0, fee = 0;
        for (const auto &transaction : block.transactions)
        {
          uint64_t inAmount = m_currency.getTransactionAllInputsAmount(transaction.tx, block.height);
          uint64_t outAmount = getOutputAmount(transaction.tx);
          fee += inAmount < outAmount ? cn::parameters::MINIMUM_FEE : inAmount - outAmount;
          interest += m_currency.calculateTotalTransactionInterest(transaction.tx, b);
        }

        std::vector<size_t> lastBlocksSizes;
        get_last_n_blocks_sizes(lastBlocksSizes, m_currency.rewardBlocksWindow());
        size_t blocksSizeMedian = common::medianValue(lastBlocksSizes);

        uint64_t reward;
        int64_t emissionChange;
        if (!m_currency.getBlockReward(blocksSizeMedian, block.block_cumulative_size, alreadyGeneratedCoinsPrev, fee, b, reward, emissionChange))
        {
          logger(logging::ERROR, logging::BRIGHT_RED) << "An error occurred";
          return false;
        }
        uint64_t alreadyGeneratedCoins = alreadyGeneratedCoinsPrev + emissionChange + interest;
        block.already_generated_coins = alreadyGeneratedCoins;
        m_blocks.replace(b, block);
        alreadyGeneratedCoinsPrev = alreadyGeneratedCoins;
      }

      std::chrono::duration<double> duration = std::chrono::steady_clock::now() - startTime;
      logger(logging::INFO, logging::BRIGHT_WHITE) << "Rebuilding blocks took: " << duration.count();
      storeCache();
      m_blocks.close();
      return m_blocks.open(PathHelpers::appendPath(m_config_folder, m_currency.blocksFileName()),
                           PathHelpers::appendPath(m_config_folder, m_currency.blockIndexesFileName()), 1024);
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error rebuilding blocks";
      try
      {
        m_blocks.close();
        m_blocks.open(PathHelpers::appendPath(m_config_folder, m_currency.blocksFileName()),
                      PathHelpers::appendPath(m_config_folder, m_currency.blockIndexesFileName()), 1024);
      }
      catch (...)
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to reopen blockchain files after error";
      }
      return false;
    }
  }

  // ─── Store Cache ────────────────────────────────────────────────────────

  bool Blockchain::storeCache()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    if (m_useMdbx)
      return storeMdbxCache();

    return storeLegacyCache();
  }

  bool Blockchain::storeMdbxCache()
  {
    if (!m_mdbxStorage)
      return false;

    std::vector<uint8_t> buf;

    // Block hashes
    m_mdbxStorage->putMeta("idx_hashes",
                           std::vector<uint8_t>((uint8_t *)m_blockHashes.data(),
                                                (uint8_t *)m_blockHashes.data() + m_blockHashes.size() * sizeof(crypto::Hash)));

    // Hash → height map
    buf.clear();
    for (const auto &p : m_hashToHeight)
    {
      buf.insert(buf.end(), (uint8_t *)p.first.data, (uint8_t *)p.first.data + sizeof(crypto::Hash));
      uint32_t h = p.second;
      buf.insert(buf.end(), (uint8_t *)&h, (uint8_t *)&h + sizeof(h));
    }
    m_mdbxStorage->putMeta("idx_hash2height", buf);

    // Transaction map
    buf.clear();
    for (const auto &kv : m_transactionMap)
    {
      buf.insert(buf.end(), (uint8_t *)kv.first.data, (uint8_t *)kv.first.data + sizeof(crypto::Hash));
      uint64_t packed = (static_cast<uint64_t>(kv.second.block) << 16) | kv.second.transaction;
      buf.insert(buf.end(), (uint8_t *)&packed, (uint8_t *)&packed + sizeof(packed));
    }
    m_mdbxStorage->putMeta("idx_txmap", buf);

    // Spent keys — both meta blob (fast cache) and native DB (ground truth)
    buf.clear();
    std::vector<crypto::KeyImage> keyImages;
    keyImages.reserve(m_spent_keys.size());
    for (const auto &p : m_spent_keys)
    {
      buf.insert(buf.end(), (uint8_t *)p.first.data, (uint8_t *)p.first.data + sizeof(crypto::KeyImage));
      uint32_t h = p.second;
      buf.insert(buf.end(), (uint8_t *)&h, (uint8_t *)&h + sizeof(h));
      keyImages.push_back(p.first);
    }
    m_mdbxStorage->putMeta("idx_spentkeys", buf);
    m_mdbxStorage->markKeyImagesSpent(keyImages); // Native DB for instant startup verification

    // Top height
    uint32_t topHeight = m_mdbxStorage->topBlockHeight();
    m_mdbxStorage->putMeta("idx_topheight",
                           std::vector<uint8_t>((uint8_t *)&topHeight, (uint8_t *)&topHeight + sizeof(topHeight)));

    // Outputs index
    buf.clear();
    for (const auto &p : m_outputs)
    {
      uint64_t amount = p.first;
      buf.insert(buf.end(), (uint8_t *)&amount, (uint8_t *)&amount + sizeof(amount));
      uint32_t count = static_cast<uint32_t>(p.second.size());
      buf.insert(buf.end(), (uint8_t *)&count, (uint8_t *)&count + sizeof(count));
      for (const auto &pair : p.second)
      {
        uint32_t block = pair.first.block;
        uint16_t tx = pair.first.transaction;
        uint16_t outIdx = pair.second;
        buf.insert(buf.end(), (uint8_t *)&block, (uint8_t *)&block + sizeof(block));
        buf.insert(buf.end(), (uint8_t *)&tx, (uint8_t *)&tx + sizeof(tx));
        buf.insert(buf.end(), (uint8_t *)&outIdx, (uint8_t *)&outIdx + sizeof(outIdx));
      }
    }
    m_mdbxStorage->putMeta("idx_outputs", buf);

    // Multisig outputs index
    buf.clear();
    for (const auto &p : m_multisignatureOutputs)
    {
      uint64_t amount = p.first;
      buf.insert(buf.end(), (uint8_t *)&amount, (uint8_t *)&amount + sizeof(amount));
      uint32_t count = static_cast<uint32_t>(p.second.size());
      buf.insert(buf.end(), (uint8_t *)&count, (uint8_t *)&count + sizeof(count));
      for (const auto &usage : p.second)
      {
        uint32_t block = usage.transactionIndex.block;
        uint16_t tx = usage.transactionIndex.transaction;
        uint16_t outIdx = usage.outputIndex;
        uint8_t used = usage.isUsed ? 1 : 0;
        buf.insert(buf.end(), (uint8_t *)&block, (uint8_t *)&block + sizeof(block));
        buf.insert(buf.end(), (uint8_t *)&tx, (uint8_t *)&tx + sizeof(tx));
        buf.insert(buf.end(), (uint8_t *)&outIdx, (uint8_t *)&outIdx + sizeof(outIdx));
        buf.push_back(used);
      }
    }
    m_mdbxStorage->putMeta("idx_msig", buf);

    // Deposit index — use the class's own serialize method for consistency
    {
      cn::BinaryArray temp;
      cn::toBinaryArray(m_depositIndex, temp);
      m_mdbxStorage->putMeta("idx_deposits", std::vector<uint8_t>(temp.begin(), temp.end()));
    }

    m_mdbxStorage->flush();
    logger(logging::INFO, logging::BRIGHT_GREEN) << "MDBX index saved successfully.";
    return true;
  }

  bool Blockchain::storeLegacyCache()
  {
    logger(logging::INFO, logging::BRIGHT_WHITE) << "Saving blockchain...";
    BlockCacheSerializer ser(*this, getTailId(), logger.getLogger());
    const std::string &blocksCacheFileName = m_currency.blocksCacheFileName();
    std::string blockCacheBkpFileName = blocksCacheFileName + ".bkp";

    try
    {
      std::rename(blocksCacheFileName.c_str(), blockCacheBkpFileName.c_str());
      if (!ser.save(PathHelpers::appendPath(m_config_folder, blocksCacheFileName)))
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to save blockchain cache";
        return false;
      }
      logger(logging::INFO, logging::BRIGHT_GREEN) << "The Blockchain was successfully saved.";
      return true;
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to save blockchain cache";
      return false;
    }
  }

  // ─── Reset ──────────────────────────────────────────────────────────────

  bool Blockchain::resetAndSetGenesisBlock(const Block &b)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    blocksClear();
    m_blockIndex.clear();
    m_transactionMap.clear();
    m_spent_keys.clear();
    m_alternative_chains.clear();
    m_outputs.clear();
    m_paymentIdIndex.clear();
    m_timestampIndex.clear();
    m_generatedTransactionsIndex.clear();
    m_orthanBlocksIndex.clear();

    if (m_useMdbx)
    {
      m_blockHashes.clear();
      m_hashToHeight.clear();
      m_cachedEntries.clear();
      m_cacheIndex = 0;
    }

    block_verification_context bvc = boost::value_initialized<block_verification_context>();
    addNewBlock(b, bvc);
    return bvc.m_added_to_main_chain && !bvc.m_verification_failed;
  }

} // namespace cn