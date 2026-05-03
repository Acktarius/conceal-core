// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "MDBXBlockchainStorage.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Common/StringTools.h"
#include "Serialization/SerializationTools.h"

#include <functional>
#include <stdexcept>
#include <cstring>

using namespace CryptoNote;
using namespace common;
using namespace cn;

static inline void *mdbx_cast(const void *ptr)
{
  return const_cast<void *>(ptr);
}

MDBXBlockchainStorage::MDBXBlockchainStorage(const std::string &dataDir, bool bulkSyncMode)
    : m_dataDir(dataDir)
{
  m_bulkSyncMode = bulkSyncMode;
  openEnvironment(dataDir);
}

MDBXBlockchainStorage::~MDBXBlockchainStorage()
{
  close();
}

void MDBXBlockchainStorage::openEnvironment(const std::string &path)
{
  int rc = mdbx_env_create(&m_env);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("mdbx_env_create failed: " + std::string(mdbx_strerror(rc)));

  auto envGuard = std::unique_ptr<MDBX_env, decltype(&mdbx_env_close)>(m_env, mdbx_env_close);

  mdbx_env_set_maxdbs(m_env, 8);

  mdbx_env_set_geometry(m_env,
                        -1,
                        -1,
                        (intptr_t)1 << 35,
                        16 << 20,
                        -1,
                        -1);

  const MDBX_env_flags_t openFlags = MDBX_NOSUBDIR | MDBX_NORDAHEAD | MDBX_COALESCE | MDBX_LIFORECLAIM;

  rc = mdbx_env_open(m_env, path.c_str(), openFlags, 0664);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("mdbx_env_open failed: " + std::string(mdbx_strerror(rc)));

  // 3x faster migration, never to be used elsewhere!
  if (m_bulkSyncMode)
    mdbx_env_set_flags(m_env, MDBX_SAFE_NOSYNC, true);

  MDBX_txn *txn;
  rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("txn_begin failed: " + std::string(mdbx_strerror(rc)));

  auto txnGuard = std::unique_ptr<MDBX_txn, std::function<void(MDBX_txn *)>>(
      txn, [](MDBX_txn *t)
      { if (t) mdbx_txn_abort(t); });

  openDatabases(txn);

  rc = mdbx_txn_commit(txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("initial txn_commit failed: " + std::string(mdbx_strerror(rc)));

  txnGuard.release();
  envGuard.release();
}

void MDBXBlockchainStorage::openDatabases(MDBX_txn *txn)
{
  mdbx_dbi_open(txn, "heights", MDBX_CREATE, &m_dbiHeights);
  mdbx_dbi_open(txn, "block_heights", MDBX_CREATE, &m_dbiBlockHeights);
  mdbx_dbi_open(txn, "meta", MDBX_CREATE, &m_dbiMeta);
  mdbx_dbi_open(txn, "block_entries", MDBX_CREATE, &m_dbiBlockEntries);
  mdbx_dbi_open(txn, "block_headers", MDBX_CREATE, &m_dbiBlockHeaders);
}

// ---------- Reads ----------
bool MDBXBlockchainStorage::blockExists(const crypto::Hash &hash) const
{
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  MDBX_val key{mdbx_cast(hash.data), sizeof(hash)};
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiBlockHeights, &key, &value);
  mdbx_txn_abort(txn);
  return rc == MDBX_SUCCESS;
}

bool MDBXBlockchainStorage::getBlock(const crypto::Hash &hash, cn::Block &block) const
{
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  MDBX_val key{mdbx_cast(hash.data), sizeof(hash)};
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiBlockHeights, &key, &value);
  if (rc != MDBX_SUCCESS || value.iov_len < sizeof(uint32_t))
  {
    mdbx_txn_abort(txn);
    return false;
  }
  uint32_t height = *static_cast<const uint32_t *>(value.iov_base);
  mdbx_txn_abort(txn);

  MDBX_txn *txn2;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn2);
  std::string heightKey = "be_" + std::to_string(height);
  MDBX_val entryKey{mdbx_cast(heightKey.data()), heightKey.size()};
  MDBX_val entryValue;
  rc = mdbx_get(txn2, m_dbiBlockEntries, &entryKey, &entryValue);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn2);
    return false;
  }

  std::string str(static_cast<const char *>(entryValue.iov_base), entryValue.iov_len);
  cn::BinaryArray ba = common::asBinaryArray(str);
  mdbx_txn_abort(txn2);

  cn::Block tempBlock;
  bool ok = cn::fromBinaryArray(tempBlock, ba);
  if (ok)
    block = tempBlock;
  return ok;
}

