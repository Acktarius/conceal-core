// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2020 Karbo developers
// Copyright (c) 2018-2025 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Blockchain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <boost/foreach.hpp>
#include "Common/Math.h"
#include "Common/int-util.h"
#include "Common/ShuffleGenerator.h"
#include "Common/StdInputStream.h"
#include "Common/StdOutputStream.h"
#include "Rpc/CoreRpcServerCommandsDefinitions.h"
#include "Serialization/BinarySerializationTools.h"
#include "CryptoNoteTools.h"
#include "TransactionExtra.h"
#include "CryptoNoteConfig.h"
#include "parallel_hashmap/phmap_dump.h"

using namespace logging;
using namespace common;

namespace
{
  std::string appendPath(const std::string &path, const std::string &fileName)
  {
    std::string result = path;
    if (!result.empty())
    {
      result += '/';
    }

    result += fileName;
    return result;
  }
} // namespace

namespace std
{
  bool operator<(const crypto::Hash &hash1, const crypto::Hash &hash2)
  {
    return memcmp(&hash1, &hash2, crypto::HASH_SIZE) < 0;
  }

  bool operator<(const crypto::KeyImage &keyImage1, const crypto::KeyImage &keyImage2)
  {
    return memcmp(&keyImage1, &keyImage2, 32) < 0;
  }
} // namespace std

#define CURRENT_BLOCKCACHE_STORAGE_ARCHIVE_VER 5
#define CURRENT_BLOCKCHAININDICES_STORAGE_ARCHIVE_VER 1

namespace cn
{
  class BlockCacheSerializer;
  class BlockchainIndicesSerializer;
} // namespace cn

namespace cn
{

  // custom serialization to speedup cache loading
  bool serialize(std::vector<std::pair<Blockchain::TransactionIndex, uint16_t>> &value, common::StringView name, cn::ISerializer &s)
  {
    const size_t elementSize = sizeof(std::pair<Blockchain::TransactionIndex, uint16_t>);
    size_t size = value.size() * elementSize;

    if (!s.beginArray(size, name))
    {
      return false;
    }

    if (s.type() == cn::ISerializer::INPUT)
    {
      if (size % elementSize != 0)
      {
        throw std::runtime_error("Invalid vector size");
      }
      value.resize(size / elementSize);
    }

    if (size)
    {
      s.binary(value.data(), size, "");
    }

    s.endArray();
    return true;
  }

  void serialize(Blockchain::TransactionIndex &value, ISerializer &s)
  {
    s(value.block, "block");
    s(value.transaction, "tx");
  }

  class BlockCacheSerializer
  {

  public:
    BlockCacheSerializer(Blockchain &bs, const crypto::Hash &lastBlockHash, ILogger &logger) : m_bs(bs), m_lastBlockHash(lastBlockHash), logger(logger, "BlockCacheSerializer")
    {
    }

    void load(const std::string &filename)
    {
      std::ifstream stdStream;
      try
      {
        stdStream.open(filename, std::ios::binary);
        if (!stdStream)
        {
          return;
        }

        StdInputStream stream(stdStream);
        BinaryInputStreamSerializer s(stream);
        cn::serialize(*this, s);

        stdStream.close();
      }
      catch (const std::exception &e)
      {
        logger(WARNING) << "loading failed: " << e.what();
      }
    }

    bool save(const std::string &filename)
    {
      std::ofstream file;
      try
      {
        file.open(filename, std::ios::binary);
        if (!file)
        {
          return false;
        }

        StdOutputStream stream(file);
        BinaryOutputStreamSerializer s(stream);
        cn::serialize(*this, s);

        file.flush();
        file.close();
        return true;
      }
      catch (const std::exception &)
      {
        if (file.is_open())
        {
          file.close();
        }
        return false;
      }

      return true;
    }

    void serialize(ISerializer &s)
    {
      auto start = std::chrono::steady_clock::now();

      uint8_t version = CURRENT_BLOCKCACHE_STORAGE_ARCHIVE_VER;
      s(version, "version");

      // ignore old versions, do rebuild
      if (version < CURRENT_BLOCKCACHE_STORAGE_ARCHIVE_VER)
      {
        return;
      }

      std::string operation;
      if (s.type() == ISerializer::INPUT)
      {
        operation = "loading ";
        crypto::Hash blockHash;
        s(blockHash, "last_block");

        if (blockHash != m_lastBlockHash)
        {
          return;
        }
      }
      else
      {
        operation = "- saving ";
        s(m_lastBlockHash, "last_block");
      }

      logger(INFO) << operation << "block index";
      s(m_bs.m_blockIndex, "block_index");

      logger(INFO) << operation << "transaction map";
      if (s.type() == ISerializer::INPUT)
      {
        phmap::BinaryInputArchive ar_in(appendPath(m_bs.m_config_folder, "transactionsmap.dat").c_str());
        m_bs.m_transactionMap.phmap_load(ar_in);
      }
      else
      {
        phmap::BinaryOutputArchive ar_out(appendPath(m_bs.m_config_folder, "transactionsmap.dat").c_str());
        m_bs.m_transactionMap.phmap_dump(ar_out);
      }

      logger(INFO) << operation << "spent keys";
      if (s.type() == ISerializer::INPUT)
      {
        phmap::BinaryInputArchive ar_in(appendPath(m_bs.m_config_folder, "spentkeys.dat").c_str());
        m_bs.m_spent_keys.phmap_load(ar_in);
      }
      else
      {
        phmap::BinaryOutputArchive ar_out(appendPath(m_bs.m_config_folder, "spentkeys.dat").c_str());
        m_bs.m_spent_keys.phmap_dump(ar_out);
      }

      logger(INFO) << operation << "outputs";
      s(m_bs.m_outputs, "outputs");

      logger(INFO) << operation << "multi-signature outputs";
      s(m_bs.m_multisignatureOutputs, "multisig_outputs");

      logger(INFO) << operation << "deposit index";
      s(m_bs.m_depositIndex, "deposit_index");

      auto dur = std::chrono::steady_clock::now() - start;

      logger(INFO) << "Serialization time: " << std::chrono::duration_cast<std::chrono::milliseconds>(dur).count() << "ms";

      m_loaded = true;
    }

    bool loaded() const
    {
      return m_loaded;
    }

  private:
    Blockchain &m_bs;
    crypto::Hash m_lastBlockHash;
    LoggerRef logger;
    bool m_loaded = false;
  };

  class BlockchainIndicesSerializer
  {

  public:
    BlockchainIndicesSerializer(Blockchain &bs, const crypto::Hash &lastBlockHash, ILogger &logger) : m_bs(bs), m_lastBlockHash(lastBlockHash), logger(logger, "BlockchainIndicesSerializer")
    {
    }

    void serialize(ISerializer &s)
    {
      uint8_t version = CURRENT_BLOCKCHAININDICES_STORAGE_ARCHIVE_VER;

      KV_MEMBER(version);

      // ignore old versions, do rebuild
      if (version != CURRENT_BLOCKCHAININDICES_STORAGE_ARCHIVE_VER)
      {
        return;
      }

      std::string operation;

      if (s.type() == ISerializer::INPUT)
      {
        operation = "loading ";

        crypto::Hash blockHash;
        s(blockHash, "blockHash");

        if (blockHash != m_lastBlockHash)
        {
          return;
        }
      }
      else
      {
        operation = "- saving ";
        s(m_lastBlockHash, "blockHash");
      }

      logger(INFO) << operation << "paymentID index";
      s(m_bs.m_paymentIdIndex, "paymentIdIndex");

      logger(INFO) << operation << "timestamp index";
      s(m_bs.m_timestampIndex, "timestampIndex");

      logger(INFO) << operation << "generated transactions index";
      s(m_bs.m_generatedTransactionsIndex, "generatedTransactionsIndex");

      m_loaded = true;
    }

    template <class Archive>
    void serialize(Archive &ar, unsigned int version)
    {

      // ignore old versions, do rebuild
      if (version < CURRENT_BLOCKCHAININDICES_STORAGE_ARCHIVE_VER)
        return;

      std::string operation;
      if (Archive::is_loading::value)
      {
        operation = "loading ";
        crypto::Hash blockHash;
        ar & blockHash;

        if (blockHash != m_lastBlockHash)
        {
          return;
        }
      }
      else
      {
        operation = "- saving ";
        ar & m_lastBlockHash;
      }

      logger(INFO) << operation << "paymentID index";
      ar & m_bs.m_paymentIdIndex;

      logger(INFO) << operation << "timestamp index";
      ar & m_bs.m_timestampIndex;

      logger(INFO) << operation << "generated transactions index";
      ar & m_bs.m_generatedTransactionsIndex;

      m_loaded = true;
    }

    bool loaded() const
    {
      return m_loaded;
    }

  private:
    Blockchain &m_bs;
    crypto::Hash m_lastBlockHash;
    LoggerRef logger;
    bool m_loaded = false;
  };

  Blockchain::Blockchain(const Currency &currency, tx_memory_pool &tx_pool, ILogger &logger, bool blockchainIndexesEnabled, bool blockchainAutosaveEnabled, bool useMdbx) : m_currency(currency),
                                                                                                                                                                            m_tx_pool(tx_pool),
                                                                                                                                                                            m_checkpoints(logger),
                                                                                                                                                                            m_upgradeDetectorV2(currency, this, BLOCK_MAJOR_VERSION_2, logger),
                                                                                                                                                                            m_upgradeDetectorV3(currency, this, BLOCK_MAJOR_VERSION_3, logger),
                                                                                                                                                                            m_upgradeDetectorV4(currency, this, BLOCK_MAJOR_VERSION_4, logger),
                                                                                                                                                                            m_upgradeDetectorV7(currency, this, BLOCK_MAJOR_VERSION_7, logger),
                                                                                                                                                                            m_upgradeDetectorV8(currency, this, BLOCK_MAJOR_VERSION_8, logger),
                                                                                                                                                                            m_blockchainIndexesEnabled(blockchainIndexesEnabled),
                                                                                                                                                                            m_blockchainAutosaveEnabled(blockchainAutosaveEnabled),
                                                                                                                                                                            m_sparseChainCacheValid(false),
                                                                                                                                                                            m_cachedSparseChainHeight(0),
                                                                                                                                                                            logger(logger, "Blockchain")
  {
#ifdef HAVE_MDBX
    m_useMdbx = useMdbx;
    m_cachedEntries.reserve(MDBX_CACHE_SIZE);
#endif
    m_lastSparseChainUpdate = std::chrono::steady_clock::now();
  }

  // Block access wrappers (work with both MDBX and SwappedVector)
  size_t Blockchain::blocksSize() const
  {
#ifdef HAVE_MDBX
    if (m_useMdbx && m_mdbxStorage)
      return m_blockHashes.size();
#endif
    return m_blocks.size();
  }

  bool Blockchain::blocksEmpty() const
  {
#ifdef HAVE_MDBX
    if (m_useMdbx && m_mdbxStorage)
      return m_blockHashes.empty();
#endif
    return m_blocks.empty();
  }

  const Blockchain::BlockEntry &Blockchain::blocksAt(size_t i) const
  {
#ifdef HAVE_MDBX
    if (m_useMdbx && m_mdbxStorage)
    {
      // Check LRU cache
      for (size_t c = 0; c < m_cachedEntries.size(); ++c)
      {
        if (m_cachedEntries[c].height == i)
          return m_cachedEntries[c].entry;
      }

      // Load from MDBX
      cn::BinaryArray ba;
      if (!m_mdbxStorage->getBlockEntry((uint32_t)i, ba))
        throw std::runtime_error("blocksAt: block not found at height " + std::to_string(i));

      BlockEntry entry;
      if (!cn::fromBinaryArray(entry, ba))
        throw std::runtime_error("blocksAt: failed to deserialise block at height " + std::to_string(i));

      if (m_cachedEntries.size() < MDBX_CACHE_SIZE)
      {
        m_cachedEntries.push_back({i, std::move(entry)});
        return m_cachedEntries.back().entry;
      }
      else
      {
        m_cachedEntries[m_cacheIndex] = {i, std::move(entry)};
        size_t insertedIndex = m_cacheIndex;
        m_cacheIndex = (m_cacheIndex + 1) % MDBX_CACHE_SIZE;
        return m_cachedEntries[insertedIndex].entry;
      }
    }
#endif
    return const_cast<Blocks &>(m_blocks)[i];
  }

  Blockchain::BlockEntry &Blockchain::blocksAt(size_t i)
  {
    return const_cast<BlockEntry &>(const_cast<const Blockchain *>(this)->blocksAt(i));
  }

  Blockchain::BlockEntry Blockchain::blocksBack() const
  {
    return blocksAt(blocksSize() - 1);
  }

