// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "MDBXBlockchainStorage.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Common/StringTools.h"
#include "Serialization/SerializationTools.h"

using namespace CryptoNote;
using namespace common;
using namespace cn;

static inline void *mdbx_cast(const void *ptr)
{
  return const_cast<void *>(ptr);
}

MDBXBlockchainStorage::MDBXBlockchainStorage(const std::string &dataDir)
    : m_dataDir(dataDir)
{
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
    throw std::runtime_error("mdbx_env_create failed");

  mdbx_env_set_maxdbs(m_env, 10);

  mdbx_env_set_geometry(m_env, -1, -1, (intptr_t)1 << 40, -1, -1, -1);

  rc = mdbx_env_open(m_env, path.c_str(), MDBX_NOSUBDIR | MDBX_WRITEMAP, 0664);

  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("mdbx_env_open failed");

  mdbx_env_set_flags(m_env, MDBX_SAFE_NOSYNC, true);

  MDBX_txn *txn;
  rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("txn_begin failed");
  openDatabases(txn);
  rc = mdbx_txn_commit(txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("initial txn_commit failed");
}

void MDBXBlockchainStorage::openDatabases(MDBX_txn *txn)
{
  mdbx_dbi_open(txn, "blocks", MDBX_CREATE, &m_dbiBlocks);
  mdbx_dbi_open(txn, "heights", MDBX_CREATE, &m_dbiHeights);
  mdbx_dbi_open(txn, "block_heights", MDBX_CREATE, &m_dbiBlockHeights);
  mdbx_dbi_open(txn, "spent_keys", MDBX_CREATE, &m_dbiSpentKeys);
  mdbx_dbi_open(txn, "meta", MDBX_CREATE, &m_dbiMeta);
  mdbx_dbi_open(txn, "block_entries", MDBX_CREATE, &m_dbiBlockEntries);
}

// ---------- Reads ----------
bool MDBXBlockchainStorage::blockExists(const crypto::Hash &hash) const
{
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  MDBX_val key{mdbx_cast(hash.data), sizeof(hash)};
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiBlocks, &key, &value);
  mdbx_txn_abort(txn);
  return rc == MDBX_SUCCESS;
}

bool MDBXBlockchainStorage::getBlock(const crypto::Hash &hash, cn::Block &block) const
{
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  MDBX_val key{mdbx_cast(hash.data), sizeof(hash)};
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiBlocks, &key, &value);
  if (rc == MDBX_SUCCESS)
  {
    std::string str(static_cast<const char *>(value.iov_base), value.iov_len);
    cn::BinaryArray ba = common::asBinaryArray(str);
    cn::Block tempBlock;
    bool ok = cn::fromBinaryArray(tempBlock, ba);
    if (ok)
    {
      block = tempBlock;
    }
    mdbx_txn_abort(txn);
    return ok;
  }
  mdbx_txn_abort(txn);
  return false;
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
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  MDBX_val key{mdbx_cast(keyImage.data), sizeof(keyImage)};
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiSpentKeys, &key, &value);
  mdbx_txn_abort(txn);
  return rc == MDBX_SUCCESS;
}

void MDBXBlockchainStorage::markKeyImageSpent(const crypto::KeyImage &keyImage)
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  MDBX_val key{mdbx_cast(keyImage.data), sizeof(keyImage)};
  MDBX_val empty{nullptr, 0};
  mdbx_put(m_writeTxn, m_dbiSpentKeys, &key, &empty, MDBX_UPSERT);
  ++m_opsSinceLastCommit;
}

// ---------- Write operations ----------
void MDBXBlockchainStorage::addBlock(const cn::Block &block, const crypto::Hash &hash, uint32_t height)
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  cn::BinaryArray ba = cn::toBinaryArray(block);
  std::string buffer = common::asString(ba);

  MDBX_val key{mdbx_cast(hash.data), sizeof(hash)};
  MDBX_val blkValue{mdbx_cast(buffer.data()), buffer.size()};
  int rc = mdbx_put(m_writeTxn, m_dbiBlocks, &key, &blkValue, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("mdbx_put block failed");

  MDBX_val hkey{&height, sizeof(height)};
  rc = mdbx_put(m_writeTxn, m_dbiHeights, &hkey, &key, MDBX_UPSERT);
  rc = mdbx_put(m_writeTxn, m_dbiBlockHeights, &key, &hkey, MDBX_UPSERT);

  // Track height in-memory instead of reading from DB (avoids read tx conflict)
  if (height > m_cachedTopHeight)
  {
    m_cachedTopHeight = height;
  }
  setTopBlockHeightInternal(height);

  ++m_opsSinceLastCommit;
  if (m_opsSinceLastCommit >= kCommitBatchSize)
  {
    commitWriteTransaction(false);
  }
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
  mdbx_del(m_writeTxn, m_dbiBlocks, &key, nullptr);
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
  // Rely on batch commit counter instead of forcing a commit on every block
  commitWriteTransaction(false);
}

void MDBXBlockchainStorage::setTopBlockHeightInternal(uint32_t height)
{
  std::string metaKey = "top_height";
  MDBX_val key{mdbx_cast(metaKey.data()), metaKey.size()};
  MDBX_val value{&height, sizeof(height)};
  mdbx_put(m_writeTxn, m_dbiMeta, &key, &value, MDBX_UPSERT);
}

// ---------- Helpers ----------
void MDBXBlockchainStorage::ensureWriteTxn()
{
  if (!m_writeTxn)
  {
    int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &m_writeTxn);
    if (rc != MDBX_SUCCESS)
      throw std::runtime_error("failed to begin write txn");
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
    commitWriteTransaction(true);
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
      throw std::runtime_error("commit failed");
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
    throw std::runtime_error("pushBlockEntry failed");

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
  mdbx_put(m_writeTxn, m_dbiMeta, &mkey, &mval, MDBX_UPSERT);
  commitWriteTransaction(true); // force commit
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