uint32_t MDBXBlockchainStorage::getBlockHeight(const crypto::Hash &hash) const
{
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  MDBX_val key{mdbx_cast(hash.data), sizeof(hash)};
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiBlockHeights, &key, &value);
  if (rc == MDBX_SUCCESS && value.iov_len >= sizeof(uint32_t))
  {
    uint32_t height = *static_cast<const uint32_t *>(value.iov_base);
    mdbx_txn_abort(txn);
    return height;
  }
  mdbx_txn_abort(txn);
  return 0;
}

crypto::Hash MDBXBlockchainStorage::getBlockHash(uint32_t height) const
{
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  MDBX_val key{&height, sizeof(height)};
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiHeights, &key, &value);
  if (rc == MDBX_SUCCESS && value.iov_len == sizeof(crypto::Hash))
  {
    crypto::Hash hash = *static_cast<const crypto::Hash *>(value.iov_base);
    mdbx_txn_abort(txn);
    return hash;
  }
  mdbx_txn_abort(txn);
  return crypto::Hash();
}

bool MDBXBlockchainStorage::isSpentKeyImage(const crypto::KeyImage &keyImage) const
{
  return false;
}

void MDBXBlockchainStorage::markKeyImageSpent(const crypto::KeyImage &keyImage)
{
  // handled in-memory via m_spent_keys and persisted to meta by storeCache
}

// ---------- Write operations ----------
void MDBXBlockchainStorage::addBlock(const cn::Block &block, const crypto::Hash &hash, uint32_t height)
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  MDBX_val key{mdbx_cast(hash.data), sizeof(hash)};
  MDBX_val hkey{&height, sizeof(height)};

  int rc = mdbx_put(m_writeTxn, m_dbiHeights, &hkey, &key, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("addBlock heights failed: " + std::string(mdbx_strerror(rc)));
  }

  rc = mdbx_put(m_writeTxn, m_dbiBlockHeights, &key, &hkey, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("addBlock block_heights failed: " + std::string(mdbx_strerror(rc)));
  }

  if (height > m_cachedTopHeight)
    m_cachedTopHeight = height;
  setTopBlockHeightInternal(height);

  ++m_opsSinceLastCommit;
  if (m_opsSinceLastCommit >= kCommitBatchSize)
    commitWriteTransaction(false);
}

void MDBXBlockchainStorage::removeBlock(const crypto::Hash &hash)
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  MDBX_val key{mdbx_cast(hash.data), sizeof(hash)};

  MDBX_val hval;
  if (mdbx_get(m_writeTxn, m_dbiBlockHeights, &key, &hval) == MDBX_SUCCESS)
  {
    mdbx_del(m_writeTxn, m_dbiHeights, &hval, nullptr);
  }

  mdbx_del(m_writeTxn, m_dbiBlockHeights, &key, nullptr);
  ++m_opsSinceLastCommit;
}

// ---------- Global state ----------
uint32_t MDBXBlockchainStorage::topBlockHeight() const
{
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  std::string metaKey = "top_height";
  MDBX_val key{mdbx_cast(metaKey.data()), metaKey.size()};
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiMeta, &key, &value);
  if (rc == MDBX_SUCCESS && value.iov_len >= sizeof(uint32_t))
  {
    uint32_t height = *static_cast<const uint32_t *>(value.iov_base);
    mdbx_txn_abort(txn);
    return height;
  }
  mdbx_txn_abort(txn);
  return 0;
}

void MDBXBlockchainStorage::setTopBlockHeight(uint32_t height)
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();
  setTopBlockHeightInternal(height);
  commitWriteTransaction(false);
}