  void Blockchain::blocksClear()
  {
#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      m_cachedEntries.clear();
      m_cachedEntries.reserve(MDBX_CACHE_SIZE);
      m_cacheIndex = 0;
      m_blockHashes.clear();
      m_hashToHeight.clear();
      return;
    }
#endif
    m_blocks.clear();
  }

  // Lightweight header access
  cn::BlockHeaderPOD Blockchain::getBlockHeader(uint32_t height) const
  {
#ifdef HAVE_MDBX
    if (m_useMdbx && m_mdbxStorage)
    {
      cn::BlockHeaderPOD hdr;
      if (m_mdbxStorage->getBlockHeader(height, hdr))
      {
        // Safety check: if the header has zero cumulative difficulty but the
        // block exists (height < blocksSize()), fall back to the full block entry.
        if (hdr.cumulativeDifficulty == 0 && height < blocksSize())
        {
          logger(DEBUGGING) << "Header for height " << height << " has zero cumulativeDifficulty, falling back to full block entry";
          // fall through to fallback
        }
        else
          return hdr;
      }
      // fallback
      const BlockEntry &entry = blocksAt(height);
      hdr.majorVersion = entry.bl.majorVersion;
      hdr.minorVersion = entry.bl.minorVersion;
      hdr.timestamp = entry.bl.timestamp;
      hdr.previousBlockHash = entry.bl.previousBlockHash;
      hdr.nonce = entry.bl.nonce;
      hdr.blockCumulativeSize = entry.block_cumulative_size;
      hdr.cumulativeDifficulty = entry.cumulative_difficulty;
      hdr.alreadyGeneratedCoins = entry.already_generated_coins;
      hdr.height = entry.height;
      return hdr;
    }
#endif
    // non‑MDBX path
    const BlockEntry &entry = blocksAt(height);
    cn::BlockHeaderPOD hdr;
    hdr.majorVersion = entry.bl.majorVersion;
    hdr.minorVersion = entry.bl.minorVersion;
    hdr.timestamp = entry.bl.timestamp;
    hdr.previousBlockHash = entry.bl.previousBlockHash;
    hdr.nonce = entry.bl.nonce;
    hdr.blockCumulativeSize = entry.block_cumulative_size;
    hdr.cumulativeDifficulty = entry.cumulative_difficulty;
    hdr.alreadyGeneratedCoins = entry.already_generated_coins;
    hdr.height = entry.height;
    return hdr;
  }

  // BasicUpgradeDetector (non‑template) implementation
  BasicUpgradeDetector::BasicUpgradeDetector(const Currency &currency, Blockchain *blockchain,
                                             uint8_t targetVersion, logging::ILogger &log)
      : logger(log, "upgrade"),
        m_currency(currency),
        m_blockchain(blockchain),
        m_targetVersion(targetVersion),
        m_votingCompleteHeight(UNDEF_HEIGHT)
  {
  }

  bool BasicUpgradeDetector::init()
  {
    uint32_t upgradeH = m_currency.upgradeHeight(m_targetVersion);
    if (upgradeH == UNDEF_HEIGHT)
    {
      if (m_blockchain->blocksEmpty())
      {
        m_votingCompleteHeight = UNDEF_HEIGHT;
      }
      else if (m_targetVersion - 1 == m_blockchain->blocksBack().bl.majorVersion)
      {
        m_votingCompleteHeight = findVotingCompleteHeight(
            static_cast<uint32_t>(m_blockchain->blocksSize() - 1));
      }
      else if (m_targetVersion <= m_blockchain->blocksBack().bl.majorVersion)
      {
        uint32_t found = 0;
        for (size_t i = 0; i < m_blockchain->blocksSize(); ++i)
        {
          if (m_blockchain->blocksAt(i).bl.majorVersion == m_targetVersion)
          {
            found = static_cast<uint32_t>(i);
            break;
          }
        }
        m_votingCompleteHeight = findVotingCompleteHeight(found);
        if (m_votingCompleteHeight == UNDEF_HEIGHT)
        {
          logger(logging::ERROR, logging::BRIGHT_RED)
              << "Internal error: voting complete height isn't found, upgrade height = " << found;
          return false;
        }
      }
      else
      {
        m_votingCompleteHeight = UNDEF_HEIGHT;
      }
    }
    else if (!m_blockchain->blocksEmpty())
    {
      if (m_blockchain->blocksSize() <= upgradeH + 1)
      {
        if (m_blockchain->blocksBack().bl.majorVersion >= m_targetVersion)
        {
          logger(logging::ERROR, logging::BRIGHT_RED)
              << "Internal error: block at height " << (m_blockchain->blocksSize() - 1)
              << " has invalid version " << static_cast<int>(m_blockchain->blocksBack().bl.majorVersion)
              << ", expected " << static_cast<int>(m_targetVersion - 1) << " or less";
          return false;
        }
      }
      else
      {
        int versionAfter = m_blockchain->blocksAt(upgradeH + 1).bl.majorVersion;
        if (versionAfter != m_targetVersion)
        {
          logger(logging::ERROR, logging::BRIGHT_RED)
              << "Internal error: block at height " << (upgradeH + 1)
              << " has invalid version " << versionAfter
              << ", expected " << static_cast<int>(m_targetVersion);
          return false;
        }
      }
    }
    return true;
  }

  uint32_t BasicUpgradeDetector::upgradeHeight() const
  {
    if (m_currency.upgradeHeight(m_targetVersion) == UNDEF_HEIGHT)
      return (m_votingCompleteHeight == UNDEF_HEIGHT)
                 ? UNDEF_HEIGHT
                 : m_currency.calculateUpgradeHeight(m_votingCompleteHeight);
    return m_currency.upgradeHeight(m_targetVersion);
  }

  void BasicUpgradeDetector::blockPushed()
  {
    assert(!m_blockchain->blocksEmpty());

    if (m_currency.upgradeHeight(m_targetVersion) != UNDEF_HEIGHT)
    {
      if (m_blockchain->blocksSize() <= m_currency.upgradeHeight(m_targetVersion) + 1)
        assert(m_blockchain->blocksBack().bl.majorVersion <= m_targetVersion - 1);
      else
        assert(m_blockchain->blocksBack().bl.majorVersion >= m_targetVersion);
    }
    else if (m_votingCompleteHeight != UNDEF_HEIGHT)
    {
      assert(m_blockchain->blocksSize() > m_votingCompleteHeight);
      if (m_blockchain->blocksSize() <= upgradeHeight())
      {
        assert(m_blockchain->blocksBack().bl.majorVersion == m_targetVersion - 1);
        if (m_blockchain->blocksSize() % (60 * 60 / m_currency.difficultyTarget()) == 0)
        {
          auto interval = m_currency.difficultyTarget() *
                          (upgradeHeight() - m_blockchain->blocksSize() + 2);
          time_t upgradeTimestamp = time(nullptr) + static_cast<time_t>(interval);
          std::string upgradeTime = common::formatTimestamp(upgradeTimestamp);
          logger(logging::TRACE, logging::BRIGHT_GREEN)
              << "###### UPGRADE is going to happen after block index "
              << upgradeHeight() << " at about " << upgradeTime
              << " (in " << common::timeIntervalToString(interval)
              << ")! Current last block index "
              << (m_blockchain->blocksSize() - 1)
              << ", hash " << get_block_hash(m_blockchain->blocksBack().bl);
        }
      }
      else if (m_blockchain->blocksSize() == upgradeHeight() + 1)
      {
        assert(m_blockchain->blocksBack().bl.majorVersion == m_targetVersion - 1);
        logger(logging::TRACE, logging::BRIGHT_GREEN)
            << "###### UPGRADE has happened! Starting from block index "
            << (upgradeHeight() + 1)
            << " blocks with major version below " << static_cast<int>(m_targetVersion)
            << " will be rejected!";
      }
      else
      {
        assert(m_blockchain->blocksBack().bl.majorVersion == m_targetVersion);
      }
    }
    else
    {
      uint32_t lastBlockHeight = static_cast<uint32_t>(m_blockchain->blocksSize() - 1);
      if (isVotingComplete(lastBlockHeight))
      {
        m_votingCompleteHeight = lastBlockHeight;
        logger(logging::TRACE, logging::BRIGHT_GREEN)
            << "###### UPGRADE voting complete at block index "
            << m_votingCompleteHeight
            << "! UPGRADE is going to happen after block index "
            << upgradeHeight() << "!";
      }
    }
  }

  void BasicUpgradeDetector::blockPopped()
  {
    if (m_votingCompleteHeight != UNDEF_HEIGHT)
    {
      assert(m_currency.upgradeHeight(m_targetVersion) == UNDEF_HEIGHT);
      if (m_blockchain->blocksSize() == m_votingCompleteHeight)
      {
        logger(logging::TRACE, logging::BRIGHT_YELLOW)
            << "###### UPGRADE after block index " << upgradeHeight()
            << " has been canceled!";
        m_votingCompleteHeight = UNDEF_HEIGHT;
      }
      else
      {
        assert(m_blockchain->blocksSize() > m_votingCompleteHeight);
      }
    }
  }

  size_t BasicUpgradeDetector::getNumberOfVotes(uint32_t height)
  {
    if (height < m_currency.upgradeVotingWindow() - 1)
      return 0;
    size_t voteCounter = 0;
    for (size_t i = height + 1 - m_currency.upgradeVotingWindow(); i <= height; ++i)
    {
      const auto &b = m_blockchain->blocksAt(i).bl;
      voteCounter += (b.majorVersion == m_targetVersion - 1 && b.minorVersion == BLOCK_MINOR_VERSION_1) ? 1 : 0;
    }
    return voteCounter;
  }

  uint32_t BasicUpgradeDetector::findVotingCompleteHeight(uint32_t probableUpgradeHeight)
  {
    assert(m_currency.upgradeHeight(m_targetVersion) == UNDEF_HEIGHT);
    uint32_t start = (probableUpgradeHeight > m_currency.maxUpgradeDistance())
                         ? probableUpgradeHeight - m_currency.maxUpgradeDistance()
                         : 0;
    for (size_t i = start; i <= probableUpgradeHeight; ++i)
    {
      if (isVotingComplete(static_cast<uint32_t>(i)))
        return static_cast<uint32_t>(i);
    }
    return UNDEF_HEIGHT;
  }

  bool BasicUpgradeDetector::isVotingComplete(uint32_t height)
  {
    assert(m_currency.upgradeHeight(m_targetVersion) == UNDEF_HEIGHT);
    assert(m_currency.upgradeVotingWindow() > 1);
    assert(m_currency.upgradeVotingThreshold() > 0 && m_currency.upgradeVotingThreshold() <= 100);
    size_t voteCounter = getNumberOfVotes(height);
    return (size_t)m_currency.upgradeVotingThreshold() * m_currency.upgradeVotingWindow() <= 100 * voteCounter;
  }

  // Original Blockchain methods
  bool Blockchain::addObserver(IBlockchainStorageObserver *observer)
  {
    return m_observerManager.add(observer);
  }

  bool Blockchain::removeObserver(IBlockchainStorageObserver *observer)
  {
    return m_observerManager.remove(observer);
  }

  bool Blockchain::checkTransactionInputs(const cn::Transaction &tx, BlockInfo &maxUsedBlock)
  {
    return checkTransactionInputs(tx, maxUsedBlock.height, maxUsedBlock.id) && check_tx_outputs(tx, maxUsedBlock.height);
  }

  bool Blockchain::checkTransactionInputs(const cn::Transaction &tx, BlockInfo &maxUsedBlock, BlockInfo &lastFailed)
  {

    BlockInfo tail;
    // not the best implementation at this time, sorry :(
    // check is ring_signature already checked ?
    if (maxUsedBlock.empty())
    {
      // not checked, lets try to check
      if (!lastFailed.empty() && getCurrentBlockchainHeight() > lastFailed.height && getBlockIdByHeight(lastFailed.height) == lastFailed.id)
      {
        return false; // we already sure that this tx is broken for this height
      }

      if (!checkTransactionInputs(tx, maxUsedBlock.height, maxUsedBlock.id, &tail))
      {
        lastFailed = tail;
        return false;
      }
    }
    else
    {
      if (maxUsedBlock.height >= getCurrentBlockchainHeight())
      {
        return false;
      }

      if (getBlockIdByHeight(maxUsedBlock.height) != maxUsedBlock.id)
      {
        // if we already failed on this height and id, skip actual ring signature check
        if (lastFailed.id == getBlockIdByHeight(lastFailed.height))
        {
          return false;
        }

        // check ring signature again, it is possible (with very small chance) that this transaction become again valid
        if (!checkTransactionInputs(tx, maxUsedBlock.height, maxUsedBlock.id, &tail))
        {
          lastFailed = tail;
          return false;
        }
      }
    }

    return true;
  }

  bool Blockchain::haveSpentKeyImages(const cn::Transaction &tx)
  {
    return this->haveTransactionKeyImagesAsSpent(tx);
  }

  bool Blockchain::checkTransactionSize(size_t blobSize)
  {
    if (blobSize >= getCurrentCumulativeBlocksizeLimit() - m_currency.minerTxBlobReservedSize())
    {
      logger(ERROR) << "transaction is too big " << blobSize << ", maximum allowed size is " << (getCurrentCumulativeBlocksizeLimit() - m_currency.minerTxBlobReservedSize());

      return false;
    }

    return true;
  }

  bool Blockchain::haveTransaction(const crypto::Hash &id)
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
    return m_transactionMap.find(id) != m_transactionMap.end();
  }

  bool Blockchain::have_tx_keyimg_as_spent(const crypto::KeyImage &key_im)
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
    return m_spent_keys.find(key_im) != m_spent_keys.end();
  }

  uint32_t Blockchain::getCurrentBlockchainHeight()
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
#ifdef HAVE_MDBX
    if (m_useMdbx && m_mdbxStorage)
      return static_cast<uint32_t>(m_blockHashes.size());
#endif
    return static_cast<uint32_t>(m_blocks.size());
  }

  bool Blockchain::init(const std::string &config_folder, bool load_existing, bool testnet)
  {
    try
    {
      m_testnet = testnet;
      m_checkpoints.set_testnet(testnet);
      std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

      if (!config_folder.empty() && !tools::create_directories_if_necessary(config_folder))
      {
        logger(ERROR, BRIGHT_RED) << " Failed to create data directory: " << m_config_folder;
        return false;
      }

      m_config_folder = config_folder;

#ifdef HAVE_MDBX
      if (m_useMdbx)
      {
        logger(INFO) << "Initializing MDBX storage backend...";
        m_mdbxStorage.reset(new ::CryptoNote::MDBXBlockchainStorage(
            appendPath(config_folder, "mdbx_blocks")));

        auto mdbxStart = std::chrono::steady_clock::now();

        if (load_existing && m_mdbxStorage->topBlockHeight() > 0)
        {
          logger(INFO) << "Loading in-memory structures from MDBX...";

          m_blockHashes.clear();
          m_hashToHeight.clear();
          m_blockIndex.clear();
          m_transactionMap.clear();
          m_spent_keys.clear();
          m_outputs.clear();
          m_multisignatureOutputs.clear();
          m_depositIndex = DepositIndex();

          uint32_t topHeight = 0;
          bool loadedFast = false;
          bool fastIndexesLoaded = false;

          std::vector<uint8_t> metaBuf;

          if (m_mdbxStorage->getMeta("idx_hashes", metaBuf) && !metaBuf.empty())
          {
            size_t count = metaBuf.size() / sizeof(crypto::Hash);
            m_blockHashes.resize(count);
            memcpy(m_blockHashes.data(), metaBuf.data(), metaBuf.size());
          }

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

          if (m_mdbxStorage->getMeta("idx_topheight", metaBuf) && metaBuf.size() == sizeof(uint32_t))
          {
            memcpy(&topHeight, metaBuf.data(), sizeof(topHeight));
            loadedFast = (m_blockHashes.size() == topHeight + 1);
          }

          if (loadedFast)
          {
            bool outputsLoaded = false;
            bool msigLoaded = false;

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
              outputsLoaded = !m_outputs.empty();
            }

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
              msigLoaded = !m_multisignatureOutputs.empty();
            }

            fastIndexesLoaded = outputsLoaded && msigLoaded && !m_spent_keys.empty() && !m_transactionMap.empty();

            m_depositIndex = DepositIndex();
            for (uint32_t h = 0; h <= topHeight; ++h)
            {
              if (h % 10000 == 0)
                logger(INFO, BRIGHT_WHITE) << "Rebuilding MDBX index for Height " << h << " of " << topHeight;

              cn::BinaryArray ba;
              if (m_mdbxStorage->getBlockEntry(h, ba))
              {
                BlockEntry entry;
                if (cn::fromBinaryArray(entry, ba))
                {
                  if (!fastIndexesLoaded)
                  {
                    crypto::Hash blockHash = get_block_hash(entry.bl);

                    for (uint32_t t = 0; t < entry.transactions.size(); ++t)
                    {
                      crypto::Hash txHash = getObjectHash(entry.transactions[t].tx);
                      TransactionIndex txIdx = {h, static_cast<uint16_t>(t)};
                      m_transactionMap.insert(std::make_pair(txHash, txIdx));

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
                  }

                  uint64_t interest = 0;
                  for (const auto &tx : entry.transactions)
                    interest += m_currency.calculateTotalTransactionInterest(tx.tx, h);
                  pushToDepositIndex(entry, interest);
                }
              }
            }

            for (const auto &h : m_blockHashes)
              m_blockIndex.push(h);

            logger(INFO) << "Fast index loaded: " << m_blockHashes.size() << " blocks";
          }
          else
          {
            logger(INFO) << "Fast index not available, doing full rebuild...";

            m_blockHashes.clear();
            m_hashToHeight.clear();
            m_blockIndex.clear();
            m_transactionMap.clear();
            m_spent_keys.clear();
            m_outputs.clear();
            m_multisignatureOutputs.clear();
            m_depositIndex = DepositIndex();

            topHeight = m_mdbxStorage->topBlockHeight();
            for (uint32_t h = 0; h <= topHeight; ++h)
            {
              cn::BinaryArray ba;
              if (m_mdbxStorage->getBlockEntry(h, ba))
              {
                if (h % 10000 == 0)
                  logger(INFO, BRIGHT_WHITE) << "Rebuilding MDBX index for Height " << h << " of " << topHeight;

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

            logger(INFO) << "Loaded " << m_blockHashes.size() << " blocks from MDBX (full rebuild)";
          }
        }

        auto mdbxEnd = std::chrono::steady_clock::now();
        auto mdbxMs = std::chrono::duration_cast<std::chrono::milliseconds>(mdbxEnd - mdbxStart).count();
        logger(INFO) << "MDBX initialization took " << mdbxMs << " ms";

        m_cacheIndex = 0;
        m_cachedEntries.clear();
        m_cachedEntries.reserve(MDBX_CACHE_SIZE);
      }
      else
#endif
      {
        if (!m_blocks.open(appendPath(config_folder, m_currency.blocksFileName()),
                           appendPath(config_folder, m_currency.blockIndexesFileName()), 1024))
        {
          logger(ERROR, BRIGHT_RED) << "Failed to open blockchain storage files";
          return false;
        }
      }

#ifdef HAVE_MDBX
      if (!m_useMdbx)
#endif
      {
        if (load_existing && !blocksEmpty())
        {
          logger(INFO) << "Loading blockchain";
          BlockCacheSerializer loader(*this, get_block_hash(blocksBack().bl), logger.getLogger());
          const std::string &blocksCacheFileName = m_currency.blocksCacheFileName();

          try
          {
            loader.load(appendPath(config_folder, blocksCacheFileName));

            if (!loader.loaded())
            {
              std::string blockCacheBkpFileName = blocksCacheFileName + ".bkp";
              loader.load(appendPath(config_folder, blockCacheBkpFileName));

              if (!loader.loaded())
              {
                logger(WARNING, BRIGHT_YELLOW) << " No actual blockchain cache found, rebuilding internal structures";
                if (!rebuildCache())
                {
                  logger(ERROR, BRIGHT_RED) << "Failed to rebuild cache";
                  return false;
                }
              }
            }

            uint64_t checkBlockHeight = 24732;
            uint64_t checkMinimum = 13000000000000;
            if (!m_testnet && blocksSize() > checkBlockHeight &&
                blocksAt(checkBlockHeight).already_generated_coins < checkMinimum)
            {
              logger(WARNING, BRIGHT_YELLOW) << "Invalid blocks cache, rebuilding internal structures";
              if (!rebuildBlocks())
              {
                logger(WARNING, BRIGHT_YELLOW) << "Impossible to rebuild";
                return false;
              }
            }

            if (m_blockchainIndexesEnabled)
            {
              loadBlockchainIndices();
            }
          }
          catch (const std::exception &)
          {
            logger(ERROR, BRIGHT_RED) << "Error loading blockchain cache";

            logger(WARNING, BRIGHT_YELLOW) << "Attempting to rebuild cache after load error";
            try
            {
              if (!rebuildCache())
              {
                logger(ERROR, BRIGHT_RED) << "Failed to rebuild cache";
                return false;
              }
            }
            catch (const std::exception &)
            {
              logger(ERROR, BRIGHT_RED) << "Failed to rebuild cache";
              return false;
            }
          }
        }
        else
        {
          blocksClear();
        }
      }

      if (blocksEmpty())
      {
        logger(INFO, BRIGHT_WHITE) << "Blockchain not loaded, generating genesis block.";

        try
        {
          block_verification_context bvc = boost::value_initialized<block_verification_context>();
          pushBlock(m_currency.genesisBlock(), get_block_hash(m_currency.genesisBlock()), bvc, 0);
          if (bvc.m_verification_failed)
          {
            logger(ERROR, BRIGHT_RED) << "Failed to add genesis block to blockchain";
            return false;
          }
        }
        catch (const std::exception &)
        {
          logger(ERROR, BRIGHT_RED) << "Error creating genesis block";
          return false;
        }
      }
      else
      {
#ifdef HAVE_MDBX
        if (!m_useMdbx)
#endif
        {
          crypto::Hash firstBlockHash = get_block_hash(blocksAt(0).bl);
          if (!(firstBlockHash == m_currency.genesisBlockHash()))
          {
            logger(ERROR, BRIGHT_RED) << "Failed to init: genesis block mismatch. ...";
            return false;
          }
        }
      }

#ifdef HAVE_MDBX
    mdbx_initialized:
#endif

      try
      {
        uint32_t lastValidCheckpointHeight = 0;
        if (!checkCheckpoints(lastValidCheckpointHeight))
        {
          logger(WARNING, BRIGHT_YELLOW) << "Invalid checkpoint. Rollback blockchain to last valid checkpoint at height "
                                         << lastValidCheckpointHeight;
          rollbackBlockchainTo(lastValidCheckpointHeight);
        }
      }
      catch (const std::exception &)
      {
        logger(ERROR, BRIGHT_RED) << "Error checking/rolling back checkpoints";
        return false;
      }

      try
      {
        if (!m_upgradeDetectorV2.init() || !m_upgradeDetectorV3.init() ||
            !m_upgradeDetectorV4.init() || !m_upgradeDetectorV7.init() ||
            !m_upgradeDetectorV8.init())
        {
          logger(ERROR, BRIGHT_RED) << "Failed to initialize one or more upgrade detectors";
          return false;
        }
      }
      catch (const std::exception &e)
      {
        logger(ERROR, BRIGHT_RED) << "Error initializing upgrade detectors: " << e.what();
        return false;
      }

      update_next_comulative_size_limit();

#ifdef HAVE_MDBX
      if (m_useMdbx)
      {
        logger(INFO, BRIGHT_GREEN)
            << "Blockchain initialized. Local Height: " << blocksSize() - 1 << " [MDBX backend is active]";
      }
      else
#endif
      {
        uint64_t timestamp_diff = time(nullptr) - blocksBack().bl.timestamp;
        if (!blocksBack().bl.timestamp)
          timestamp_diff = time(nullptr) - 1341378000;

        logger(INFO, BRIGHT_GREEN)
            << "Blockchain initialized. last block: " << blocksSize() - 1 << ", "
            << common::timeIntervalToString(timestamp_diff)
            << " time ago, current difficulty: " << getDifficultyForNextBlock();
      }

      return true;
    }
    catch (const std::exception &)
    {
      logger(ERROR, BRIGHT_RED) << "Error initializing blockchain";
      return false;
    }
  }

  bool Blockchain::checkCheckpoints(uint32_t &lastValidCheckpointHeight)
  {
    std::vector<uint32_t> checkpointHeights = m_checkpoints.getCheckpointHeights();
    for (const auto &checkpointHeight : checkpointHeights)
    {
      if (blocksSize() <= checkpointHeight)
      {
        return true;
      }

      if (m_checkpoints.check_block(checkpointHeight, getBlockIdByHeight(checkpointHeight)))
      {
        lastValidCheckpointHeight = checkpointHeight;
      }
      else
      {
        return false;
      }
    }
    logger(INFO, BRIGHT_WHITE) << "Checkpoints passed";
    return true;
  }

  bool Blockchain::rebuildCache()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    logger(INFO, BRIGHT_WHITE) << "Rebuilding cache";

    std::chrono::steady_clock::time_point timePoint = std::chrono::steady_clock::now();
    try
    {
#ifdef HAVE_MDBX
      if (m_useMdbx)
      {
        m_blockHashes.clear();
        m_hashToHeight.clear();
      }
      else
#endif
        m_blockIndex.clear();

      m_transactionMap.clear();
      m_spent_keys.clear();
      m_outputs.clear();
      m_multisignatureOutputs.clear();

      for (uint32_t b = 0; b < blocksSize(); ++b)
      {
        if (b % 1000 == 0)
        {
          logger(INFO, BRIGHT_WHITE) << "Rebuilding Cache for Height " << b << " of " << blocksSize();
        }

        const BlockEntry &block = blocksAt(b);
        crypto::Hash blockHash = get_block_hash(block.bl);

#ifdef HAVE_MDBX
        if (m_useMdbx)
        {
          m_blockHashes.push_back(blockHash);
          m_hashToHeight[blockHash] = b;
        }
        else
#endif
          m_blockIndex.push(blockHash);

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
      logger(INFO, BRIGHT_WHITE) << "Rebuilding internal structures took: " << duration.count();
      logger(INFO, BRIGHT_GREEN) << "Cache rebuilt successfully";
      return true;
    }
    catch (const std::exception &)
    {
      logger(ERROR, BRIGHT_RED) << "Error rebuilding cache";
      return false;
    }
    catch (...)
    {
      logger(ERROR, BRIGHT_RED) << "Unknown error rebuilding cache";
      return false;
    }
  }

  bool Blockchain::rebuildBlocks()
  {
#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      logger(ERROR, BRIGHT_RED) << "rebuildBlocks not supported with MDBX backend";
      return false;
    }
#endif
    logger(INFO, BRIGHT_WHITE) << "Rebuilding cache";

    try
    {
      std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
      uint64_t alreadyGeneratedCoinsPrev = 0;
      for (uint32_t b = 0; b < blocksSize(); ++b)
      {
        if (b % 10000 == 0)
          logger(INFO, BRIGHT_WHITE) << "Rebuilding blocks for Height " << b << " of " << blocksSize();
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
          logger(ERROR, BRIGHT_RED) << "An error occurred";
          return false;
        }
        uint64_t alreadyGeneratedCoins = alreadyGeneratedCoinsPrev + emissionChange + interest;
        block.already_generated_coins = alreadyGeneratedCoins;
        m_blocks.replace(b, block);
        alreadyGeneratedCoinsPrev = alreadyGeneratedCoins;
      }

      std::chrono::duration<double> duration = std::chrono::steady_clock::now() - startTime;
      logger(INFO, BRIGHT_WHITE) << "Rebuilding blocks took: " << duration.count();
      storeCache();
      m_blocks.close();
      return m_blocks.open(appendPath(m_config_folder, m_currency.blocksFileName()), appendPath(m_config_folder, m_currency.blockIndexesFileName()), 1024);
    }
    catch (const std::exception &)
    {
      logger(ERROR, BRIGHT_RED) << "Error rebuilding blocks";
      try
      {
        m_blocks.close();
        m_blocks.open(appendPath(m_config_folder, m_currency.blocksFileName()),
                      appendPath(m_config_folder, m_currency.blockIndexesFileName()), 1024);
      }
      catch (...)
      {
        logger(ERROR, BRIGHT_RED) << "Failed to reopen blockchain files after error";
      }
      return false;
    }
  }

  bool Blockchain::storeCache()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      if (m_mdbxStorage)
      {
        std::vector<uint8_t> buf;

        m_mdbxStorage->putMeta("idx_hashes",
                               std::vector<uint8_t>((uint8_t *)m_blockHashes.data(),
                                                    (uint8_t *)m_blockHashes.data() + m_blockHashes.size() * sizeof(crypto::Hash)));

        buf.clear();
        for (const auto &p : m_hashToHeight)
        {
          buf.insert(buf.end(), (uint8_t *)p.first.data, (uint8_t *)p.first.data + sizeof(crypto::Hash));
          uint32_t h = p.second;
          buf.insert(buf.end(), (uint8_t *)&h, (uint8_t *)&h + sizeof(h));
        }
        m_mdbxStorage->putMeta("idx_hash2height", buf);

        buf.clear();
        for (const auto &kv : m_transactionMap)
        {
          buf.insert(buf.end(), (uint8_t *)kv.first.data, (uint8_t *)kv.first.data + sizeof(crypto::Hash));
          uint64_t packed = (static_cast<uint64_t>(kv.second.block) << 16) | kv.second.transaction;
          buf.insert(buf.end(), (uint8_t *)&packed, (uint8_t *)&packed + sizeof(packed));
        }
        m_mdbxStorage->putMeta("idx_txmap", buf);

        buf.clear();
        for (const auto &p : m_spent_keys)
        {
          buf.insert(buf.end(), (uint8_t *)p.first.data, (uint8_t *)p.first.data + sizeof(crypto::KeyImage));
          uint32_t h = p.second;
          buf.insert(buf.end(), (uint8_t *)&h, (uint8_t *)&h + sizeof(h));
        }
        m_mdbxStorage->putMeta("idx_spentkeys", buf);

        uint32_t topHeight = static_cast<uint32_t>(m_blockHashes.size() - 1);
        m_mdbxStorage->putMeta("idx_topheight",
                               std::vector<uint8_t>((uint8_t *)&topHeight, (uint8_t *)&topHeight + sizeof(topHeight)));

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

        buf.clear();
        buf.reserve(sizeof(uint64_t) * m_depositIndex.size());
        for (size_t i = 0; i < m_depositIndex.size(); ++i)
        {
          uint64_t val = m_depositIndex.depositAmountAtHeight(static_cast<uint32_t>(i));
          buf.insert(buf.end(), (uint8_t *)&val, (uint8_t *)&val + sizeof(val));
        }
        m_mdbxStorage->putMeta("idx_deposits", buf);
        m_mdbxStorage->flush();
      }
      logger(INFO, BRIGHT_GREEN) << "MDBX index saved successfully.";
      return true;
    }
#endif

    logger(INFO, BRIGHT_WHITE) << "Saving blockchain...";
    BlockCacheSerializer ser(*this, getTailId(), logger.getLogger());
    const std::string &blocksCacheFileName = m_currency.blocksCacheFileName();
    std::string blockCacheBkpFileName = blocksCacheFileName + ".bkp";

    try
    {
      std::rename(blocksCacheFileName.c_str(), blockCacheBkpFileName.c_str());
      if (!ser.save(appendPath(m_config_folder, blocksCacheFileName)))
      {
        logger(ERROR, BRIGHT_RED) << "Failed to save blockchain cache";
        return false;
      }
      logger(INFO, BRIGHT_GREEN) << "The Blockchain was successfully saved.";
      return true;
    }
    catch (const std::exception &)
    {
      logger(ERROR, BRIGHT_RED) << "Failed to save blockchain cache";
      return false;
    }
  }

  bool Blockchain::deinit()
  {
    bool cacheStored = false, indicesStored = true;
    try
    {
      cacheStored = storeCache();
      if (m_blockchainIndexesEnabled)
        indicesStored = storeBlockchainIndices();
    }
    catch (const std::exception &)
    {
      logger(ERROR, BRIGHT_RED) << "Error occurred during blockchain deinit";
    }
    assert(m_messageQueueList.empty());
    return cacheStored && indicesStored;
  }

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

    block_verification_context bvc = boost::value_initialized<block_verification_context>();
    addNewBlock(b, bvc);
    return bvc.m_added_to_main_chain && !bvc.m_verification_failed;
  }

  crypto::Hash Blockchain::getTailId(uint32_t &height)
  {
    assert(!blocksEmpty());
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    height = getCurrentBlockchainHeight() - 1;
    return getTailId();
  }

  crypto::Hash Blockchain::getTailId()
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
#ifdef HAVE_MDBX
    if (m_useMdbx && m_mdbxStorage)
    {
      if (m_blockHashes.empty())
        return NULL_HASH;
      return m_blockHashes.back();
    }
#endif
    return m_blocks.empty() ? NULL_HASH : m_blockIndex.getTailId();
  }

  std::vector<crypto::Hash> Blockchain::buildSparseChain()
  {
    uint32_t currentHeight;
    crypto::Hash tailId;
    {
      std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
      assert(blocksSize() != 0);
      currentHeight = static_cast<uint32_t>(blocksSize());
      tailId = getTailId();
    }

    {
      std::lock_guard<std::mutex> cacheLock(m_sparseChainCacheMutex);
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastSparseChainUpdate).count();
      uint32_t heightDelta = currentHeight > m_cachedSparseChainHeight ? currentHeight - m_cachedSparseChainHeight : m_cachedSparseChainHeight - currentHeight;
      if (m_sparseChainCacheValid && elapsed < SPARSE_CHAIN_CACHE_DURATION_SECONDS && heightDelta < SPARSE_CHAIN_CACHE_BLOCK_DELTA && !m_cachedSparseChain.empty())
        return m_cachedSparseChain;
    }

    std::vector<crypto::Hash> result = doBuildSparseChainUnlocked(tailId);

    {
      std::lock_guard<std::mutex> cacheLock(m_sparseChainCacheMutex);
      m_cachedSparseChain = result;
      m_cachedSparseChainHeight = currentHeight;
      m_lastSparseChainUpdate = std::chrono::steady_clock::now();
      m_sparseChainCacheValid = true;
    }
    return result;
  }

  std::vector<crypto::Hash> Blockchain::buildSparseChain(const crypto::Hash &startBlockId)
  {
    {
      std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
      if (!haveBlock(startBlockId))
        return std::vector<crypto::Hash>();
    }
    return doBuildSparseChainUnlocked(startBlockId);
  }

  std::vector<crypto::Hash> Blockchain::doBuildSparseChain(const crypto::Hash &startBlockId) const
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);

#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      if (m_blockHashes.empty())
        return std::vector<crypto::Hash>();

      const size_t n = m_blockHashes.size() - 1; // height of top block

      // Helper: build sparse chain from in‑memory hashes for a given height
      auto buildFromHeight = [&](size_t h) -> std::vector<crypto::Hash>
      {
        std::vector<crypto::Hash> chain;
        chain.reserve(32);
        chain.push_back(m_blockHashes[h]); // top
        if (h > 0)
        {
          uint32_t step = 1;
          while (step < h)
          {
            chain.push_back(m_blockHashes[h - step]);
            step *= 2;
          }
          chain.push_back(m_blockHashes[0]); // genesis last
        }
        return chain;
      };

      // Check if startBlockId is in the main chain
      size_t height = 0;
      bool found = false;
      for (size_t i = 0; i < m_blockHashes.size(); ++i)
      {
        if (m_blockHashes[i] == startBlockId)
        {
          height = i;
          found = true;
          break;
        }
      }

      if (found)
        return buildFromHeight(height);

      // Not in main chain – must be in alternative chains
      assert(m_alternative_chains.count(startBlockId) > 0);

      // Walk up alternative chain until we hit the main chain
      std::vector<crypto::Hash> altPart;
      crypto::Hash currentId = startBlockId;
      crypto::Hash blockchainAncestor;
      while (m_alternative_chains.count(currentId))
      {
        altPart.push_back(currentId);
        blockchainAncestor = m_alternative_chains.at(currentId).bl.previousBlockHash;
        currentId = blockchainAncestor;
      }

      // blockchainAncestor should now be in the main chain
      size_t ancestorHeight = 0;
      bool ancestorFound = false;
      for (size_t i = 0; i < m_blockHashes.size(); ++i)
      {
        if (m_blockHashes[i] == blockchainAncestor)
        {
          ancestorHeight = i;
          ancestorFound = true;
          break;
        }
      }
      assert(ancestorFound);

      // Build main chain sparse part from ancestor
      std::vector<crypto::Hash> mainPart = buildFromHeight(ancestorHeight);

      // Combine: take sparse entries from altPart (powers of 2), then the main part
      // altPart is [startBlockId, …, child_of_ancestor] (most recent first)
      std::vector<crypto::Hash> result;
      result.reserve(32);
      for (size_t i = 0; i < altPart.size(); i *= 2)
        result.push_back(altPart[i]);
      if (result.back() != altPart.back())
        result.push_back(altPart.back());

      // Append main part (already includes genesis)
      result.insert(result.end(), mainPart.begin(), mainPart.end());
      return result;
    }
#endif

    // Original non‑MDBX path
    assert(m_blockIndex.size() != 0);

    std::vector<crypto::Hash> sparseChain;

    if (m_blockIndex.hasBlock(startBlockId))
    {
      sparseChain = m_blockIndex.buildSparseChain(startBlockId);
    }
    else
    {
      assert(m_alternative_chains.count(startBlockId) > 0);

      std::vector<crypto::Hash> alternativeChain;
      crypto::Hash blockchainAncestor;
      for (auto it = m_alternative_chains.find(startBlockId);
           it != m_alternative_chains.end();
           it = m_alternative_chains.find(blockchainAncestor))
      {
        alternativeChain.emplace_back(it->first);
        blockchainAncestor = it->second.bl.previousBlockHash;
      }

      for (size_t i = 1; i <= alternativeChain.size(); i *= 2)
      {
        sparseChain.emplace_back(alternativeChain[i - 1]);
      }

      assert(!sparseChain.empty());
      assert(m_blockIndex.hasBlock(blockchainAncestor));
      std::vector<crypto::Hash> sparseMainChain = m_blockIndex.buildSparseChain(blockchainAncestor);
      sparseChain.reserve(sparseChain.size() + sparseMainChain.size());
      std::copy(sparseMainChain.begin(), sparseMainChain.end(), std::back_inserter(sparseChain));
    }

    return sparseChain;
  }

  crypto::Hash Blockchain::getBlockIdByHeight(uint32_t height)
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      assert(height < m_blockHashes.size());
      return m_blockHashes[height];
    }
#endif
    assert(height < m_blockIndex.size());
    return m_blockIndex.getBlockId(height);
  }

  bool Blockchain::getBlockByHash(const crypto::Hash &blockHash, Block &b)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      auto it = m_hashToHeight.find(blockHash);
      if (it != m_hashToHeight.end())
      {
        b = blocksAt(it->second).bl;
        return true;
      }
      auto blockByHashIterator = m_alternative_chains.find(blockHash);
      if (blockByHashIterator != m_alternative_chains.end())
      {
        b = blockByHashIterator->second.bl;
        return true;
      }
      return false;
    }
#endif

    uint32_t height = 0;
    if (m_blockIndex.getBlockHeight(blockHash, height))
    {
      b = blocksAt(height).bl;
      return true;
    }

    auto blockByHashIterator = m_alternative_chains.find(blockHash);
    if (blockByHashIterator != m_alternative_chains.end())
    {
      b = blockByHashIterator->second.bl;
      return true;
    }
    return false;
  }

  bool Blockchain::getBlockHeight(const crypto::Hash &blockId, uint32_t &blockHeight)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lock(m_blockchain_lock);