void MDBXBlockchainStorage::setTopBlockHeightInternal(uint32_t height)
{
  std::string metaKey = "top_height";
  MDBX_val key{mdbx_cast(metaKey.data()), metaKey.size()};
  MDBX_val value{&height, sizeof(height)};
  int rc = mdbx_put(m_writeTxn, m_dbiMeta, &key, &value, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("setTopBlockHeightInternal failed: " + std::string(mdbx_strerror(rc)));
  }
}

// ---------- Helpers ----------
void MDBXBlockchainStorage::ensureWriteTxn()
{
  if (!m_writeTxn)
  {
    int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &m_writeTxn);
    if (rc != MDBX_SUCCESS)
      throw std::runtime_error("failed to begin write txn: " + std::string(mdbx_strerror(rc)));
  }
}

void MDBXBlockchainStorage::flush()
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  commitWriteTransaction(true);
}

void MDBXBlockchainStorage::close()
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  if (m_writeTxn)
  {
    mdbx_txn_abort(m_writeTxn);
    m_writeTxn = nullptr;
    m_opsSinceLastCommit = 0;
  }
  if (m_env)
  {
    mdbx_env_close(m_env);
    m_env = nullptr;
  }
}

void MDBXBlockchainStorage::commitWriteTransaction(bool force)
{
  if (!m_writeTxn)
    return;
  if (force || m_opsSinceLastCommit > 0)
  {
    int rc = mdbx_txn_commit(m_writeTxn);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(m_writeTxn);
      m_writeTxn = nullptr;
      m_opsSinceLastCommit = 0;
      throw std::runtime_error("commit failed: " + std::string(mdbx_strerror(rc)));
    }
  }
  else
  {
    mdbx_txn_abort(m_writeTxn);
  }
  m_writeTxn = nullptr;
  m_opsSinceLastCommit = 0;
}

// ---------- BlockEntry storage (serialized to/from BinaryArray) ----------
void MDBXBlockchainStorage::pushBlockEntry(uint32_t height, const cn::BinaryArray &serializedEntry)
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  std::string key = "be_" + std::to_string(height);
  MDBX_val mkey{mdbx_cast(key.data()), key.size()};
  MDBX_val mval{mdbx_cast(serializedEntry.data()), serializedEntry.size()};
  int rc = mdbx_put(m_writeTxn, m_dbiBlockEntries, &mkey, &mval, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("pushBlockEntry failed: " + std::string(mdbx_strerror(rc)));
  }

  ++m_opsSinceLastCommit;
  if (m_opsSinceLastCommit >= kCommitBatchSize)
    commitWriteTransaction(false);
}

bool MDBXBlockchainStorage::getBlockEntry(uint32_t height, cn::BinaryArray &serializedEntry) const
{
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);

  std::string key = "be_" + std::to_string(height);
  MDBX_val mkey{mdbx_cast(key.data()), key.size()};
  MDBX_val mval;
  int rc = mdbx_get(txn, m_dbiBlockEntries, &mkey, &mval);
  if (rc == MDBX_SUCCESS)
  {
    serializedEntry.assign(
        static_cast<const uint8_t *>(mval.iov_base),
        static_cast<const uint8_t *>(mval.iov_base) + mval.iov_len);
    mdbx_txn_abort(txn);
    return true;
  }
  mdbx_txn_abort(txn);
  return false;
}

void MDBXBlockchainStorage::popBlockEntry(uint32_t height)
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  std::string key = "be_" + std::to_string(height);
  MDBX_val mkey{mdbx_cast(key.data()), key.size()};
  mdbx_del(m_writeTxn, m_dbiBlockEntries, &mkey, nullptr);
  ++m_opsSinceLastCommit;
}

void MDBXBlockchainStorage::setInitialized()
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();
  std::string key = "initialized";
  MDBX_val mkey{mdbx_cast(key.data()), key.size()};
  uint8_t val = 1;
  MDBX_val mval{&val, sizeof(val)};
  int rc = mdbx_put(m_writeTxn, m_dbiMeta, &mkey, &mval, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("setInitialized failed: " + std::string(mdbx_strerror(rc)));
  }
  commitWriteTransaction(true);
}