#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      auto it = m_hashToHeight.find(blockId);
      if (it != m_hashToHeight.end())
      {
        blockHeight = it->second;
        return true;
      }
      return false;
    }
#endif
    return m_blockIndex.getBlockHeight(blockId, blockHeight);
  }

  difficulty_type Blockchain::getDifficultyForNextBlock()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    std::vector<uint64_t> timestamps;
    std::vector<difficulty_type> commulative_difficulties;

    size_t sz = blocksSize();
    uint8_t BlockMajorVersion = getBlockMajorVersionForHeight(static_cast<uint32_t>(sz));
    size_t offset = sz - std::min(sz, static_cast<uint64_t>(m_currency.difficultyBlocksCountByBlockVersion(BlockMajorVersion)));
    if (offset == 0)
      ++offset;

    for (; offset < sz; offset++)
    {
      cn::BlockHeaderPOD hdr = getBlockHeader(offset);
      timestamps.push_back(hdr.timestamp);
      commulative_difficulties.push_back(hdr.cumulativeDifficulty);
    }

    uint64_t block_index = sz;
    uint8_t block_major_version = get_block_major_version_for_height(block_index + 1);

    difficulty_type currentDifficulty = 0;
    if (block_major_version >= 8)
      currentDifficulty = m_currency.nextDifficultyLWMA1(timestamps, commulative_difficulties, block_index);
    else if (block_major_version >= 4)
      currentDifficulty = m_currency.nextDifficultyLWMA3(timestamps, commulative_difficulties);
    else
      currentDifficulty = m_currency.nextDifficulty(block_major_version, block_index, timestamps, commulative_difficulties);

    // If there aren't enough blocks for the algorithm, default to genesis difficulty
    if (currentDifficulty == 0)
      currentDifficulty = 1;

    return currentDifficulty;
  }

  uint64_t Blockchain::getBlockTimestamp(uint32_t height)
  {
    assert(height < blocksSize());
    return getBlockHeader(height).timestamp;
  }

  uint64_t Blockchain::getCoinsInCirculation()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    if (blocksEmpty())
      return 0;
    return getBlockHeader(blocksSize() - 1).alreadyGeneratedCoins;
  }

  uint64_t Blockchain::coinsEmittedAtHeight(uint64_t height)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

#ifdef HAVE_MDBX
    if (m_useMdbx && m_mdbxStorage)
    {
      cn::BinaryArray ba;
      if (!m_mdbxStorage->getBlockEntry((uint32_t)height, ba))
        return 0;
      BlockEntry entry;
      if (!cn::fromBinaryArray(entry, ba))
        return 0;
      return entry.already_generated_coins;
    }
#endif

    if (blocksEmpty() || height >= blocksSize())
      return 0;
    return blocksAt(height).already_generated_coins;
  }

  difficulty_type Blockchain::difficultyAtHeight(uint64_t height)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

#ifdef HAVE_MDBX
    if (m_useMdbx && m_mdbxStorage)
    {
      cn::BinaryArray ba;
      if (!m_mdbxStorage->getBlockEntry((uint32_t)height, ba))
        return 0;
      BlockEntry entry;
      if (!cn::fromBinaryArray(entry, ba))
        return 0;
      if (height < 1)
        return entry.cumulative_difficulty;

      cn::BinaryArray ba_prev;
      if (!m_mdbxStorage->getBlockEntry((uint32_t)(height - 1), ba_prev))
        return entry.cumulative_difficulty;
      BlockEntry entry_prev;
      if (!cn::fromBinaryArray(entry_prev, ba_prev))
        return entry.cumulative_difficulty;

      return entry.cumulative_difficulty - entry_prev.cumulative_difficulty;
    }
#endif

    const auto &current = blocksAt(height);
    if (height < 1)
      return current.cumulative_difficulty;
    return current.cumulative_difficulty - blocksAt(height - 1).cumulative_difficulty;
  }

  uint8_t Blockchain::get_block_major_version_for_height(uint64_t height) const
  {
    if (height > m_upgradeDetectorV8.upgradeHeight())
      return m_upgradeDetectorV8.targetVersion();
    else if (height > m_upgradeDetectorV7.upgradeHeight())
      return m_upgradeDetectorV7.targetVersion();
    else if (height > m_upgradeDetectorV4.upgradeHeight())
      return m_upgradeDetectorV4.targetVersion();
    else if (height > m_upgradeDetectorV3.upgradeHeight())
      return m_upgradeDetectorV3.targetVersion();
    else if (height > m_upgradeDetectorV2.upgradeHeight())
      return m_upgradeDetectorV2.targetVersion();
    return BLOCK_MAJOR_VERSION_1;
  }

  bool Blockchain::rollback_blockchain_switching(const std::list<Block> &original_chain, size_t rollback_height)
  {
    try
    {
      std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
      for (size_t i = blocksSize() - 1; i >= rollback_height; i--)
        popBlock(get_block_hash(blocksBack().bl));

      auto height = static_cast<uint32_t>(rollback_height - 1);
      for (const auto &bl : original_chain)
      {
        block_verification_context bvc = boost::value_initialized<block_verification_context>();
        if (!pushBlock(bl, get_block_hash(bl), bvc, ++height) || !bvc.m_added_to_main_chain)
        {
          logger(ERROR, BRIGHT_RED) << "PANIC!!! failed to add (again) block while chain switching during the rollback!";
          return false;
        }
      }
      logger(INFO, BRIGHT_WHITE) << "Rollback success.";
      return true;
    }
    catch (const std::exception &)
    {
      logger(ERROR, BRIGHT_RED) << "Error during blockchain rollback";
      return false;
    }
  }

  bool Blockchain::switch_to_alternative_blockchain(const std::list<crypto::Hash> &alt_chain, bool discard_disconnected_chain)
  {
    try
    {
      std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
      if (alt_chain.empty())
      {
        logger(ERROR, BRIGHT_RED) << "switch_to_alternative_blockchain: empty chain passed";
        return false;
      }

      uint32_t split_height = m_alternative_chains[alt_chain.front()].height;
      if (!(blocksSize() > split_height))
      {
        logger(ERROR, BRIGHT_RED) << "switch_to_alternative_blockchain: blockchain size is lower than split height";
        return false;
      }

      std::vector<crypto::Hash> mainChainTxHashes, altChainTxHashes;
      for (size_t i = blocksSize() - 1; i >= split_height; i--)
      {
        const Block &b = blocksAt(i).bl;
        std::copy(b.transactionHashes.begin(), b.transactionHashes.end(), std::inserter(mainChainTxHashes, mainChainTxHashes.end()));
      }
      for (const auto &hash : alt_chain)
      {
        const Block &b = m_alternative_chains[hash].bl;
        std::copy(b.transactionHashes.begin(), b.transactionHashes.end(), std::inserter(altChainTxHashes, altChainTxHashes.end()));
      }
      for (const auto &tx_hash : mainChainTxHashes)
      {
        if (std::find(altChainTxHashes.begin(), altChainTxHashes.end(), tx_hash) == altChainTxHashes.end())
        {
          logger(ERROR, BRIGHT_RED) << "Attempting to switch to an alternate chain, but it lacks transaction " << common::podToHex(tx_hash) << " from main chain, rejected";
          mainChainTxHashes.clear();
          mainChainTxHashes.shrink_to_fit();
          altChainTxHashes.clear();
          altChainTxHashes.shrink_to_fit();
          return false;
        }
      }

      for (const auto &hash : alt_chain)
      {
        const Block &b = m_alternative_chains[hash].bl;
        if (!checkBlockVersion(b, get_block_hash(b)))
          return false;
      }

      std::list<Block> disconnected_chain;
      for (size_t i = blocksSize() - 1; i >= split_height; i--)
      {
        Block b = blocksAt(i).bl;
        popBlock(get_block_hash(b));
        disconnected_chain.push_front(b);
      }

      uint32_t height = split_height - 1;
      for (auto alt_ch_iter = alt_chain.begin(); alt_ch_iter != alt_chain.end(); alt_ch_iter++)
      {
        auto ch_ent = *alt_ch_iter;
        block_verification_context bvc = boost::value_initialized<block_verification_context>();
        const Block &b = m_alternative_chains[ch_ent].bl;
        if (!pushBlock(b, get_block_hash(b), bvc, ++height) || !bvc.m_added_to_main_chain)
        {
          logger(INFO, BRIGHT_WHITE) << "Failed to switch to alternative blockchain";
          rollback_blockchain_switching(disconnected_chain, split_height);
          m_orthanBlocksIndex.remove(b);
          m_alternative_chains.erase(ch_ent);
          for (auto alt_ch_to_orph_iter = ++alt_ch_iter; alt_ch_to_orph_iter != alt_chain.end(); alt_ch_to_orph_iter++)
          {
            const Block &bl = m_alternative_chains[*alt_ch_to_orph_iter].bl;
            m_orthanBlocksIndex.remove(bl);
            m_alternative_chains.erase(*alt_ch_to_orph_iter);
          }
          return false;
        }
      }

      if (!discard_disconnected_chain)
      {
        for (const auto &old_ch_ent : disconnected_chain)
        {
          block_verification_context bvc = boost::value_initialized<block_verification_context>();
          if (!handle_alternative_block(old_ch_ent, get_block_hash(old_ch_ent), bvc, false))
          {
            logger(ERROR, BRIGHT_RED) << "Failed to push ex-main chain blocks to alternative chain ";
            rollback_blockchain_switching(disconnected_chain, split_height);
            return false;
          }
        }
      }

      std::vector<crypto::Hash> blocksFromCommonRoot;
      blocksFromCommonRoot.reserve(alt_chain.size() + 1);
      const Block &b = m_alternative_chains[alt_chain.front()].bl;
      blocksFromCommonRoot.push_back(b.previousBlockHash);
      for (const auto &ch_ent : alt_chain)
      {
        const Block &bl = m_alternative_chains[ch_ent].bl;
        blocksFromCommonRoot.push_back(get_block_hash(bl));
        m_orthanBlocksIndex.remove(bl);
        m_alternative_chains.erase(ch_ent);
      }
      sendMessage(BlockchainMessage(ChainSwitchMessage(std::move(blocksFromCommonRoot))));
      logger(INFO, BRIGHT_GREEN) << "Succesfully reorganized on height: " << split_height << ", new blockchain size: " << blocksSize();
      return true;
    }
    catch (const std::exception &)
    {
      logger(ERROR, BRIGHT_RED) << "Error during blockchain switching";
      return false;
    }
  }

  uint8_t Blockchain::getBlockMajorVersionForHeight(uint32_t height) const
  {
    if (height > m_upgradeDetectorV8.upgradeHeight())
      return m_upgradeDetectorV8.targetVersion();
    else if (height > m_upgradeDetectorV7.upgradeHeight())
      return m_upgradeDetectorV7.targetVersion();
    else if (height > m_upgradeDetectorV4.upgradeHeight())
      return m_upgradeDetectorV4.targetVersion();
    else if (height > m_upgradeDetectorV3.upgradeHeight())
      return m_upgradeDetectorV3.targetVersion();
    else if (height > m_upgradeDetectorV2.upgradeHeight())
      return m_upgradeDetectorV2.targetVersion();
    return BLOCK_MAJOR_VERSION_1;
  }

  difficulty_type Blockchain::get_next_difficulty_for_alternative_chain(const std::list<crypto::Hash> &alt_chain, const BlockEntry &bei)
  {
    std::vector<uint64_t> timestamps;
    std::vector<difficulty_type> commulative_difficulties;
    size_t sz = blocksSize();
    uint8_t BlockMajorVersion = getBlockMajorVersionForHeight(static_cast<uint32_t>(sz));

    if (alt_chain.size() < m_currency.difficultyBlocksCountByBlockVersion(BlockMajorVersion))
    {
      std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
      size_t main_chain_stop_offset = alt_chain.size() ? m_alternative_chains[alt_chain.front()].height : bei.height;
      size_t main_chain_count = m_currency.difficultyBlocksCountByBlockVersion(BlockMajorVersion) - std::min(m_currency.difficultyBlocksCountByBlockVersion(BlockMajorVersion), alt_chain.size());
      main_chain_count = std::min(main_chain_count, main_chain_stop_offset);
      size_t main_chain_start_offset = main_chain_stop_offset - main_chain_count;
      if (!main_chain_start_offset)
        ++main_chain_start_offset;

      for (; main_chain_start_offset < main_chain_stop_offset; ++main_chain_start_offset)
      {
        cn::BlockHeaderPOD hdr = getBlockHeader(main_chain_start_offset);
        timestamps.push_back(hdr.timestamp);
        commulative_difficulties.push_back(hdr.cumulativeDifficulty);
      }

      if (!((alt_chain.size() + timestamps.size()) <= m_currency.difficultyBlocksCountByBlockVersion(BlockMajorVersion)))
      {
        logger(ERROR, BRIGHT_RED) << "Internal error, alt_chain.size()[" << alt_chain.size() << "] + timestamps.size()[" << timestamps.size() << "] NOT <= ...";
        return false;
      }

      for (const auto &it : alt_chain)
      {
        const BlockEntry &blockEntry = m_alternative_chains[it];
        timestamps.push_back(blockEntry.bl.timestamp);
        commulative_difficulties.push_back(blockEntry.cumulative_difficulty);
      }
    }
    else
    {
      timestamps.resize(std::min(alt_chain.size(), m_currency.difficultyBlocksCountByBlockVersion(BlockMajorVersion)));
      commulative_difficulties.resize(std::min(alt_chain.size(), m_currency.difficultyBlocksCountByBlockVersion(BlockMajorVersion)));
      size_t count = 0, max_i = timestamps.size() - 1;
      BOOST_REVERSE_FOREACH(auto it, alt_chain)
      {
        const BlockEntry &blockEntry = m_alternative_chains[it];
        timestamps[max_i - count] = blockEntry.bl.timestamp;
        commulative_difficulties[max_i - count] = blockEntry.cumulative_difficulty;
        count++;
        if (count >= m_currency.difficultyBlocksCountByBlockVersion(BlockMajorVersion))
          break;
      }
    }

    uint64_t block_index = sz;
    uint8_t block_major_version = get_block_major_version_for_height(block_index + 1);

    if (block_major_version >= 8)
      return m_currency.nextDifficultyLWMA1(timestamps, commulative_difficulties, block_index);
    else if (block_major_version >= 4)
      return m_currency.nextDifficultyLWMA3(timestamps, commulative_difficulties);
    else
      return m_currency.nextDifficulty(block_major_version, block_index, timestamps, commulative_difficulties);
  }

  bool Blockchain::prevalidate_miner_transaction(const Block &b, uint32_t height) const
  {
    if (!(b.baseTransaction.inputs.size() == 1))
    {
      logger(ERROR, BRIGHT_RED) << "coinbase transaction in the block has no inputs";
      return false;
    }
    if (b.baseTransaction.signatures.size() > 1)
    {
      logger(ERROR, BRIGHT_RED) << " coinbase transaction in the block shouldn't have more than 1 signature. Signature count: " << b.baseTransaction.signatures.size();
      return false;
    }
    if (!(b.baseTransaction.inputs[0].type() == typeid(BaseInput)))
    {
      logger(ERROR, BRIGHT_RED) << "coinbase transaction in the block has the wrong type";
      return false;
    }

    if (boost::get<BaseInput>(b.baseTransaction.inputs[0]).blockIndex != height)
    {
      logger(INFO, BRIGHT_RED) << "The miner transaction in block has invalid height: " << boost::get<BaseInput>(b.baseTransaction.inputs[0]).blockIndex << ", expected: " << height;
      return false;
    }

    if (!(b.baseTransaction.unlockTime == height + m_currency.minedMoneyUnlockWindow()))
    {
      logger(ERROR, BRIGHT_RED) << "coinbase transaction transaction have wrong unlock time=" << b.baseTransaction.unlockTime << ", expected " << height + m_currency.minedMoneyUnlockWindow();
      return false;
    }
    if (!check_outs_valid(b.baseTransaction))
    {
      logger(INFO, BRIGHT_RED) << "miner transaction have invalid outputs";
      return false;
    }
    if (!check_outs_overflow(b.baseTransaction))
    {
      logger(INFO, BRIGHT_RED) << "miner transaction have money overflow in block " << get_block_hash(b);
      return false;
    }
    return true;
  }

  bool Blockchain::validate_miner_transaction(const Block &b, uint32_t height, size_t cumulativeBlockSize,
                                              uint64_t alreadyGeneratedCoins, uint64_t fee, uint64_t &reward, int64_t &emissionChange)
  {
    uint64_t minerReward = 0;
    for (auto &o : b.baseTransaction.outputs)
      minerReward += o.amount;

    std::vector<size_t> lastBlocksSizes;
    get_last_n_blocks_sizes(lastBlocksSizes, m_currency.rewardBlocksWindow());
    size_t blocksSizeMedian = common::medianValue(lastBlocksSizes);

    if (!m_currency.getBlockReward(blocksSizeMedian, cumulativeBlockSize, alreadyGeneratedCoins, fee, height, reward, emissionChange))
    {
      logger(INFO, BRIGHT_WHITE) << "block size " << cumulativeBlockSize << " is bigger than allowed for this blockchain";
      return false;
    }

    if (minerReward > reward && (minerReward - reward) > 10)
    {
      logger(ERROR, BRIGHT_RED) << "Coinbase transaction spend too much money: " << m_currency.formatAmount(minerReward) << ", block reward is " << m_currency.formatAmount(reward);
      return false;
    }
    else if (minerReward < reward)
    {
      logger(ERROR, BRIGHT_RED) << "Coinbase transaction doesn't use full amount of block reward: spent " << m_currency.formatAmount(minerReward) << ", block reward is " << m_currency.formatAmount(reward) << ", fee is " << fee;
      return false;
    }
    return true;
  }

  bool Blockchain::getBackwardBlocksSize(size_t from_height, std::vector<size_t> &sz, size_t count)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    if (!(from_height < blocksSize()))
    {
      logger(ERROR, BRIGHT_RED) << "Internal error: get_backward_blocks_sizes called with from_height=" << from_height << ", blockchain height = " << blocksSize();
      return false;
    }

    size_t start_offset = (from_height + 1) - std::min((from_height + 1), count);
    for (size_t i = start_offset; i != from_height + 1; i++)
      sz.push_back(blocksAt(i).block_cumulative_size);
    return true;
  }

  bool Blockchain::get_last_n_blocks_sizes(std::vector<size_t> &sz, size_t count)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    if (!blocksSize())
      return true;
    return getBackwardBlocksSize(blocksSize() - 1, sz, count);
  }

  uint64_t Blockchain::getCurrentCumulativeBlocksizeLimit() const { return m_current_block_cumul_sz_limit; }

  bool Blockchain::complete_timestamps_vector(uint64_t start_top_height, std::vector<uint64_t> &timestamps)
  {
    if (timestamps.size() >= m_currency.timestampCheckWindow())
      return true;

    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    size_t need_elements = m_currency.timestampCheckWindow() - timestamps.size();

    if (!(start_top_height < blocksSize()))
    {
      logger(ERROR, BRIGHT_RED) << "internal error: passed start_height = " << start_top_height << " not less then blocksSize()=" << blocksSize();
      return false;
    }

    size_t stop_offset = start_top_height > need_elements ? start_top_height - need_elements : 0;
    do
    {
      timestamps.push_back(blocksAt(start_top_height).bl.timestamp);
      if (start_top_height == 0)
        break;
      --start_top_height;
    } while (start_top_height != stop_offset);
    return true;
  }

  bool Blockchain::handle_alternative_block(const Block &b, const crypto::Hash &id, block_verification_context &bvc, bool sendNewAlternativeBlockMessage)
  {
    try
    {
      std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

      auto block_height = get_block_height(b);
      if (block_height == 0)
      {
        logger(ERROR, BRIGHT_RED) << "Block with id: " << common::podToHex(id) << " (as alternative) have wrong miner transaction";
        bvc.m_verification_failed = true;
        return false;
      }

      m_checkpoints.load_checkpoints_from_dns();

      if (!m_checkpoints.is_alternative_block_allowed(getCurrentBlockchainHeight(), block_height))
      {
        logger(DEBUGGING) << "Block with id: " << id << " can't be accepted for alternative chain, block height: " << block_height << " blockchain height: " << getCurrentBlockchainHeight();
        bvc.m_verification_failed = true;
        return false;
      }

      if (!checkBlockVersion(b, id))
      {
        bvc.m_verification_failed = true;
        return false;
      }

      size_t cumulativeSize;
      if (!getBlockCumulativeSize(b, cumulativeSize))
      {
        logger(DEBUGGING) << "Block with id: " << id << " has at least one unknown transaction. Cumulative size is calculated imprecisely";
      }
      if (!checkCumulativeBlockSize(id, cumulativeSize, block_height))
      {
        bvc.m_verification_failed = true;
        return false;
      }

      uint32_t mainPrevHeight = 0;
      bool mainPrev = false;
#ifdef HAVE_MDBX
      if (m_useMdbx)
      {
        auto it = m_hashToHeight.find(b.previousBlockHash);
        if (it != m_hashToHeight.end())
        {
          mainPrev = true;
          mainPrevHeight = it->second;
        }
      }
      else
#endif
      {
        mainPrev = m_blockIndex.getBlockHeight(b.previousBlockHash, mainPrevHeight);
      }

      const auto it_prev = m_alternative_chains.find(b.previousBlockHash);

      if (it_prev != m_alternative_chains.end() || mainPrev)
      {
        blocks_ext_by_hash::iterator alt_it = it_prev;
        std::list<crypto::Hash> alt_chain;
        std::vector<uint64_t> timestamps;
        while (alt_it != m_alternative_chains.end())
        {
          alt_chain.push_front(alt_it->first);
          timestamps.push_back(alt_it->second.bl.timestamp);
          alt_it = m_alternative_chains.find(alt_it->second.bl.previousBlockHash);
        }

        if (alt_chain.size())
        {
          const BlockEntry &bei = m_alternative_chains[alt_chain.front()];
          if (!(blocksSize() > bei.height))
          {
            logger(ERROR, BRIGHT_RED) << "main blockchain wrong height";
            return false;
          }
          crypto::Hash h = NULL_HASH;
          get_block_hash(blocksAt(bei.height - 1).bl, h);
          if (!(h == bei.bl.previousBlockHash))
          {
            logger(ERROR, BRIGHT_RED) << "alternative chain have wrong connection to main chain";
            return false;
          }
          complete_timestamps_vector(bei.height - 1, timestamps);
        }
        else
        {
          if (!mainPrev)
          {
            logger(ERROR, BRIGHT_RED) << "internal error: broken imperative condition it_main_prev != m_blocks_index.end()";
            return false;
          }
          complete_timestamps_vector(mainPrevHeight, timestamps);
        }

        if (!check_block_timestamp(timestamps, b))
        {
          logger(INFO, BRIGHT_RED) << "Block with id: " << id << " for alternative chain, have invalid timestamp: " << b.timestamp;
          bvc.m_verification_failed = true;
          return false;
        }

        BlockEntry bei = boost::value_initialized<BlockEntry>();
        bei.bl = b;
        bei.height = alt_chain.size() ? it_prev->second.height + 1 : mainPrevHeight + 1;

        bool is_a_checkpoint;
        if (!m_checkpoints.check_block(bei.height, id, is_a_checkpoint))
        {
          logger(ERROR, BRIGHT_RED) << "Checkpoint validaton failure";
          bvc.m_verification_failed = true;
          return false;
        }

        TransactionExtraMergeMiningTag mmTag;
        if (getMergeMiningTagFromExtra(bei.bl.baseTransaction.extra, mmTag) && bei.height >= cn::parameters::UPGRADE_HEIGHT_V6)
        {
          logger(ERROR, BRIGHT_RED) << "Merge mining tag was found in extra of miner transaction";
          return false;
        }

        m_is_in_checkpoint_zone = false;
        difficulty_type current_diff = get_next_difficulty_for_alternative_chain(alt_chain, bei);
        if (!current_diff)
        {
          logger(ERROR, BRIGHT_RED) << "!!!!!!! DIFFICULTY OVERHEAD !!!!!!!";
          return false;
        }

        crypto::Hash proof_of_work = NULL_HASH;
        if (!m_currency.checkProofOfWork(m_cn_context, bei.bl, current_diff, proof_of_work))
        {
          logger(INFO, BRIGHT_RED) << "Block with id: " << id << " for alternative chain, have not enough proof of work: " << proof_of_work << " expected difficulty: " << current_diff;
          bvc.m_verification_failed = true;
          return false;
        }

        if (!prevalidate_miner_transaction(b, bei.height))
        {
          logger(INFO, BRIGHT_RED) << "Block with id: " << common::podToHex(id) << " (as alternative) have wrong miner transaction.";
          bvc.m_verification_failed = true;
          return false;
        }

        bei.cumulative_difficulty = alt_chain.size() ? it_prev->second.cumulative_difficulty : blocksAt(mainPrevHeight).cumulative_difficulty;
        bei.cumulative_difficulty += current_diff;

        auto i_res = m_alternative_chains.insert(blocks_ext_by_hash::value_type(id, bei));
        if (!(i_res.second))
        {
          logger(ERROR, BRIGHT_RED) << "insertion of new alternative block returned as it already exist";
          return false;
        }

        m_orthanBlocksIndex.add(bei.bl);
        alt_chain.push_back(i_res.first->first);

        if (is_a_checkpoint)
        {
          logger(INFO, BRIGHT_GREEN) << "###### REORGANIZE on height: " << m_alternative_chains[alt_chain.front()].height << " of " << blocksSize() - 1 << ", checkpoint is found in alternative chain on height " << bei.height;
          bool r = switch_to_alternative_blockchain(alt_chain, true);
          if (r)
          {
            bvc.m_added_to_main_chain = true;
            bvc.m_switched_to_alt_chain = true;
          }
          else
            bvc.m_verification_failed = true;
          return r;
        }
        else if (blocksBack().cumulative_difficulty < bei.cumulative_difficulty)
        {
          logger(INFO, BRIGHT_GREEN) << "###### REORGANIZE on height: " << m_alternative_chains[alt_chain.front()].height << " of " << blocksSize() - 1 << " with cum_difficulty " << blocksBack().cumulative_difficulty << " alternative blockchain size: " << alt_chain.size() << " with cum_difficulty " << bei.cumulative_difficulty;
          bool r = switch_to_alternative_blockchain(alt_chain, false);
          if (r)
          {
            bvc.m_added_to_main_chain = true;
            bvc.m_switched_to_alt_chain = true;
          }
          else
            bvc.m_verification_failed = true;
          return r;
        }
        else
        {
          logger(INFO, BRIGHT_BLUE) << "----- BLOCK ADDED AS ALTERNATIVE ON HEIGHT " << bei.height << " id:\t" << id << " PoW:\t" << proof_of_work << " difficulty:\t" << current_diff;
          if (sendNewAlternativeBlockMessage)
            sendMessage(BlockchainMessage(NewAlternativeBlockMessage(id)));
          return true;
        }
      }
      else
      {
        bvc.m_marked_as_orphaned = true;
        logger(INFO, BRIGHT_RED) << "Block recognized as orphaned and rejected, id = " << id;
      }
      return true;
    }
    catch (const std::exception &)
    {
      logger(ERROR, BRIGHT_RED) << "Error handling alternative block";
      bvc.m_verification_failed = true;
      return false;
    }
  }

  bool Blockchain::getBlocks(uint32_t start_offset, uint32_t count, std::list<Block> &blocks, std::list<Transaction> &txs)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    if (start_offset >= blocksSize())
      return false;

    for (size_t i = start_offset; i < start_offset + count && i < blocksSize(); i++)
    {
      blocks.push_back(blocksAt(i).bl);
      std::list<crypto::Hash> missed_ids;
      getTransactions(blocksAt(i).bl.transactionHashes, txs, missed_ids);
      if (!(!missed_ids.size()))
      {
        logger(ERROR, BRIGHT_RED) << "have missed transactions in own block in main blockchain";
        return false;
      }
    }
    return true;
  }

  bool Blockchain::getBlocks(uint32_t start_offset, uint32_t count, std::list<Block> &blocks)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    if (start_offset >= blocksSize())
      return false;
    for (uint32_t i = start_offset; i < start_offset + count && i < blocksSize(); i++)
      blocks.push_back(blocksAt(i).bl);
    return true;
  }

  bool Blockchain::handleGetObjects(NOTIFY_REQUEST_GET_OBJECTS::request &arg, NOTIFY_RESPONSE_GET_OBJECTS::request &rsp)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    rsp.current_blockchain_height = getCurrentBlockchainHeight();
    std::list<Block> blocks;
    getBlocks(arg.blocks, blocks, rsp.missed_ids);

    for (const auto &bl : blocks)
    {
      std::list<crypto::Hash> missed_tx_id;
      std::list<Transaction> txs;
      getTransactions(bl.transactionHashes, txs, rsp.missed_ids);
      if (!(!missed_tx_id.size()))
      {
        logger(ERROR, BRIGHT_RED) << "Internal error: have missed missed_tx_id.size()=" << missed_tx_id.size() << " for block id = " << get_block_hash(bl);
        return false;
      }
      rsp.blocks.push_back(block_complete_entry());
      block_complete_entry &e = rsp.blocks.back();
      e.block = asString(toBinaryArray(bl));
      for (const Transaction &tx : txs)
        e.txs.push_back(asString(toBinaryArray(tx)));
    }

    std::list<Transaction> txs;
    getTransactions(arg.txs, txs, rsp.missed_ids);
    for (const auto &tx : txs)
      rsp.txs.push_back(asString(toBinaryArray(tx)));
    return true;
  }

  bool Blockchain::getTransactionsWithOutputGlobalIndexes(const std::vector<crypto::Hash> &txs_ids, std::list<crypto::Hash> &missed_txs, std::vector<std::pair<Transaction, std::vector<uint32_t>>> &txs)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    for (const auto &tx_id : txs_ids)
    {
      auto it = m_transactionMap.find(tx_id);
      if (it == m_transactionMap.end())
        missed_txs.push_back(tx_id);
      else
      {
        const TransactionEntry &tx = transactionByIndex(it->second);
        if (!(tx.m_global_output_indexes.size()))
        {
          logger(ERROR, BRIGHT_RED) << "Internal error: global indexes for transaction " << tx_id << " is empty";
          return false;
        }
        txs.emplace_back(tx.tx, tx.m_global_output_indexes);
      }
    }
    return true;
  }

  bool Blockchain::getAlternativeBlocks(std::list<Block> &blocks)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    for (const auto &alt_bl : m_alternative_chains)
      blocks.push_back(alt_bl.second.bl);
    return true;
  }

  uint32_t Blockchain::getAlternativeBlocksCount()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return static_cast<uint32_t>(m_alternative_chains.size());
  }

  bool Blockchain::add_out_to_get_random_outs(std::vector<std::pair<TransactionIndex, uint16_t>> &amount_outs, COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount &result_outs, uint64_t amount, size_t i)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    const Transaction &tx = transactionByIndex(amount_outs[i].first).tx;
    if (!(tx.outputs.size() > amount_outs[i].second))
    {
      logger(ERROR, BRIGHT_RED) << "internal error: in global outs index, transaction out index=" << amount_outs[i].second << " more than transaction outputs = " << tx.outputs.size() << ", for tx id = " << getObjectHash(tx);
      return false;
    }
    if (!(tx.outputs[amount_outs[i].second].target.type() == typeid(KeyOutput)))
    {
      logger(ERROR, BRIGHT_RED) << "unknown tx out type";
      return false;
    }
    if (!is_tx_spendtime_unlocked(tx.unlockTime))
      return false;

    COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry &oen = *result_outs.outs.insert(result_outs.outs.end(), COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry());
    oen.global_amount_index = static_cast<uint32_t>(i);
    oen.out_key = boost::get<KeyOutput>(tx.outputs[amount_outs[i].second].target).key;
    return true;
  }

  size_t Blockchain::find_end_of_allowed_index(const std::vector<std::pair<TransactionIndex, uint16_t>> &amount_outs)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    if (amount_outs.empty())
      return 0;
    size_t i = amount_outs.size();
    do
    {
      --i;
      if (amount_outs[i].first.block + m_currency.minedMoneyUnlockWindow() <= getCurrentBlockchainHeight())
        return i + 1;
    } while (i != 0);
    return 0;
  }

  bool Blockchain::getRandomOutsByAmount(const COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::request &req, COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::response &res)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    for (uint64_t amount : req.amounts)
    {
      COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount &result_outs = *res.outs.insert(res.outs.end(), COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount());
      result_outs.amount = amount;
      auto it = m_outputs.find(amount);
      if (it == m_outputs.end())
      {
        logger(ERROR, BRIGHT_RED) << "COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS: not outs for amount " << amount;
        continue;
      }

      std::vector<std::pair<TransactionIndex, uint16_t>> &amount_outs = it->second;
      size_t up_index_limit = find_end_of_allowed_index(amount_outs);
      if (!(up_index_limit <= amount_outs.size()))
      {
        logger(ERROR, BRIGHT_RED) << "internal error: find_end_of_allowed_index returned wrong index=" << up_index_limit << ", with amount_outs.size = " << amount_outs.size();
        return false;
      }

      if (up_index_limit > 0)
      {
        ShuffleGenerator<size_t, crypto::random_engine<size_t>> generator(up_index_limit);
        for (uint64_t j = 0; j < up_index_limit && result_outs.outs.size() < req.outs_count; ++j)
          add_out_to_get_random_outs(amount_outs, result_outs, amount, generator());
      }
    }
    return true;
  }

  uint32_t Blockchain::findBlockchainSupplement(const std::vector<crypto::Hash> &qblock_ids)
  {
    assert(!qblock_ids.empty());
    uint32_t blockIndex = 0;
    {
      std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
#ifdef HAVE_MDBX
      if (m_useMdbx)
      {
        blockIndex = findBlockchainSupplementInternal(qblock_ids);
      }
      else
#endif
          if (!m_blockIndex.findSupplement(qblock_ids, blockIndex))
        blockIndex = findBlockchainSupplementInternal(qblock_ids);
    }
    return blockIndex;
  }

  uint64_t Blockchain::blockDifficulty(size_t i)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    if (!(i < blocksSize()))
    {
      logger(ERROR, BRIGHT_RED) << "wrong block index i = " << i << " at Blockchain::block_difficulty()";
      return 0;
    }
    uint64_t diff_i = getBlockHeader(i).cumulativeDifficulty;
    if (i == 0)
      return diff_i;
    return diff_i - getBlockHeader(i - 1).cumulativeDifficulty;
  }

  void Blockchain::print_blockchain(uint64_t start_index, uint64_t end_index)
  {
    std::stringstream ss;
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    if (start_index >= blocksSize())
    {
      logger(INFO, BRIGHT_WHITE) << "Wrong starter index set: " << start_index << ", expected max index " << blocksSize() - 1;
      return;
    }
    for (size_t i = start_index; i != blocksSize() && i != end_index; i++)
    {
      cn::BlockHeaderPOD hdr = getBlockHeader(i);
      ss << "height " << i << ", timestamp " << hdr.timestamp
         << ", cumul_dif " << hdr.cumulativeDifficulty
         << ", cumul_size " << hdr.blockCumulativeSize
         << "\nid\t\t"
#ifdef HAVE_MDBX
         << common::podToHex(m_blockHashes[i])
#else
         << common::podToHex(get_block_hash(blocksAt(i).bl))
#endif
         << "\ndifficulty\t\t" << blockDifficulty(i) << ", nonce " << hdr.nonce
         << ", tx_count " << blocksAt(i).bl.transactionHashes.size()
         << ENDL;
    }
    logger(INFO) << "Blockchain:\n"
                 << ss.str();
  }

  void Blockchain::print_blockchain_index(bool print_all)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      if (!print_all)
      {
        uint32_t height = getCurrentBlockchainHeight() - 1;
        std::string id = common::podToHex(m_blockHashes[height]);
        logger(INFO) << "Current blockchain index: id: " << id << " height: " << height;
      }
      else
      {
        logger(INFO) << "Blockchain indexes:";
        size_t height = 0;
        for (const auto &id : m_blockHashes)
          logger(INFO) << "id: " << id << " height: " << height++;
      }
    }
    else