bool MDBXBlockchainStorage::isInitialized() const
{
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  std::string key = "initialized";
  MDBX_val mkey{mdbx_cast(key.data()), key.size()};
  MDBX_val mval;
  int rc = mdbx_get(txn, m_dbiMeta, &mkey, &mval);
  mdbx_txn_abort(txn);
  return rc == MDBX_SUCCESS;
}

void MDBXBlockchainStorage::pushBlockHeader(uint32_t height, const cn::BlockHeaderPOD &hdr)
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();
  std::string key = "hdr_" + std::to_string(height);
  MDBX_val mkey{mdbx_cast(key.data()), key.size()};
  MDBX_val mval{mdbx_cast(&hdr), sizeof(hdr)};
  int rc = mdbx_put(m_writeTxn, m_dbiBlockHeaders, &mkey, &mval, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("pushBlockHeader failed: " + std::string(mdbx_strerror(rc)));
  }
  ++m_opsSinceLastCommit;
}

bool MDBXBlockchainStorage::getBlockHeader(uint32_t height, cn::BlockHeaderPOD &hdr) const
{
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  std::string key = "hdr_" + std::to_string(height);
  MDBX_val mkey{mdbx_cast(key.data()), key.size()};
  MDBX_val mval;
  int rc = mdbx_get(txn, m_dbiBlockHeaders, &mkey, &mval);
  if (rc == MDBX_SUCCESS && mval.iov_len == sizeof(cn::BlockHeaderPOD))
  {
    memcpy(&hdr, mval.iov_base, sizeof(hdr));
    mdbx_txn_abort(txn);
    return true;
  }
  mdbx_txn_abort(txn);
  return false;
}

void MDBXBlockchainStorage::putMeta(const std::string &key, const std::vector<uint8_t> &value)
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();
  MDBX_val mk{mdbx_cast(key.data()), key.size()};
  MDBX_val mv{mdbx_cast(value.data()), value.size()};
  int rc = mdbx_put(m_writeTxn, m_dbiMeta, &mk, &mv, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("putMeta failed: " + std::string(mdbx_strerror(rc)));
  }
}

bool MDBXBlockchainStorage::getMeta(const std::string &key, std::vector<uint8_t> &value) const
{
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  MDBX_val mk{mdbx_cast(key.data()), key.size()};
  MDBX_val mv;
  int rc = mdbx_get(txn, m_dbiMeta, &mk, &mv);
  if (rc == MDBX_SUCCESS)
  {
    value.assign(static_cast<const uint8_t *>(mv.iov_base),
                 static_cast<const uint8_t *>(mv.iov_base) + mv.iov_len);
    mdbx_txn_abort(txn);
    return true;
  }
  mdbx_txn_abort(txn);
  return false;
}

void MDBXBlockchainStorage::abortWriteTxn()
{
  if (m_writeTxn)
  {
    mdbx_txn_abort(m_writeTxn);
    m_writeTxn = nullptr;
    m_opsSinceLastCommit = 0;
  }
}

std::string MDBXBlockchainStorage::printDatabaseStats() const
{
  std::string result;

  MDBX_txn *txn;
  if (mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS)
    return "error: failed to begin transaction";

  MDBX_dbi dbis[] = {m_dbiHeights, m_dbiBlockHeights,
                     m_dbiMeta, m_dbiBlockEntries, m_dbiBlockHeaders};
  const char *names[] = {"heights", "block_heights",
                         "meta", "block_entries", "block_headers"};

  for (int i = 0; i < 5; i++)
  {
    MDBX_stat stat;
    if (mdbx_dbi_stat(txn, dbis[i], &stat, sizeof(stat)) == MDBX_SUCCESS)
    {
      size_t dataMB = (stat.ms_leaf_pages * 4096) >> 20;
      char line[128];
      snprintf(line, sizeof(line), "  %-18s %8lu entries  %6zu MB\n",
               names[i], (unsigned long)stat.ms_entries, dataMB);
      result += line;
    }
  }

  mdbx_txn_abort(txn);
  return result;
}