#endif
    {
      if (!print_all)
      {
        uint32_t height = getCurrentBlockchainHeight() - 1;
        std::string id = common::podToHex(m_blockIndex.getBlockId(height));
        logger(INFO) << "Current blockchain index: id: " << id << " height: " << height;
      }
      else
      {
        std::vector<crypto::Hash> blockIds = m_blockIndex.getBlockIds(0, std::numeric_limits<uint32_t>::max());
        logger(INFO) << "Blockchain indexes:";
        size_t height = 0;
        for (auto i = blockIds.begin(); i != blockIds.end(); ++i, ++height)
          logger(INFO) << "id: " << *i << " height: " << height;
      }
    }
  }

  void Blockchain::print_blockchain_outs(const std::string &file)
  {
    std::stringstream ss;
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    for (const outputs_container::value_type &v : m_outputs)
    {
      const std::vector<std::pair<TransactionIndex, uint16_t>> &vals = v.second;
      if (!vals.empty())
      {
        ss << "amount: " << v.first << ENDL;
        for (size_t i = 0; i != vals.size(); i++)
          ss << "\t" << getObjectHash(transactionByIndex(vals[i].first).tx) << ": " << vals[i].second << ENDL;
      }
    }
    if (common::saveStringToFile(file, ss.str()))
      logger(INFO, BRIGHT_WHITE) << "Current outputs index writen to file: " << file;
    else
      logger(WARNING, BRIGHT_YELLOW) << "Failed to write current outputs index to file: " << file;
  }

  std::vector<crypto::Hash> Blockchain::findBlockchainSupplement(const std::vector<crypto::Hash> &remoteBlockIds, size_t maxCount, uint32_t &totalBlockCount, uint32_t &startBlockIndex)
  {
    assert(!remoteBlockIds.empty());
    uint32_t startIndex = 0, totalCount = 0;
    {
      std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
      totalCount = getCurrentBlockchainHeight();
      startIndex = findBlockchainSupplement(remoteBlockIds);
    }
    totalBlockCount = totalCount;
    startBlockIndex = startIndex;
    return getBlockIds(startIndex, static_cast<uint32_t>(maxCount));
  }

  bool Blockchain::haveBlock(const crypto::Hash &id)
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      if (m_hashToHeight.count(id))
        return true;
      if (m_alternative_chains.count(id))
        return true;
      return false;
    }
#endif
    if (m_blockIndex.hasBlock(id))
      return true;
    if (m_alternative_chains.count(id))
      return true;
    return false;
  }

  size_t Blockchain::getTotalTransactions()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return m_transactionMap.size();
  }

  bool Blockchain::getTransactionOutputGlobalIndexes(const crypto::Hash &tx_id, std::vector<uint32_t> &indexs)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    auto it = m_transactionMap.find(tx_id);
    if (it == m_transactionMap.end())
    {
      logger(WARNING, YELLOW) << "warning: get_tx_outputs_gindexs failed to find transaction with id = " << tx_id;
      return false;
    }
    const TransactionEntry &tx = transactionByIndex(it->second);
    if (!(tx.m_global_output_indexes.size()))
    {
      logger(ERROR, BRIGHT_RED) << "internal error: global indexes for transaction " << tx_id << " is empty";
      return false;
    }
    indexs.resize(tx.m_global_output_indexes.size());
    for (size_t i = 0; i < tx.m_global_output_indexes.size(); ++i)
      indexs[i] = tx.m_global_output_indexes[i];
    return true;
  }

  bool Blockchain::get_out_by_msig_gindex(uint64_t amount, uint64_t gindex, MultisignatureOutput &out)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    auto it = m_multisignatureOutputs.find(amount);
    if (it == m_multisignatureOutputs.end() || it->second.size() <= gindex)
      return false;
    auto msigUsage = it->second[gindex];
    auto &targetOut = transactionByIndex(msigUsage.transactionIndex).tx.outputs[msigUsage.outputIndex].target;
    if (targetOut.type() != typeid(MultisignatureOutput))
      return false;
    out = boost::get<MultisignatureOutput>(targetOut);
    return true;
  }

  bool Blockchain::checkTransactionInputs(const Transaction &tx, uint32_t &max_used_block_height, crypto::Hash &max_used_block_id, BlockInfo *tail)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    if (tail)
      tail->id = getTailId(tail->height);
    bool res = checkTransactionInputs(tx, &max_used_block_height);
    if (!res)
      return false;
    if (!(max_used_block_height < blocksSize()))
    {
      logger(ERROR, BRIGHT_RED) << "internal error: max used block index=" << max_used_block_height << " is not less then blockchain size = " << blocksSize();
      return false;
    }
    get_block_hash(blocksAt(max_used_block_height).bl, max_used_block_id);
    return true;
  }

  bool Blockchain::haveTransactionKeyImagesAsSpent(const Transaction &tx)
  {
    for (const auto &in : tx.inputs)
      if (in.type() == typeid(KeyInput) && have_tx_keyimg_as_spent(boost::get<KeyInput>(in).keyImage))
        return true;
    return false;
  }

  bool Blockchain::checkTransactionInputs(const Transaction &tx, uint32_t *pmax_used_block_height)
  {
    crypto::Hash tx_prefix_hash = getObjectHash(*static_cast<const TransactionPrefix *>(&tx));
    return checkTransactionInputs(tx, tx_prefix_hash, pmax_used_block_height);
  }

  bool Blockchain::checkTransactionInputs(const Transaction &tx, const crypto::Hash &tx_prefix_hash, uint32_t *pmax_used_block_height)
  {
    size_t inputIndex = 0;
    if (pmax_used_block_height)
      *pmax_used_block_height = 0;
    crypto::Hash transactionHash = getObjectHash(tx);
    for (const auto &txin : tx.inputs)
    {
      assert(inputIndex < tx.signatures.size());
      if (txin.type() == typeid(KeyInput))
      {
        const KeyInput &in_to_key = boost::get<KeyInput>(txin);
        if (!(!in_to_key.outputIndexes.empty()))
        {
          logger(ERROR, BRIGHT_RED) << "empty in_to_key.outputIndexes in transaction with id " << getObjectHash(tx);
          return false;
        }
        if (have_tx_keyimg_as_spent(in_to_key.keyImage))
        {
          logger(DEBUGGING) << "Key image already spent in blockchain: " << common::podToHex(in_to_key.keyImage);
          return false;
        }
        if (!isInCheckpointZone(getCurrentBlockchainHeight()))
        {
          if (!check_tx_input(in_to_key, tx_prefix_hash, tx.signatures[inputIndex], pmax_used_block_height))
          {
            logger(INFO, BRIGHT_WHITE) << "Failed to check input in transaction " << transactionHash;
            return false;
          }
        }
        ++inputIndex;
      }
      else if (txin.type() == typeid(MultisignatureInput))
      {
        if (!isInCheckpointZone(getCurrentBlockchainHeight()))
        {
          if (!validateInput(::boost::get<MultisignatureInput>(txin), transactionHash, tx_prefix_hash, tx.signatures[inputIndex]))
            return false;
        }
        ++inputIndex;
      }
      else
      {
        logger(INFO, BRIGHT_WHITE) << "Transaction << " << transactionHash << " contains input of unsupported type.";
        return false;
      }
    }
    return true;
  }

  bool Blockchain::is_tx_spendtime_unlocked(uint64_t unlock_time)
  {
    if (unlock_time < m_currency.maxBlockHeight())
    {
      if (getCurrentBlockchainHeight() - 1 + m_currency.lockedTxAllowedDeltaBlocks() >= unlock_time)
        return true;
      else
        return false;
    }
    else
    {
      auto current_time = static_cast<uint64_t>(time(nullptr));
      if (current_time + m_currency.lockedTxAllowedDeltaSeconds() >= unlock_time)
        return true;
      else
        return false;
    }
    return false;
  }

  bool Blockchain::check_tx_input(const KeyInput &txin, const crypto::Hash &tx_prefix_hash, const std::vector<crypto::Signature> &sig, uint32_t *pmax_related_block_height)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    struct outputs_visitor
    {
      std::vector<const crypto::PublicKey *> &m_results_collector;
      Blockchain &m_bch;
      LoggerRef logger;
      outputs_visitor(std::vector<const crypto::PublicKey *> &results_collector, Blockchain &bch, ILogger &logger) : m_results_collector(results_collector), m_bch(bch), logger(logger, "outputs_visitor") {}
      bool handle_output(const Transaction &tx, const TransactionOutput &out, size_t transactionOutputIndex)
      {
        if (!m_bch.is_tx_spendtime_unlocked(tx.unlockTime))
        {
          logger(INFO, BRIGHT_WHITE) << "One of outputs for one of inputs have wrong tx.unlockTime = " << tx.unlockTime;
          return false;
        }
        if (out.target.type() != typeid(KeyOutput))
        {
          logger(INFO, BRIGHT_WHITE) << "Output have wrong type id, which=" << out.target.which();
          return false;
        }
        m_results_collector.push_back(&boost::get<KeyOutput>(out.target).key);
        return true;
      }
    };

    std::vector<const crypto::PublicKey *> output_keys;
    outputs_visitor vi(output_keys, *this, logger.getLogger());
    if (!scanOutputKeysForIndexes(txin, vi, pmax_related_block_height))
    {
      logger(INFO, BRIGHT_WHITE) << "Failed to get output keys for tx with amount = " << m_currency.formatAmount(txin.amount) << " and count indexes " << txin.outputIndexes.size();
      return false;
    }
    if (txin.outputIndexes.size() != output_keys.size())
    {
      logger(INFO, BRIGHT_WHITE) << "Output keys mismatch";
      return false;
    }
    if (getCurrentBlockchainHeight() > cn::parameters::UPGRADE_HEIGHT_V4 && getCurrentBlockchainHeight() < cn::parameters::UPGRADE_HEIGHT_V5 && txin.outputIndexes.size() < 3)
    {
      logger(ERROR, BRIGHT_RED) << "ring size too small";
      return false;
    }
    if (!(sig.size() == output_keys.size()))
    {
      logger(ERROR, BRIGHT_RED) << "sig count mismatch";
      return false;
    }
    if (isInCheckpointZone(getCurrentBlockchainHeight()))
      return true;

    static const crypto::KeyImage I = {{0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
    static const crypto::KeyImage L = {{0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10}};
    if (!(scalarmultKey(txin.keyImage, L) == I))
      return false;
    return crypto::check_ring_signature(tx_prefix_hash, txin.keyImage, output_keys, sig.data());
  }

  uint64_t Blockchain::get_adjusted_time() const { return time(nullptr); }

  bool Blockchain::check_tx_outputs(const Transaction &tx, uint32_t height) const
  {
    std::string error;
    for (const TransactionOutput &out : tx.outputs)
      if (!boost::apply_visitor(check_tx_outputs_visitor(tx, height, out.amount, m_currency, error), out.target))
      {
        logger(ERROR, BRIGHT_WHITE) << getObjectHash(tx) << ": " << error;
        return false;
      }
    return true;
  }

  bool Blockchain::check_block_timestamp_main(const Block &b)
  {
    if (b.timestamp > get_adjusted_time() + m_currency.blockFutureTimeLimit())
    {
      logger(INFO, BRIGHT_WHITE) << "Timestamp of block with id: " << get_block_hash(b) << " too far in future";
      return false;
    }
    std::vector<uint64_t> timestamps;
    size_t sz = blocksSize(), offset = sz <= m_currency.timestampCheckWindow() ? 0 : sz - m_currency.timestampCheckWindow();
    for (; offset != sz; ++offset)
      timestamps.push_back(getBlockHeader(offset).timestamp);
    return check_block_timestamp(std::move(timestamps), b);
  }

  bool Blockchain::check_block_timestamp(std::vector<uint64_t> timestamps, const Block &b)
  {
    if (timestamps.size() < m_currency.timestampCheckWindow())
      return true;
    uint64_t median_ts = common::medianValue(timestamps);
    if (b.timestamp < median_ts)
    {
      logger(INFO, BRIGHT_WHITE) << "Timestamp of block " << get_block_hash(b) << " less than median";
      return false;
    }
    return true;
  }

  bool Blockchain::checkBlockVersion(const Block &b, const crypto::Hash &blockHash)
  {
    uint64_t height = get_block_height(b);
    const uint8_t expected = get_block_major_version_for_height(height);
    if (b.majorVersion != expected)
    {
      logger(INFO, BRIGHT_WHITE) << "Block " << blockHash << " has wrong major version";
      return false;
    }
    return true;
  }

  bool Blockchain::checkCumulativeBlockSize(const crypto::Hash &blockId, size_t cumulativeBlockSize, uint64_t height)
  {
    size_t maxSize = m_currency.maxBlockCumulativeSize(height);
    if (cumulativeBlockSize > maxSize)
    {
      logger(INFO, BRIGHT_WHITE) << "Block " << blockId << " is too big";
      return false;
    }
    return true;
  }

  bool Blockchain::getBlockCumulativeSize(const Block &block, size_t &cumulativeSize)
  {
    try
    {
      std::vector<Transaction> blockTxs;
      std::vector<crypto::Hash> missedTxs;
      getTransactions(block.transactionHashes, blockTxs, missedTxs, true);
      cumulativeSize = getObjectBinarySize(block.baseTransaction);
      for (const Transaction &tx : blockTxs)
        cumulativeSize += getObjectBinarySize(tx);

      if (!missedTxs.empty())
      {
        // Transactions not available, estimate using the block entry from storage
        logger(DEBUGGING) << "Some transactions missing for cumulative size calculation, using stored size";
        // Fall through - the caller already handles this with a debug message
      }

      return missedTxs.empty();
    }
    catch (const std::exception &)
    {
      logger(ERROR, BRIGHT_RED) << "Error calculating cumulative block size";
      return false;
    }
  }

  bool Blockchain::update_next_comulative_size_limit()
  {
    std::vector<size_t> sz;
    get_last_n_blocks_sizes(sz, m_currency.rewardBlocksWindow());
    uint64_t median = common::medianValue(sz);
    if (median <= m_currency.blockGrantedFullRewardZone())
      median = m_currency.blockGrantedFullRewardZone();
    m_current_block_cumul_sz_limit = median * 2;
    return true;
  }

  bool Blockchain::addNewBlock(const Block &bl_, block_verification_context &bvc)
  {
    try
    {
      Block bl = bl_;
      crypto::Hash id;
      if (!get_block_hash(bl, id))
      {
        logger(ERROR, BRIGHT_RED) << "Failed to get block hash";
        bvc.m_verification_failed = true;
        return false;
      }

      bool add_result;
      {
        std::lock_guard<decltype(m_tx_pool)> poolLock(m_tx_pool);
        std::lock_guard<decltype(m_blockchain_lock)> bcLock(m_blockchain_lock);

        if (haveBlock(id))
        {
          logger(TRACE) << "block already exists";
          bvc.m_already_exists = true;
          return false;
        }

        auto height = static_cast<uint32_t>(blocksSize());

        if (!(bl.previousBlockHash == getTailId()))
        {
          bvc.m_added_to_main_chain = false;
          add_result = handle_alternative_block(bl, id, bvc);
        }
        else
        {
          add_result = pushBlock(bl, id, bvc, height);
          if (add_result)
          {
            sendMessage(BlockchainMessage(NewBlockMessage(id)));
            if (m_blockchainAutosaveEnabled && height % 720 == 0)
              storeCache();
          }
        }
      }

      if (add_result && bvc.m_added_to_main_chain)
        m_observerManager.notify(&IBlockchainStorageObserver::blockchainUpdated);
      return add_result;
    }
    catch (const std::exception &)
    {
      logger(ERROR, BRIGHT_RED) << "Error adding new block to blockchain";
      bvc.m_verification_failed = true;
      return false;
    }
  }

  const Blockchain::TransactionEntry &Blockchain::transactionByIndex(TransactionIndex index)
  {
#ifdef HAVE_MDBX
    if (m_useMdbx && m_mdbxStorage)
      return blocksAt(index.block).transactions[index.transaction];
#endif
    return m_blocks[index.block].transactions[index.transaction];
  }

  bool Blockchain::pushBlock(const Block &blockData, const crypto::Hash &id, block_verification_context &bvc, uint32_t height)
  {
    try
    {
      std::vector<Transaction> transactions;
      if (!loadTransactions(blockData, transactions, height))
      {
        bvc.m_verification_failed = true;
        return false;
      }

      if (!pushBlock(blockData, transactions, id, bvc))
      {
        saveTransactions(transactions, height);
        return false;
      }

      return true;
    }
    catch (const std::exception &e)
    {
      logger(ERROR, BRIGHT_RED) << "Exception in pushBlock: " << e.what();
      bvc.m_verification_failed = true;
      return false;
    }
  }

  bool Blockchain::pushBlock(const Block &blockData, const std::vector<Transaction> &transactions, const crypto::Hash &id, block_verification_context &bvc)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    auto blockProcessingStart = std::chrono::steady_clock::now();
    crypto::Hash blockHash = id;

    // Duplicate check
#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      if (m_hashToHeight.count(blockHash) > 0)
      {
        logger(ERROR, BRIGHT_RED) << "Block " << blockHash << " already exists";
        bvc.m_verification_failed = true;
        return false;
      }
    }
    else
#endif
        if (m_blockIndex.hasBlock(blockHash))
    {
      logger(ERROR, BRIGHT_RED) << "Block " << blockHash << " already exists";
      bvc.m_verification_failed = true;
      return false;
    }

    if (!checkBlockVersion(blockData, blockHash))
    {
      bvc.m_verification_failed = true;
      return false;
    }

    // Height and merge mining check
    uint32_t height = 0;
    TransactionExtraMergeMiningTag mmTag;
#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      auto it = m_hashToHeight.find(blockHash);
      if (it != m_hashToHeight.end())
      {
        height = it->second;
        if (getMergeMiningTagFromExtra(blockData.baseTransaction.extra, mmTag) && height >= cn::parameters::UPGRADE_HEIGHT_V6)
        {
          logger(ERROR, BRIGHT_RED) << "Merge mining tag found";
          return false;
        }
      }
    }
    else
#endif
        if (m_blockIndex.getBlockHeight(blockHash, height))
    {
      if (getMergeMiningTagFromExtra(blockData.baseTransaction.extra, mmTag) && height >= cn::parameters::UPGRADE_HEIGHT_V6)
      {
        logger(ERROR, BRIGHT_RED) << "Merge mining tag found";
        return false;
      }
    }

    if (blockData.previousBlockHash != getTailId())
    {
      logger(INFO, BRIGHT_WHITE) << "Wrong previousBlockHash";
      bvc.m_verification_failed = true;
      return false;
    }
    if (!check_block_timestamp_main(blockData))
    {
      logger(INFO, BRIGHT_WHITE) << "Invalid timestamp";
      bvc.m_verification_failed = true;
      return false;
    }

    // Difficulty and PoW
    auto targetTimeStart = std::chrono::steady_clock::now();
    difficulty_type currentDifficulty = getDifficultyForNextBlock();
    auto target_calculating_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - targetTimeStart).count();
    if (!currentDifficulty)
    {
      logger(ERROR, BRIGHT_RED) << "difficulty overhead";
      return false;
    }

    auto longhashTimeStart = std::chrono::steady_clock::now();
    crypto::Hash proof_of_work = NULL_HASH;
    if (m_checkpoints.is_in_checkpoint_zone(getCurrentBlockchainHeight()))
    {
      if (!m_checkpoints.check_block(getCurrentBlockchainHeight(), blockHash))
      {
        bvc.m_verification_failed = true;
        return false;
      }
    }
    else if (!m_currency.checkProofOfWork(m_cn_context, blockData, currentDifficulty, proof_of_work))
    {
      logger(INFO, BRIGHT_WHITE) << "Block " << blockHash << " has too weak proof of work";
      bvc.m_verification_failed = true;
      return false;
    }

    // Miner transaction pre‑validation
    if (!prevalidate_miner_transaction(blockData, static_cast<uint32_t>(blocksSize())))
    {
      logger(INFO, BRIGHT_WHITE) << "Block " << blockHash << " failed prevalidation";
      bvc.m_verification_failed = true;
      return false;
    }

    // Build the complete BlockEntry
    BlockEntry block;
    block.bl = blockData;
    block.height = static_cast<uint32_t>(blocksSize());

    // Start with the miner transaction
    crypto::Hash minerTransactionHash = getObjectHash(blockData.baseTransaction);
    block.transactions.resize(1 + transactions.size());
    block.transactions[0].tx = blockData.baseTransaction;

    TransactionIndex transactionIndex = {block.height, static_cast<uint16_t>(0)};
    pushTransaction(block, minerTransactionHash, transactionIndex);

    size_t coinbase_blob_size = getObjectBinarySize(blockData.baseTransaction);
    size_t cumulative_block_size = coinbase_blob_size;
    uint64_t fee_summary = 0, interestSummary = 0;

    // Add all non‑base transactions
    for (size_t i = 0; i < transactions.size(); ++i)
    {
      const crypto::Hash &tx_id = blockData.transactionHashes[i];
      block.transactions[1 + i].tx = transactions[i];
      size_t blob_size = toBinaryArray(transactions[i]).size();
      uint64_t fee = m_currency.getTransactionFee(transactions[i], block.height);

      bool isTransactionValid = true;
      if (block.bl.majorVersion == BLOCK_MAJOR_VERSION_1 && transactions[i].version > TRANSACTION_VERSION_1)
      {
        isTransactionValid = false;
        logger(INFO, BRIGHT_WHITE) << "invalid version";
      }
      if (!checkTransactionInputs(transactions[i]))
      {
        isTransactionValid = false;
        logger(INFO, BRIGHT_WHITE) << "wrong inputs";
      }
      if (!check_tx_outputs(transactions[i], block.height))
      {
        isTransactionValid = false;
        logger(INFO, BRIGHT_WHITE) << "invalid output";
      }

      if (!isTransactionValid)
      {
        logger(INFO, BRIGHT_WHITE) << "Block " << blockHash << " has invalid transaction: " << tx_id;
        bvc.m_verification_failed = true;

        // Rollback the miner transaction we already pushed
        popTransaction(blockData.baseTransaction, minerTransactionHash);
        return false;
      }

      ++transactionIndex.transaction;
      pushTransaction(block, tx_id, transactionIndex);
      cumulative_block_size += blob_size;
      fee_summary += fee;
      interestSummary += m_currency.calculateTotalTransactionInterest(transactions[i], block.height);
    }

    if (!checkCumulativeBlockSize(blockHash, cumulative_block_size, block.height))
    {
      bvc.m_verification_failed = true;
      // Rollback all transactions pushed so far (miner + non‑base)
      popTransactions(block, minerTransactionHash);
      return false;
    }

    // Validate miner transaction amounts
    int64_t emissionChange = 0;
    uint64_t reward = 0;
    uint64_t already_generated_coins = (block.height == 0) ? 0 : getBlockHeader(block.height - 1).alreadyGeneratedCoins;
    if (!validate_miner_transaction(blockData, block.height, cumulative_block_size, already_generated_coins, fee_summary, reward, emissionChange))
    {
      logger(INFO, BRIGHT_WHITE) << "Block " << blockHash << " has invalid miner transaction";
      bvc.m_verification_failed = true;
      popTransactions(block, minerTransactionHash);
      return false;
    }

    // Finalize the block entry
    block.block_cumulative_size = cumulative_block_size;
    block.cumulative_difficulty = currentDifficulty;
    block.already_generated_coins = already_generated_coins + emissionChange + interestSummary;
    if (block.height > 0)
      block.cumulative_difficulty += getBlockHeader(block.height - 1).cumulativeDifficulty;

    // PUSH THE COMPLETE BLOCK (ONCE!)
    pushBlock(block);
    pushToDepositIndex(block, interestSummary);

#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      cn::BlockHeaderPOD hdr;
      hdr.majorVersion = block.bl.majorVersion;
      hdr.minorVersion = block.bl.minorVersion;
      hdr.timestamp = block.bl.timestamp;
      hdr.previousBlockHash = block.bl.previousBlockHash;
      hdr.nonce = block.bl.nonce;
      hdr.blockCumulativeSize = block.block_cumulative_size;
      hdr.cumulativeDifficulty = block.cumulative_difficulty;
      hdr.alreadyGeneratedCoins = block.already_generated_coins;
      hdr.height = block.height;
      m_mdbxStorage->pushBlockHeader(block.height, hdr);
      m_mdbxStorage->flush();

      blocksAt(block.height); // force cache load, optional
    }
#endif

    logger(DEBUGGING) << "+++++ Block added id:\t" << blockHash << " PoW:\t" << proof_of_work << " HEIGHT " << block.height << " difficulty:\t" << currentDifficulty;

    bvc.m_added_to_main_chain = true;
    m_upgradeDetectorV2.blockPushed();
    m_upgradeDetectorV3.blockPushed();
    m_upgradeDetectorV4.blockPushed();
    m_upgradeDetectorV7.blockPushed();
    m_upgradeDetectorV8.blockPushed();
    update_next_comulative_size_limit();

    // Auto‑checkpoint generation
    const uint32_t networkHeight = static_cast<uint32_t>(blocksSize());
    if (networkHeight > cn::CHECKPOINT_VERIFICATION_BUFFER)
    {
      const uint32_t maxCheckpointHeight = networkHeight - cn::CHECKPOINT_VERIFICATION_BUFFER;
      const uint32_t lastCheckpoint = m_checkpoints.getMaxHeight();
      uint32_t nextMilestone = ((lastCheckpoint / cn::CHECKPOINT_INTERVAL) + 1) * cn::CHECKPOINT_INTERVAL;

      while (nextMilestone <= maxCheckpointHeight)
      {
        crypto::Hash milestoneHash = getBlockIdByHeight(nextMilestone);
#ifdef HAVE_MDBX
        if (m_useMdbx && m_mdbxStorage)
          m_mdbxStorage->storeCheckpoint(nextMilestone, milestoneHash);
#endif
        m_checkpoints.add_checkpoint(nextMilestone, common::podToHex(milestoneHash));
        logger(INFO, BRIGHT_GREEN) << "Auto-generated checkpoint at height " << nextMilestone
                                   << " (buffer: " << (networkHeight - nextMilestone) << " blocks behind tip)";

        // Notify protocol handler to share with peers
        if (m_checkpointGeneratedCallback)
        {
          m_checkpointGeneratedCallback(nextMilestone, milestoneHash);
        }

        nextMilestone += cn::CHECKPOINT_INTERVAL;
      }
    }

    return true;
  }

  uint64_t Blockchain::fullDepositAmount() const
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return m_depositIndex.fullDepositAmount();
  }
  uint64_t Blockchain::depositAmountAtHeight(size_t height) const
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return m_depositIndex.depositAmountAtHeight(static_cast<DepositIndex::DepositHeight>(height));
  }
  uint64_t Blockchain::depositInterestAtHeight(size_t height) const
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return m_depositIndex.depositInterestAtHeight(static_cast<DepositIndex::DepositHeight>(height));
  }

  void Blockchain::pushToDepositIndex(const BlockEntry &block, uint64_t interest)
  {
    int64_t deposit = 0;
    for (const auto &tx : block.transactions)
    {
      for (const auto &in : tx.tx.inputs)
        if (in.type() == typeid(MultisignatureInput))
        {
          auto &multisign = boost::get<MultisignatureInput>(in);
          if (multisign.term > 0)
            deposit -= multisign.amount;
        }
      for (const auto &out : tx.tx.outputs)
        if (out.target.type() == typeid(MultisignatureOutput))
        {
          auto &multisign = boost::get<MultisignatureOutput>(out.target);
          if (multisign.term > 0)
            deposit += out.amount;
        }
    }
    m_depositIndex.pushBlock(deposit, interest);
  }

  bool Blockchain::pushBlock(const BlockEntry &block)
  {
    crypto::Hash blockHash = get_block_hash(block.bl);

#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      cn::BinaryArray ba = cn::toBinaryArray(block);
      uint32_t height = block.height;
      m_mdbxStorage->pushBlockEntry(height, ba);
      m_mdbxStorage->addBlock(block.bl, blockHash, height);
      m_mdbxStorage->setTopBlockHeight(height);

      // update in‑memory index (only if new height)
      if (height >= m_blockHashes.size())
      {
        m_blockHashes.push_back(blockHash);
        m_hashToHeight[blockHash] = height;
      }

      // update LRU cache if this block was already cached (early store)
      for (size_t c = 0; c < m_cachedEntries.size(); ++c)
      {
        if (m_cachedEntries[c].height == block.height)
        {
          m_cachedEntries[c].entry = block; // replace with final data
          break;
        }
      }
    }
    else
#endif
    {
      m_blocks.push_back(block);
      m_blockIndex.push(blockHash);
    }

    m_timestampIndex.add(block.bl.timestamp, blockHash);
    m_generatedTransactionsIndex.add(block.bl);
    return true;
  }

  void Blockchain::popBlock(const crypto::Hash &blockHash)
  {
    if (blocksEmpty())
    {
      logger(ERROR, BRIGHT_RED) << "Attempt to pop block from empty blockchain.";
      return;
    }

    BlockEntry entry = blocksBack();
    uint32_t height = static_cast<uint32_t>(blocksSize());

    std::vector<Transaction> transactions(entry.transactions.size() - 1);
    for (size_t i = 0; i < entry.transactions.size() - 1; ++i)
      transactions[i] = entry.transactions[1 + i].tx;

    saveTransactions(transactions, height);
    popTransactions(entry, getObjectHash(entry.bl.baseTransaction));

    m_timestampIndex.remove(entry.bl.timestamp, blockHash);
    m_generatedTransactionsIndex.remove(entry.bl);
    m_depositIndex.popBlock();

#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      uint32_t top = m_mdbxStorage->topBlockHeight();
      m_mdbxStorage->popBlockEntry(top);
      m_mdbxStorage->removeBlock(blockHash);
      if (top > 0)
        m_mdbxStorage->setTopBlockHeight(top - 1);
      m_blockHashes.pop_back();
      m_hashToHeight.erase(blockHash);
    }
    else
#endif
    {
      m_blocks.pop_back();
      m_blockIndex.pop();
      assert(m_blockIndex.size() == m_blocks.size());
    }

    m_upgradeDetectorV2.blockPopped();
    m_upgradeDetectorV3.blockPopped();
    m_upgradeDetectorV4.blockPopped();
    m_upgradeDetectorV7.blockPopped();
    m_upgradeDetectorV8.blockPopped();
  }

  bool Blockchain::pushTransaction(BlockEntry &block, const crypto::Hash &transactionHash, TransactionIndex transactionIndex)
  {
    auto result = m_transactionMap.insert(std::make_pair(transactionHash, transactionIndex));
    if (!result.second)
    {
      logger(ERROR, BRIGHT_RED) << "Duplicate transaction";
      return false;
    }
    TransactionEntry &transaction = block.transactions[transactionIndex.transaction];
    if (!checkMultisignatureInputsDiff(transaction.tx))
    {
      logger(ERROR, BRIGHT_RED) << "Double spending transaction";
      m_transactionMap.erase(transactionHash);
      return false;
    }

    for (size_t i = 0; i < transaction.tx.inputs.size(); ++i)
      if (transaction.tx.inputs[i].type() == typeid(KeyInput))
      {
        const auto &keyImage = ::boost::get<KeyInput>(transaction.tx.inputs[i]).keyImage;
        if (!m_spent_keys.insert(std::make_pair(keyImage, block.height)).second)
        {
          for (size_t j = 0; j < i; ++j)
            m_spent_keys.erase(::boost::get<KeyInput>(transaction.tx.inputs[i - 1 - j]).keyImage);
          m_transactionMap.erase(transactionHash);
          return false;
        }
      }

    for (const auto &inv : transaction.tx.inputs)
      if (inv.type() == typeid(MultisignatureInput))
      {
        const MultisignatureInput &in = ::boost::get<MultisignatureInput>(inv);
        m_multisignatureOutputs[in.amount][in.outputIndex].isUsed = true;
      }

    transaction.m_global_output_indexes.resize(transaction.tx.outputs.size());
    for (uint32_t output = 0; output < transaction.tx.outputs.size(); ++output)
    {
      if (transaction.tx.outputs[output].target.type() == typeid(KeyOutput))
      {
        auto &amountOutputs = m_outputs[transaction.tx.outputs[output].amount];
        transaction.m_global_output_indexes[output] = static_cast<uint32_t>(amountOutputs.size());
        amountOutputs.push_back(std::make_pair<>(transactionIndex, output));
      }
      else if (transaction.tx.outputs[output].target.type() == typeid(MultisignatureOutput))
      {
        auto &amountOutputs = m_multisignatureOutputs[transaction.tx.outputs[output].amount];
        transaction.m_global_output_indexes[output] = static_cast<uint32_t>(amountOutputs.size());
        amountOutputs.push_back({transactionIndex, static_cast<uint16_t>(output), false});
      }
    }
    m_paymentIdIndex.add(transaction.tx);
    return true;
  }

  void Blockchain::popTransaction(const Transaction &transaction, const crypto::Hash &transactionHash)
  {
    TransactionIndex transactionIndex = m_transactionMap.at(transactionHash);
    for (size_t outputIndex = 0; outputIndex < transaction.outputs.size(); ++outputIndex)
    {
      const TransactionOutput &output = transaction.outputs[transaction.outputs.size() - 1 - outputIndex];
      if (output.target.type() == typeid(KeyOutput))
      {
        auto amountOutputs = m_outputs.find(output.amount);
        if (amountOutputs == m_outputs.end() || amountOutputs->second.empty())
          continue;
        if (amountOutputs->second.back().first.block != transactionIndex.block || amountOutputs->second.back().first.transaction != transactionIndex.transaction)
          continue;
        if (amountOutputs->second.back().second != transaction.outputs.size() - 1 - outputIndex)
          continue;
        amountOutputs->second.pop_back();
        if (amountOutputs->second.empty())
          m_outputs.erase(amountOutputs);
      }
      else if (output.target.type() == typeid(MultisignatureOutput))
      {
        auto amountOutputs = m_multisignatureOutputs.find(output.amount);
        if (amountOutputs == m_multisignatureOutputs.end() || amountOutputs->second.empty())
          continue;
        if (amountOutputs->second.back().isUsed)
          continue;
        if (amountOutputs->second.back().transactionIndex.block != transactionIndex.block || amountOutputs->second.back().transactionIndex.transaction != transactionIndex.transaction)
          continue;
        if (amountOutputs->second.back().outputIndex != transaction.outputs.size() - 1 - outputIndex)
          continue;
        amountOutputs->second.pop_back();
        if (amountOutputs->second.empty())
          m_multisignatureOutputs.erase(amountOutputs);
      }
    }
    for (auto &input : transaction.inputs)
      if (input.type() == typeid(KeyInput))
        m_spent_keys.erase(::boost::get<KeyInput>(input).keyImage);
      else if (input.type() == typeid(MultisignatureInput))
      {
        const MultisignatureInput &in = ::boost::get<MultisignatureInput>(input);
        m_multisignatureOutputs[in.amount][in.outputIndex].isUsed = false;
      }
    m_paymentIdIndex.remove(transaction);
    m_transactionMap.erase(transactionHash);
  }

  void Blockchain::popTransactions(const BlockEntry &block, const crypto::Hash &minerTransactionHash)
  {
    for (size_t i = 0; i < block.transactions.size() - 1; ++i)
      popTransaction(block.transactions[block.transactions.size() - 1 - i].tx, block.bl.transactionHashes[block.transactions.size() - 2 - i]);
    popTransaction(block.bl.baseTransaction, minerTransactionHash);
  }

  bool Blockchain::validateInput(const MultisignatureInput &input, const crypto::Hash &transactionHash, const crypto::Hash &transactionPrefixHash, const std::vector<crypto::Signature> &transactionSignatures)
  {
    assert(input.signatureCount == transactionSignatures.size());
    auto amountOutputs = m_multisignatureOutputs.find(input.amount);
    if (amountOutputs == m_multisignatureOutputs.end() || input.outputIndex >= amountOutputs->second.size())
      return false;
    const MultisignatureOutputUsage &outputIndex = amountOutputs->second[input.outputIndex];
    if (outputIndex.isUsed)
      return false;

    const Transaction &outputTransaction = blocksAt(outputIndex.transactionIndex.block).transactions[outputIndex.transactionIndex.transaction].tx;
    if (!is_tx_spendtime_unlocked(outputTransaction.unlockTime))
      return false;
    assert(outputTransaction.outputs[outputIndex.outputIndex].target.type() == typeid(MultisignatureOutput));
    const MultisignatureOutput &output = ::boost::get<MultisignatureOutput>(outputTransaction.outputs[outputIndex.outputIndex].target);
    if (input.signatureCount != output.requiredSignatureCount || input.term != output.term)
      return false;
    if (output.term != 0 && outputIndex.transactionIndex.block + output.term > getCurrentBlockchainHeight())
      return false;

    size_t inputSignatureIndex = 0, outputKeyIndex = 0;
    while (inputSignatureIndex < input.signatureCount)
    {
      if (outputKeyIndex == output.keys.size())
        return false;
      if (crypto::check_signature(transactionPrefixHash, output.keys[outputKeyIndex], transactionSignatures[inputSignatureIndex]))
        ++inputSignatureIndex;
      ++outputKeyIndex;
    }
    return true;
  }

  bool Blockchain::rollbackBlockchainTo(uint32_t height)
  {
    try
    {
      std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
      logger(INFO, BRIGHT_YELLOW) << "Rolling back blockchain to height " << height;
      if (height >= blocksSize())
      {
        logger(WARNING, BRIGHT_YELLOW) << "Requested rollback to height " << height << " >= current height " << blocksSize();
        return true;
      }
      while (blocksSize() > height + 1)
        if (!removeLastBlock())
        {
          logger(ERROR, BRIGHT_RED) << "Failed to remove last block";
          return false;
        }
      logger(INFO, BRIGHT_GREEN) << "Blockchain successfully rolled back to height: " << height << " Synchronization will resume";
      return true;
    }
    catch (const std::exception &)
    {
      logger(ERROR, BRIGHT_RED) << "Error rolling back blockchain";
      return false;
    }
  }

  bool Blockchain::removeLastBlock()
  {
#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      uint32_t top = m_mdbxStorage->topBlockHeight();
      if (top == 0)
        return false;
      cn::BinaryArray ba;
      if (!m_mdbxStorage->getBlockEntry(top, ba))
        return false;
      BlockEntry entry;
      if (!cn::fromBinaryArray(entry, ba))
        return false;

      popTransactions(entry, getObjectHash(entry.bl.baseTransaction));
      crypto::Hash blockHash = get_block_hash(entry.bl);
      m_timestampIndex.remove(entry.bl.timestamp, blockHash);
      m_generatedTransactionsIndex.remove(entry.bl);

      m_mdbxStorage->popBlockEntry(top);
      m_mdbxStorage->removeBlock(blockHash);
      if (top > 0)
        m_mdbxStorage->setTopBlockHeight(top - 1);
      m_blockHashes.pop_back();
      m_hashToHeight.erase(blockHash);
      return true;
    }
#endif
    if (m_blocks.empty())
      return false;
    popTransactions(m_blocks.back(), getObjectHash(m_blocks.back().bl.baseTransaction));
    crypto::Hash blockHash = getBlockIdByHeight(m_blocks.back().height);
    m_timestampIndex.remove(m_blocks.back().bl.timestamp, blockHash);
    m_generatedTransactionsIndex.remove(m_blocks.back().bl);
    m_blocks.pop_back();
    m_blockIndex.pop();
    assert(m_blockIndex.size() == m_blocks.size());
    return true;
  }

  bool Blockchain::getLowerBound(uint64_t timestamp, uint64_t startOffset, uint32_t &height)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      assert(startOffset < m_blockHashes.size());

      for (size_t i = startOffset; i < m_blockHashes.size(); ++i)
      {
        cn::BlockHeaderPOD hdr = getBlockHeader(static_cast<uint32_t>(i));
        if (hdr.timestamp >= timestamp - m_currency.blockFutureTimeLimit())
        {
          height = static_cast<uint32_t>(i);
          return true;
        }
      }
      return false;
    }
#endif

    assert(startOffset < blocksSize());
    auto bound = std::lower_bound(m_blocks.begin() + startOffset, m_blocks.end(), timestamp - m_currency.blockFutureTimeLimit(),
                                  [](const BlockEntry &b, uint64_t timestamp)
                                  { return b.bl.timestamp < timestamp; });
    if (bound == m_blocks.end())
      return false;
    height = static_cast<uint32_t>(std::distance(m_blocks.begin(), bound));
    return true;
  }

  std::vector<crypto::Hash> Blockchain::getBlockIds(uint32_t startHeight, uint32_t maxCount)
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      std::vector<crypto::Hash> result;
      if (m_blockHashes.empty())
        return result;

      if (startHeight >= m_blockHashes.size())
        startHeight = (uint32_t)(m_blockHashes.size() - 1);

      uint32_t end = std::min(startHeight + maxCount, static_cast<uint32_t>(m_blockHashes.size()));
      result.assign(m_blockHashes.begin() + startHeight, m_blockHashes.begin() + end);
      return result;
    }
#endif
    return m_blockIndex.getBlockIds(startHeight, maxCount);
  }

  bool Blockchain::getBlockContainingTransaction(const crypto::Hash &txId, crypto::Hash &blockId, uint32_t &blockHeight)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    auto it = m_transactionMap.find(txId);
    if (it == m_transactionMap.end())
      return false;
    blockHeight = blocksAt(it->second.block).height;
    blockId = getBlockIdByHeight(blockHeight);
    return true;
  }

  bool Blockchain::getAlreadyGeneratedCoins(const crypto::Hash &hash, uint64_t &generatedCoins)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      auto it = m_hashToHeight.find(hash);
      if (it != m_hashToHeight.end())
      {
        generatedCoins = getBlockHeader(it->second).alreadyGeneratedCoins;
        return true;
      }
      auto altIt = m_alternative_chains.find(hash);
      if (altIt != m_alternative_chains.end())
      {
        generatedCoins = altIt->second.already_generated_coins;
        return true;
      }
      return false;
    }
#endif

    uint32_t height = 0;
    if (m_blockIndex.getBlockHeight(hash, height))
    {
      generatedCoins = getBlockHeader(height).alreadyGeneratedCoins;
      return true;
    }
    auto it = m_alternative_chains.find(hash);
    if (it != m_alternative_chains.end())
    {
      generatedCoins = it->second.already_generated_coins;
      return true;
    }
    return false;
  }

  bool Blockchain::getBlockSize(const crypto::Hash &hash, size_t &size)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

#ifdef HAVE_MDBX
    if (m_useMdbx)
    {
      auto it = m_hashToHeight.find(hash);
      if (it != m_hashToHeight.end())
      {
        size = getBlockHeader(it->second).blockCumulativeSize;
        return true;
      }
      auto altIt = m_alternative_chains.find(hash);
      if (altIt != m_alternative_chains.end())
      {
        size = altIt->second.block_cumulative_size;
        return true;
      }
      return false;
    }
#endif

    uint32_t height = 0;
    if (m_blockIndex.getBlockHeight(hash, height))
    {
      size = getBlockHeader(height).blockCumulativeSize;
      return true;
    }
    auto it = m_alternative_chains.find(hash);
    if (it != m_alternative_chains.end())
    {
      size = it->second.block_cumulative_size;
      return true;
    }
    return false;
  }

  bool Blockchain::getMultisigOutputReference(const MultisignatureInput &txInMultisig, std::pair<crypto::Hash, size_t> &outputReference)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    auto amountIter = m_multisignatureOutputs.find(txInMultisig.amount);
    if (amountIter == m_multisignatureOutputs.end() || amountIter->second.size() <= txInMultisig.outputIndex)
      return false;
    const MultisignatureOutputUsage &outputIndex = amountIter->second[txInMultisig.outputIndex];
    const Transaction &outputTransaction = blocksAt(outputIndex.transactionIndex.block).transactions[outputIndex.transactionIndex.transaction].tx;
    outputReference.first = getObjectHash(outputTransaction);
    outputReference.second = outputIndex.outputIndex;
    return true;
  }

  bool Blockchain::storeBlockchainIndices()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    logger(INFO, BRIGHT_WHITE) << "Saving blockchain indices";
    BlockchainIndicesSerializer ser(*this, getTailId(), logger.getLogger());
    try
    {
      if (!storeToBinaryFile(ser, appendPath(m_config_folder, m_currency.blockchinIndicesFileName())))
      {
        logger(ERROR, BRIGHT_RED) << "Failed to save";
        return false;
      }
    }
    catch (const std::exception &)
    {
      logger(ERROR, BRIGHT_RED) << "Failed to save";
      return false;
    }
    return true;
  }

  bool Blockchain::loadBlockchainIndices()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    logger(INFO, BRIGHT_WHITE) << "Loading blockchain indices for BlockchainExplorer";
    BlockchainIndicesSerializer loader(*this, get_block_hash(blocksBack().bl), logger.getLogger());
    bool needsRebuild = false;
    try
    {
      loadFromBinaryFile(loader, appendPath(m_config_folder, m_currency.blockchinIndicesFileName()));
      needsRebuild = !loader.loaded();
    }
    catch (const std::exception &)
    {
      needsRebuild = true;
    }

    if (needsRebuild)
    {
      logger(WARNING, BRIGHT_YELLOW) << "No actual blockchain indices for BlockchainExplorer found, rebuilding";
      auto timePoint = std::chrono::steady_clock::now();
      m_paymentIdIndex.clear();
      m_timestampIndex.clear();
      m_generatedTransactionsIndex.clear();
      for (uint32_t b = 0; b < blocksSize(); ++b)
      {
        if (b % 1000 == 0)
          logger(INFO, BRIGHT_WHITE) << "Rebuilding Indices for Height " << b << " of " << blocksSize();
        cn::BlockHeaderPOD hdr = getBlockHeader(b);
        crypto::Hash hash;
#ifdef HAVE_MDBX
        hash = m_blockHashes[b];
#else
        hash = get_block_hash(blocksAt(b).bl);
#endif
        m_timestampIndex.add(hdr.timestamp, hash);
        const BlockEntry &block = blocksAt(b); // needed for transactions
        m_generatedTransactionsIndex.add(block.bl);
        for (size_t t = 0; t < block.transactions.size(); ++t)
          m_paymentIdIndex.add(block.transactions[t].tx);
      }
      logger(INFO, BRIGHT_WHITE) << "Rebuilding blockchain indices took: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - timePoint).count();
    }
    return true;
  }

  bool Blockchain::getGeneratedTransactionsNumber(uint32_t height, uint64_t &generatedTransactions)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return m_generatedTransactionsIndex.find(height, generatedTransactions);
  }
  bool Blockchain::getOrphanBlockIdsByHeight(uint32_t height, std::vector<crypto::Hash> &blockHashes)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return m_orthanBlocksIndex.find(height, blockHashes);
  }
  bool Blockchain::getBlockIdsByTimestamp(uint64_t timestampBegin, uint64_t timestampEnd, uint32_t blocksNumberLimit, std::vector<crypto::Hash> &hashes, uint32_t &blocksNumberWithinTimestamps)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return m_timestampIndex.find(timestampBegin, timestampEnd, blocksNumberLimit, hashes, blocksNumberWithinTimestamps);
  }
  bool Blockchain::getTransactionIdsByPaymentId(const crypto::Hash &paymentId, std::vector<crypto::Hash> &transactionHashes)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return m_paymentIdIndex.find(paymentId, transactionHashes);
  }

  bool Blockchain::loadTransactions(const Block &block, std::vector<Transaction> &transactions, uint32_t height)
  {
    transactions.resize(block.transactionHashes.size());
    size_t transactionSize;
    uint64_t fee;
    for (size_t i = 0; i < block.transactionHashes.size(); ++i)
      if (!m_tx_pool.take_tx(block.transactionHashes[i], transactions[i], transactionSize, fee))
      {
        tx_verification_context context;
        for (size_t j = 0; j < i; ++j)
          if (!m_tx_pool.add_tx(transactions[i - 1 - j], context, true, height))
            throw std::runtime_error("failed to add transaction to pool");
        return false;
      }
    return true;
  }

  void Blockchain::saveTransactions(const std::vector<Transaction> &transactions, uint32_t height)
  {
    tx_verification_context context;
    for (size_t i = 0; i < transactions.size(); ++i)
      if (!m_tx_pool.add_tx(transactions[transactions.size() - 1 - i], context, true, height))
        throw std::runtime_error("failed to add transaction to pool");
  }

  bool Blockchain::addMessageQueue(MessageQueue<BlockchainMessage> &messageQueue) { return m_messageQueueList.insert(messageQueue); }
  bool Blockchain::removeMessageQueue(MessageQueue<BlockchainMessage> &messageQueue) { return m_messageQueueList.remove(messageQueue); }

  void Blockchain::sendMessage(const BlockchainMessage &message)
  {
    for (auto iter = m_messageQueueList.begin(); iter != m_messageQueueList.end(); ++iter)
      iter->push(message);
  }

  bool Blockchain::isBlockInMainChain(const crypto::Hash &blockId) const
  {
#ifdef HAVE_MDBX
    if (m_useMdbx)
      return m_hashToHeight.count(blockId) > 0;
#endif
    return m_blockIndex.hasBlock(blockId);
  }

  bool Blockchain::isInCheckpointZone(const uint32_t height) const { return m_checkpoints.is_in_checkpoint_zone(height); }

  uint32_t Blockchain::findBlockchainSupplementInternal(const std::vector<crypto::Hash> &qblock_ids) const
  {
    uint32_t currentHeight = static_cast<uint32_t>(blocksSize()), bestMatch = 0;
    for (const auto &blockId : qblock_ids)
    {
      uint32_t height = 0;

#ifdef HAVE_MDBX
      if (m_useMdbx)
      {
        auto it = m_hashToHeight.find(blockId);
        if (it != m_hashToHeight.end())
          height = it->second;
      }
      else
#endif
      {
        m_blockIndex.getBlockHeight(blockId, height);
      }

      if (height + 1 > bestMatch && height < currentHeight)
        bestMatch = height + 1;
    }
    return bestMatch;
  }

  void Blockchain::invalidateSparseChainCache()
  {
    std::lock_guard<std::mutex> cacheLock(m_sparseChainCacheMutex);
    m_sparseChainCacheValid = false;
    m_cachedSparseChain.clear();
  }
  std::vector<crypto::Hash> Blockchain::getCachedSparseChain() { return buildSparseChain(); }
  std::vector<crypto::Hash> Blockchain::doBuildSparseChainUnlocked(const crypto::Hash &startBlockId) const { return doBuildSparseChain(startBlockId); }

  std::string Blockchain::printDatabaseStats() const
  {
#ifdef HAVE_MDBX
    if (m_useMdbx && m_mdbxStorage)
      return m_mdbxStorage->printDatabaseStats();
#endif
    return "MDBX not enabled";
  }

  void Blockchain::setCheckpointGeneratedCallback(CheckpointGeneratedCallback callback)
  {
    m_checkpointGeneratedCallback = std::move(callback);
  }
} // namespace cn
