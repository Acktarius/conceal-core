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
#include <iomanip>
#include <sstream>

using namespace CryptoNote;
using namespace common;
using namespace cn;

// Builds an MDBX_val from any raw pointer+length — used for both RO and RW transactions
// MDBX_val.iov_base is non‑const in the API, but the library treats it as const for RO txns
MDBX_val MDBXBlockchainStorage::to_val(const void *data, size_t len)
{
  MDBX_val v;
  v.iov_base = const_cast<void *>(data); // safe: MDBX doesn't mutate data in RO txns, copies it in RW txns
  v.iov_len = len;
  return v;
}

// Convenience overload for std::string keys/values
MDBX_val MDBXBlockchainStorage::to_val(const std::string &s)
{
  return to_val(s.data(), s.size());
}

// Convenience overload for crypto::Hash keys/values (always 32 bytes)
MDBX_val MDBXBlockchainStorage::to_val(const crypto::Hash &h)
{
  return to_val(h.data, sizeof(h));
}

// Convenience overload for BlockHeaderPOD values
MDBX_val MDBXBlockchainStorage::to_val(const cn::BlockHeaderPOD &hdr)
{
  return to_val(&hdr, sizeof(hdr));
}

// Returns "be_00000042" for height 42 — padding width supports up to 99,999,999 blocks
std::string MDBXBlockchainStorage::blockEntryKey(uint32_t height)
{
  std::ostringstream oss;
  oss << "be_" << std::setw(kHeightKeyWidth) << std::setfill('0') << height;
  return oss.str();
}

// Returns "hdr_00000042" for height 42
std::string MDBXBlockchainStorage::blockHeaderKey(uint32_t height)
{
  std::ostringstream oss;
  oss << "hdr_" << std::setw(kHeightKeyWidth) << std::setfill('0') << height;
  return oss.str();
}

// Returns "checkpoint_0000042" — standardised to 7 digits (existing format kept for compatibility)
std::string MDBXBlockchainStorage::checkpointKey(uint32_t height)
{
  std::ostringstream oss;
  oss << "checkpoint_" << std::setw(kHeightKeyWidth) << std::setfill('0') << height;
  return oss.str();
}

MDBXBlockchainStorage::MDBXBlockchainStorage(const std::string &dataDir, bool bulkSyncMode, uint64_t sizeLimitBytes)
    : m_dataDir(dataDir), m_sizeLimitBytes(sizeLimitBytes), m_bulkSyncMode(bulkSyncMode)
{
  openEnvironment(dataDir);
  m_cachedTopHeight = topBlockHeight(); // Seed the cache so MDBX_APPEND works immediately after restart
}

MDBXBlockchainStorage::~MDBXBlockchainStorage()
{
  close(); // Aborts any pending write txn and closes the environment
}

void MDBXBlockchainStorage::openEnvironment(const std::string &path)
{
  // Create the MDBX environment handle
  int rc = mdbx_env_create(&m_env);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("mdbx_env_create failed: " + std::string(mdbx_strerror(rc)));

  // RAII guard: closes environment if we throw before releasing it
  auto envGuard = std::unique_ptr<MDBX_env, decltype(&mdbx_env_close)>(m_env, mdbx_env_close);

  // Allow up to 12 named databases
  mdbx_env_set_maxdbs(m_env, 12);

  // Configure mmap geometry: larger lower bound (256 MB) avoids frequent resizes on growing databases
  // MDBX_SAFE_NOSYNC is intentionally NOT used — it causes excessive database bloat (10 GB → 100+ GB)
  // The migration tool may apply NOSYNC externally on the raw env handle if needed
  if (m_bulkSyncMode)
  {
    if (m_sizeLimitBytes > 0)
      mdbx_env_set_geometry(m_env, -1, -1, (intptr_t)m_sizeLimitBytes, 256 << 20, -1, -1);
    else
      mdbx_env_set_geometry(m_env, -1, -1, -1, 256 << 20, -1, -1);
  }
  else
  {
    // Normal mode: allow up to 64 GB growth, 256 MB initial size
    mdbx_env_set_geometry(m_env, -1, -1, (intptr_t)1 << 36, 256 << 20, -1, -1);
  }

  // Open flags: no external subdir, no readahead (SSD optimisation), coalesce free pages, LIFO reclaim
  const MDBX_env_flags_t openFlags = MDBX_NOSUBDIR | MDBX_NORDAHEAD | MDBX_COALESCE | MDBX_LIFORECLAIM;

  rc = mdbx_env_open(m_env, path.c_str(), openFlags, 0664);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("mdbx_env_open failed: " + std::string(mdbx_strerror(rc)));

  // Open a write transaction to create/verify all named databases
  MDBX_txn *txn;
  rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("txn_begin failed: " + std::string(mdbx_strerror(rc)));

  // RAII guard: aborts the txn if we throw before committing
  auto txnGuard = std::unique_ptr<MDBX_txn, std::function<void(MDBX_txn *)>>(
      txn, [](MDBX_txn *t)
      { if (t) mdbx_txn_abort(t); });

  openDatabases(txn); // Creates/opens all named databases

  rc = mdbx_txn_commit(txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("initial txn_commit failed: " + std::string(mdbx_strerror(rc)));

  txnGuard.release(); // Transaction committed successfully — don't abort
  envGuard.release(); // Environment is in a valid state — don't close
}

bool MDBXBlockchainStorage::blockExists(const crypto::Hash &hash) const
{
  // Single read txn: check if the hash exists in the block_heights database
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  MDBX_val key = to_val(hash);
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiBlockHeights, &key, &value);
  mdbx_txn_abort(txn);
  return rc == MDBX_SUCCESS;
}

bool MDBXBlockchainStorage::getBlock(const crypto::Hash &hash, cn::Block &block) const
{
  // Single read txn: look up height from hash, then fetch the serialised block entry
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);

  // Step 1: hash → height
  MDBX_val key = to_val(hash);
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiBlockHeights, &key, &value);
  if (rc != MDBX_SUCCESS || value.iov_len < sizeof(uint32_t))
  {
    mdbx_txn_abort(txn);
    return false;
  }
  uint32_t height = *static_cast<const uint32_t *>(value.iov_base);

  // Step 2: height → serialised block entry (same transaction — zero‑copy read)
  std::string beKey = blockEntryKey(height);
  MDBX_val entryKey = to_val(beKey);
  MDBX_val entryValue;
  rc = mdbx_get(txn, m_dbiBlockEntries, &entryKey, &entryValue);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    return false;
  }

  // Step 3: deserialise binary array → Block struct
  std::string str(static_cast<const char *>(entryValue.iov_base), entryValue.iov_len);
  cn::BinaryArray ba = common::asBinaryArray(str);
  mdbx_txn_abort(txn); // All reads done — release the transaction

  cn::Block tempBlock;
  bool ok = cn::fromBinaryArray(tempBlock, ba);
  if (ok)
    block = tempBlock;
  return ok;
}

uint32_t MDBXBlockchainStorage::getBlockHeight(const crypto::Hash &hash) const
{
  // Single read txn: hash → height lookup
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  MDBX_val key = to_val(hash);
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiBlockHeights, &key, &value);
  if (rc == MDBX_SUCCESS && value.iov_len >= sizeof(uint32_t))
  {
    uint32_t height = *static_cast<const uint32_t *>(value.iov_base);
    mdbx_txn_abort(txn);
    return height;
  }
  mdbx_txn_abort(txn);
  return 0; // Not found → return 0 (genesis or unknown)
}

crypto::Hash MDBXBlockchainStorage::getBlockHash(uint32_t height) const
{
  // Single read txn: height → hash lookup
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  MDBX_val key = to_val(&height, sizeof(height));
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiHeights, &key, &value);
  if (rc == MDBX_SUCCESS && value.iov_len == sizeof(crypto::Hash))
  {
    crypto::Hash hash = *static_cast<const crypto::Hash *>(value.iov_base);
    mdbx_txn_abort(txn);
    return hash;
  }
  mdbx_txn_abort(txn);
  return crypto::Hash(); // Not found → return null hash
}

bool MDBXBlockchainStorage::getBlockHeader(uint32_t height, cn::BlockHeaderPOD &hdr) const
{
  // Single read txn: fetch header by padded height key
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  std::string key = blockHeaderKey(height);
  MDBX_val mkey = to_val(key);
  MDBX_val mval;
  int rc = mdbx_get(txn, m_dbiBlockHeaders, &mkey, &mval);
  if (rc == MDBX_SUCCESS && mval.iov_len == sizeof(cn::BlockHeaderPOD))
  {
    memcpy(&hdr, mval.iov_base, sizeof(hdr)); // POD copy — no fallback needed when headers are always complete
    mdbx_txn_abort(txn);
    return true;
  }
  mdbx_txn_abort(txn);
  return false;
}

void MDBXBlockchainStorage::getBlockHeadersRange(uint32_t startHeight, uint32_t count,
                                                 std::vector<cn::BlockHeaderPOD> &out) const
{
  // Opens a single read transaction and cursor to fetch [startHeight, startHeight+count) headers
  // Massively faster than calling getBlockHeader() N times (avoids N txn begin/abort)
  out.clear();
  out.reserve(count);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return;

  MDBX_cursor *cursor;
  rc = mdbx_cursor_open(txn, m_dbiBlockHeaders, &cursor);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    return;
  }

  // Position cursor at the first desired header key
  std::string startKey = blockHeaderKey(startHeight);
  MDBX_val mkey = to_val(startKey);
  MDBX_val mval;
  rc = mdbx_cursor_get(cursor, &mkey, &mval, MDBX_SET_RANGE); // Seeks to key >= startKey

  // Iterate forward, collecting headers until we have 'count' or run out of keys
  while (rc == MDBX_SUCCESS && out.size() < count)
  {
    // Verify the key actually belongs to a header (should always match "hdr_")
    if (mkey.iov_len > 4 && memcmp(mkey.iov_base, "hdr_", 4) == 0 &&
        mval.iov_len == sizeof(cn::BlockHeaderPOD))
    {
      cn::BlockHeaderPOD hdr;
      memcpy(&hdr, mval.iov_base, sizeof(hdr));
      out.push_back(hdr);
    }
    rc = mdbx_cursor_get(cursor, &mkey, &mval, MDBX_NEXT);
  }

  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
}

void MDBXBlockchainStorage::pushCompleteBlock(uint32_t height,
                                              const crypto::Hash &hash,
                                              const cn::BinaryArray &serializedEntry,
                                              const cn::BlockHeaderPOD &hdr)
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  // 1. Bi-directional height ↔ hash mapping
  MDBX_val hkey = to_val(&height, sizeof(height));
  MDBX_val hashVal = to_val(hash);
  int rc = mdbx_put(m_writeTxn, m_dbiHeights, &hkey, &hashVal, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("pushCompleteBlock heights failed: " + std::string(mdbx_strerror(rc)));
  }

  rc = mdbx_put(m_writeTxn, m_dbiBlockHeights, &hashVal, &hkey, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("pushCompleteBlock block_heights failed: " + std::string(mdbx_strerror(rc)));
  }

  // 2. Serialised block entry with padded key and MDBX_APPEND when height is strictly increasing
  std::string beKey = blockEntryKey(height);
  MDBX_val bkey = to_val(beKey);
  MDBX_val bval = to_val(serializedEntry.data(), serializedEntry.size());
  MDBX_put_flags_t putFlags = MDBX_UPSERT;
  if (height == m_cachedTopHeight + 1)
    putFlags = static_cast<MDBX_put_flags_t>(putFlags | MDBX_APPEND); // O(1) insertion during sequential sync
  rc = mdbx_put(m_writeTxn, m_dbiBlockEntries, &bkey, &bval, putFlags);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("pushCompleteBlock block_entries failed: " + std::string(mdbx_strerror(rc)));
  }

  // 3. Block header (same MDBX_APPEND optimisation)
  std::string hdrKey = blockHeaderKey(height);
  MDBX_val hdrk = to_val(hdrKey);
  MDBX_val hdrv = to_val(hdr);
  MDBX_put_flags_t hdrFlags = MDBX_UPSERT;
  if (height == m_cachedTopHeight + 1)
    hdrFlags = static_cast<MDBX_put_flags_t>(hdrFlags | MDBX_APPEND);
  rc = mdbx_put(m_writeTxn, m_dbiBlockHeaders, &hdrk, &hdrv, hdrFlags);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("pushCompleteBlock headers failed: " + std::string(mdbx_strerror(rc)));
  }

  // 4. Update cached + persisted top height
  if (height > m_cachedTopHeight)
    m_cachedTopHeight = height;
  setTopBlockHeightInternal(height);

  // 5. Batch commit: only flush when the counter hits the threshold
  ++m_opsSinceLastCommit;
  size_t batchSize = m_bulkSyncMode ? kCommitBatchSizeBulk : kCommitBatchSize;
  if (m_opsSinceLastCommit >= batchSize)
    commitWriteTransaction(false);
}

void MDBXBlockchainStorage::addBlock(const cn::Block &block, const crypto::Hash &hash, uint32_t height)
{
  // Legacy method — prefer pushCompleteBlock() for new blocks
  // Still necessary for reorg scenarios where blocks are added piecemeal
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  MDBX_val key = to_val(hash);
  MDBX_val hkey = to_val(&height, sizeof(height));

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
  size_t batchSize = m_bulkSyncMode ? kCommitBatchSizeBulk : kCommitBatchSize;
  if (m_opsSinceLastCommit >= batchSize)
    commitWriteTransaction(false);
}

void MDBXBlockchainStorage::removeBlock(const crypto::Hash &hash)
{
  // Removes both height→hash and hash→height mappings for a reorg
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  MDBX_val key = to_val(hash);
  MDBX_val hval;
  if (mdbx_get(m_writeTxn, m_dbiBlockHeights, &key, &hval) == MDBX_SUCCESS)
  {
    mdbx_del(m_writeTxn, m_dbiHeights, &hval, nullptr); // Remove height→hash entry
  }
  mdbx_del(m_writeTxn, m_dbiBlockHeights, &key, nullptr); // Remove hash→height entry
  ++m_opsSinceLastCommit;
}

void MDBXBlockchainStorage::removeCompleteBlock(uint32_t height, const crypto::Hash &hash)
{
  // Removes header, block entry, and both height mappings in one transaction
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  // 1. Remove block header
  std::string hdrKey = blockHeaderKey(height);
  MDBX_val hk = to_val(hdrKey);
  mdbx_del(m_writeTxn, m_dbiBlockHeaders, &hk, nullptr);

  // 2. Remove serialised block entry
  std::string beKey = blockEntryKey(height);
  MDBX_val bk = to_val(beKey);
  mdbx_del(m_writeTxn, m_dbiBlockEntries, &bk, nullptr);

  // 3. Remove hash→height mapping
  MDBX_val hashKey = to_val(hash);
  mdbx_del(m_writeTxn, m_dbiBlockHeights, &hashKey, nullptr);

  // 4. Remove height→hash mapping
  MDBX_val hval = to_val(&height, sizeof(height));
  mdbx_del(m_writeTxn, m_dbiHeights, &hval, nullptr);

  // 5. Decrement top height if we removed the tip
  if (height == m_cachedTopHeight && height > 0)
    m_cachedTopHeight = height - 1;
  setTopBlockHeightInternal(m_cachedTopHeight);

  commitWriteTransaction(true); // Force immediate commit — reorgs must be durable
}

void MDBXBlockchainStorage::removeBlockHeader(uint32_t height)
{
  // Removes just the header (used during partial reorg cleanup)
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();
  std::string key = blockHeaderKey(height);
  MDBX_val mkey = to_val(key);
  mdbx_del(m_writeTxn, m_dbiBlockHeaders, &mkey, nullptr);
}

uint32_t MDBXBlockchainStorage::topBlockHeight() const
{
  // Reads the persisted top height from the meta database — always accurate
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  std::string metaKey = "top_height";
  MDBX_val key = to_val(metaKey);
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiMeta, &key, &value);
  if (rc == MDBX_SUCCESS && value.iov_len >= sizeof(uint32_t))
  {
    uint32_t height = *static_cast<const uint32_t *>(value.iov_base);
    mdbx_txn_abort(txn);
    return height;
  }
  mdbx_txn_abort(txn);
  return 0; // Empty database → start at height 0
}

void MDBXBlockchainStorage::setTopBlockHeight(uint32_t height)
{
  // Public setter: acquires lock, writes height, and commits
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();
  setTopBlockHeightInternal(height);
  commitWriteTransaction(false);
}

void MDBXBlockchainStorage::setTopBlockHeightInternal(uint32_t height)
{
  // Writes the height to meta DB — does NOT commit (caller controls when to flush)
  std::string metaKey = "top_height";
  MDBX_val key = to_val(metaKey);
  MDBX_val value = to_val(&height, sizeof(height));
  int rc = mdbx_put(m_writeTxn, m_dbiMeta, &key, &value, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("setTopBlockHeightInternal failed: " + std::string(mdbx_strerror(rc)));
  }
}

void MDBXBlockchainStorage::ensureWriteTxn()
{
  // Lazily starts a write transaction when needed — reused across multiple ops
  if (!m_writeTxn)
  {
    int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &m_writeTxn);
    if (rc != MDBX_SUCCESS)
      throw std::runtime_error("failed to begin write txn: " + std::string(mdbx_strerror(rc)));
  }
}

void MDBXBlockchainStorage::flush()
{
  // Forces all buffered writes to disk immediately
  std::lock_guard<std::mutex> lock(m_txMutex);
  commitWriteTransaction(true);
}

void MDBXBlockchainStorage::close()
{
  // Aborts any pending write txn, then closes the environment
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
  // Commits the active write txn if there are ops to flush, or if forced
  if (!m_writeTxn)
    return;
  if (force || m_opsSinceLastCommit > 0)
  {
    int rc = mdbx_txn_commit(m_writeTxn);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(m_writeTxn); // Commit failed — abort to release locks
      m_writeTxn = nullptr;
      m_opsSinceLastCommit = 0;
      throw std::runtime_error("commit failed: " + std::string(mdbx_strerror(rc)));
    }
  }
  else
  {
    mdbx_txn_abort(m_writeTxn); // No ops to commit — just abort (cheaper)
  }
  m_writeTxn = nullptr;
  m_opsSinceLastCommit = 0;
}

void MDBXBlockchainStorage::abortWriteTxn()
{
  // Discards the current write transaction (used on error paths)
  if (m_writeTxn)
  {
    mdbx_txn_abort(m_writeTxn);
    m_writeTxn = nullptr;
    m_opsSinceLastCommit = 0;
  }
}

void MDBXBlockchainStorage::pushBlockEntry(uint32_t height, const cn::BinaryArray &serializedEntry)
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  std::string key = blockEntryKey(height);
  MDBX_val mkey = to_val(key);
  MDBX_val mval = to_val(serializedEntry.data(), serializedEntry.size());
  MDBX_put_flags_t putFlags = MDBX_UPSERT;
  if (height == m_cachedTopHeight + 1)
    putFlags = static_cast<MDBX_put_flags_t>(putFlags | MDBX_APPEND);
  int rc = mdbx_put(m_writeTxn, m_dbiBlockEntries, &mkey, &mval, putFlags);
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
  // Reads a serialised block entry from the database
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);

  std::string key = blockEntryKey(height);
  MDBX_val mkey = to_val(key);
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
  // Deletes a block entry (used during reorg pop operations)
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  std::string key = blockEntryKey(height);
  MDBX_val mkey = to_val(key);
  mdbx_del(m_writeTxn, m_dbiBlockEntries, &mkey, nullptr);
  ++m_opsSinceLastCommit;
}

void MDBXBlockchainStorage::setInitialized()
{
  // Marks the database as fully initialised (writes a sentinel key to meta)
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();
  std::string key = "initialized";
  MDBX_val mkey = to_val(key);
  uint8_t val = 1;
  MDBX_val mval = to_val(&val, sizeof(val));
  int rc = mdbx_put(m_writeTxn, m_dbiMeta, &mkey, &mval, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("setInitialized failed: " + std::string(mdbx_strerror(rc)));
  }
  commitWriteTransaction(true); // Force durable commit — must survive crashes
}

bool MDBXBlockchainStorage::isInitialized() const
{
  // Checks if the database has been marked as initialised
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  std::string key = "initialized";
  MDBX_val mkey = to_val(key);
  MDBX_val mval;
  int rc = mdbx_get(txn, m_dbiMeta, &mkey, &mval);
  mdbx_txn_abort(txn);
  return rc == MDBX_SUCCESS;
}

void MDBXBlockchainStorage::putMeta(const std::string &key, const std::vector<uint8_t> &value)
{
  // Stores an arbitrary key‑value blob in the meta database
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();
  MDBX_val mk = to_val(key);
  MDBX_val mv = to_val(value.data(), value.size());
  int rc = mdbx_put(m_writeTxn, m_dbiMeta, &mk, &mv, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("putMeta failed: " + std::string(mdbx_strerror(rc)));
  }
}

bool MDBXBlockchainStorage::getMeta(const std::string &key, std::vector<uint8_t> &value) const
{
  // Reads an arbitrary key‑value blob from the meta database
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  MDBX_val mk = to_val(key);
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

void MDBXBlockchainStorage::pushBlockHeader(uint32_t height, const cn::BlockHeaderPOD &hdr)
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();
  std::string key = blockHeaderKey(height);
  MDBX_val mkey = to_val(key);
  MDBX_val mval = to_val(hdr);
  MDBX_put_flags_t putFlags = MDBX_UPSERT;
  if (height == m_cachedTopHeight + 1)
    putFlags = static_cast<MDBX_put_flags_t>(putFlags | MDBX_APPEND);
  int rc = mdbx_put(m_writeTxn, m_dbiBlockHeaders, &mkey, &mval, putFlags);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("pushBlockHeader failed: " + std::string(mdbx_strerror(rc)));
  }
  ++m_opsSinceLastCommit;
}

void MDBXBlockchainStorage::storeCheckpoint(uint32_t height, const crypto::Hash &hash)
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  std::string key = checkpointKey(height);
  MDBX_val mkey = to_val(key);
  MDBX_val mval = to_val(hash);

  MDBX_put_flags_t putFlags = MDBX_UPSERT;
  if (height == m_cachedTopHeight + 1)
    putFlags = static_cast<MDBX_put_flags_t>(putFlags | MDBX_APPEND);

  int rc = mdbx_put(m_writeTxn, m_dbiCheckpoints, &mkey, &mval, putFlags);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("storeCheckpoint failed: " + std::string(mdbx_strerror(rc)));
  }

  ++m_opsSinceLastCommit;
  if (m_opsSinceLastCommit >= kCommitBatchSize)
    commitWriteTransaction(false);
}

std::vector<std::pair<uint32_t, crypto::Hash>> MDBXBlockchainStorage::getCheckpoints() const
{
  // Scans the checkpoints database with a cursor and returns all (height, hash) pairs
  std::vector<std::pair<uint32_t, crypto::Hash>> result;

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return result;

  MDBX_cursor *cursor;
  rc = mdbx_cursor_open(txn, m_dbiCheckpoints, &cursor);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    return result;
  }

  MDBX_val key, value;
  while (mdbx_cursor_get(cursor, &key, &value, MDBX_NEXT) == MDBX_SUCCESS)
  {
    std::string keyStr(static_cast<const char *>(key.iov_base), key.iov_len);

    // Filter: only process keys starting with "checkpoint_"
    if (keyStr.rfind("checkpoint_", 0) != 0)
      continue;

    // Extract the numeric height from the key (skip the 11‑char prefix "checkpoint_")
    uint32_t height = 0;
    try
    {
      height = static_cast<uint32_t>(std::stoul(keyStr.substr(11)));
    }
    catch (...)
    {
      continue; // Malformed key → skip
    }

    if (value.iov_len == sizeof(crypto::Hash))
    {
      crypto::Hash hash;
      std::memcpy(&hash, value.iov_base, sizeof(crypto::Hash));
      result.emplace_back(height, hash);
    }
  }

  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  return result;
}

void MDBXBlockchainStorage::migrateToPaddedKeys()
{
  // Converts all legacy "be_123" and "hdr_123" keys to "be_00000123" and "hdr_00000123"
  // Must be run once, with the node stopped, before any normal operations
  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("migrateToPaddedKeys: txn_begin failed: " + std::string(mdbx_strerror(rc)));

  // Migrate block entries
  {
    MDBX_cursor *cursor;
    rc = mdbx_cursor_open(txn, m_dbiBlockEntries, &cursor);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      throw std::runtime_error("migrate: cursor open block_entries failed");
    }

    // First pass: count entries so we can show progress
    MDBX_val key, value;
    size_t entryCount = 0;
    while (mdbx_cursor_get(cursor, &key, &value, MDBX_NEXT) == MDBX_SUCCESS)
      ++entryCount;

    // Reset cursor to beginning
    mdbx_cursor_close(cursor);
    rc = mdbx_cursor_open(txn, m_dbiBlockEntries, &cursor);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      throw std::runtime_error("migrate: cursor reopen block_entries failed");
    }

    // Collect old→new key pairs (can't modify while iterating)
    std::vector<std::pair<std::string, std::string>> migrations;
    size_t scanned = 0;
    while (mdbx_cursor_get(cursor, &key, &value, MDBX_NEXT) == MDBX_SUCCESS)
    {
      std::string oldKey(static_cast<const char *>(key.iov_base), key.iov_len);
      if (oldKey.rfind("be_", 0) == 0 && oldKey.size() > 3)
      {
        uint32_t height = static_cast<uint32_t>(std::stoul(oldKey.substr(3)));
        std::string newKey = blockEntryKey(height);
        if (oldKey != newKey)
          migrations.emplace_back(oldKey, newKey);
      }
      ++scanned;
      if (scanned % 100000 == 0)
        std::cout << "  Scanning block entries: " << scanned << " / " << entryCount << std::endl;
    }

    // Apply migrations
    size_t migrated = 0;
    for (const auto &[oldKey, newKey] : migrations)
    {
      MDBX_val oldK = to_val(oldKey);
      MDBX_val v;
      if (mdbx_get(txn, m_dbiBlockEntries, &oldK, &v) == MDBX_SUCCESS)
      {
        MDBX_val newK = to_val(newKey);
        mdbx_put(txn, m_dbiBlockEntries, &newK, &v, MDBX_UPSERT);
        mdbx_del(txn, m_dbiBlockEntries, &oldK, nullptr);
      }
      ++migrated;
      if (migrated % 100000 == 0)
        std::cout << "  Migrating block entries: " << migrated << " / " << migrations.size() << std::endl;
    }
    mdbx_cursor_close(cursor);
    std::cout << "  Block entries migrated: " << migrated << std::endl;
  }

  // Migrate block headers
  {
    MDBX_cursor *cursor;
    rc = mdbx_cursor_open(txn, m_dbiBlockHeaders, &cursor);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      throw std::runtime_error("migrate: cursor open block_headers failed");
    }

    // Count headers
    MDBX_val key, value;
    size_t headerCount = 0;
    while (mdbx_cursor_get(cursor, &key, &value, MDBX_NEXT) == MDBX_SUCCESS)
      ++headerCount;

    // Reset cursor
    mdbx_cursor_close(cursor);
    rc = mdbx_cursor_open(txn, m_dbiBlockHeaders, &cursor);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      throw std::runtime_error("migrate: cursor reopen block_headers failed");
    }

    std::vector<std::pair<std::string, std::string>> migrations;
    size_t scanned = 0;
    while (mdbx_cursor_get(cursor, &key, &value, MDBX_NEXT) == MDBX_SUCCESS)
    {
      std::string oldKey(static_cast<const char *>(key.iov_base), key.iov_len);
      if (oldKey.rfind("hdr_", 0) == 0 && oldKey.size() > 4)
      {
        uint32_t height = static_cast<uint32_t>(std::stoul(oldKey.substr(4)));
        std::string newKey = blockHeaderKey(height);
        if (oldKey != newKey)
          migrations.emplace_back(oldKey, newKey);
      }
      ++scanned;
      if (scanned % 100000 == 0)
        std::cout << "  Scanning headers: " << scanned << " / " << headerCount << std::endl;
    }

    size_t migrated = 0;
    for (const auto &[oldKey, newKey] : migrations)
    {
      MDBX_val oldK = to_val(oldKey);
      MDBX_val v;
      if (mdbx_get(txn, m_dbiBlockHeaders, &oldK, &v) == MDBX_SUCCESS)
      {
        MDBX_val newK = to_val(newKey);
        mdbx_put(txn, m_dbiBlockHeaders, &newK, &v, MDBX_UPSERT);
        mdbx_del(txn, m_dbiBlockHeaders, &oldK, nullptr);
      }
      ++migrated;
      if (migrated % 100000 == 0)
        std::cout << "  Migrating headers: " << migrated << " / " << migrations.size() << std::endl;
    }
    mdbx_cursor_close(cursor);
    std::cout << "  Headers migrated: " << migrated << std::endl;
  }

  // Commit all migrations in one durable transaction
  std::cout << "  Committing key migration to disk..." << std::endl;
  rc = mdbx_txn_commit(txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("migrateToPaddedKeys: commit failed: " + std::string(mdbx_strerror(rc)));
  std::cout << "  Key migration committed." << std::endl;
}

// ─────────────────────────────────────────────────────────────────────────────
// Key Format Helpers (new)
// ─────────────────────────────────────────────────────────────────────────────

std::string MDBXBlockchainStorage::timestampKey(uint64_t timestamp, uint32_t height)
{
  // Format: "ts_0000000000000012345678_00000042"
  // Lexicographic order ensures timestamps sort correctly, with height as tiebreaker
  std::ostringstream oss;
  oss << "ts_"
      << std::setw(kTimestampKeyWidth) << std::setfill('0') << timestamp
      << "_"
      << std::setw(kHeightKeyWidth) << std::setfill('0') << height;
  return oss.str();
}

std::string MDBXBlockchainStorage::difficultyKey(uint32_t height)
{
  // Format: "diff_00000042"
  std::ostringstream oss;
  oss << "diff_" << std::setw(kDifficultyKeyWidth) << std::setfill('0') << height;
  return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// Transaction Storage
// ─────────────────────────────────────────────────────────────────────────────

void MDBXBlockchainStorage::pushTransaction(const crypto::Hash &txHash, const cn::BinaryArray &serializedTx)
{
  // Stores a single transaction by its hash
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  MDBX_val key = to_val(txHash);
  MDBX_val value = to_val(serializedTx.data(), serializedTx.size());

  int rc = mdbx_put(m_writeTxn, m_dbiTransactions, &key, &value, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("pushTransaction failed: " + std::string(mdbx_strerror(rc)));
  }

  ++m_opsSinceLastCommit;
  size_t batchSize = m_bulkSyncMode ? kCommitBatchSizeBulk : kCommitBatchSize;
  if (m_opsSinceLastCommit >= batchSize)
    commitWriteTransaction(false);
}

bool MDBXBlockchainStorage::getTransaction(const crypto::Hash &txHash, cn::Transaction &tx) const
{
  // Reads and deserializes a transaction by hash
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);

  MDBX_val key = to_val(txHash);
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiTransactions, &key, &value);

  if (rc == MDBX_SUCCESS)
  {
    cn::BinaryArray ba(
        static_cast<const uint8_t *>(value.iov_base),
        static_cast<const uint8_t *>(value.iov_base) + value.iov_len);
    mdbx_txn_abort(txn);

    cn::Transaction tempTx;
    bool ok = cn::fromBinaryArray(tempTx, ba);
    if (ok)
      tx = std::move(tempTx);
    return ok;
  }

  mdbx_txn_abort(txn);
  return false;
}

bool MDBXBlockchainStorage::transactionExists(const crypto::Hash &txHash) const
{
  // Quick existence check — single read txn, no deserialization
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);

  MDBX_val key = to_val(txHash);
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiTransactions, &key, &value);

  mdbx_txn_abort(txn);
  return rc == MDBX_SUCCESS;
}

void MDBXBlockchainStorage::removeTransaction(const crypto::Hash &txHash)
{
  // Removes a transaction (used during reorgs when a block is popped)
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  MDBX_val key = to_val(txHash);
  mdbx_del(m_writeTxn, m_dbiTransactions, &key, nullptr);

  ++m_opsSinceLastCommit;
}

// ─────────────────────────────────────────────────────────────────────────────
// Transaction Pool Persistence
// ─────────────────────────────────────────────────────────────────────────────

void MDBXBlockchainStorage::storePoolState(
    const std::vector<cn::BinaryArray> &serializedTxs,
    const std::vector<crypto::KeyImage> &spentKeyImages)
{
  // Atomically replaces the entire pool state.
  // Uses meta keys "pool_tx_count" / "pool_tx_N" / "pool_ki_count" / "pool_ki_N"
  // All writes happen in one transaction — no partial state.

  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  // 1. Clear old pool data by scanning and deleting keys with the pool prefix.
  // We use a small cursor scan for the meta database.
  MDBX_cursor *cursor;
  int rc = mdbx_cursor_open(m_writeTxn, m_dbiMeta, &cursor);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("storePoolState: cursor open failed: " + std::string(mdbx_strerror(rc)));
  }

  // Collect keys to delete (can't modify while iterating with some cursor ops)
  std::vector<std::string> keysToDelete;
  MDBX_val ckey, cval;
  while (mdbx_cursor_get(cursor, &ckey, &cval, MDBX_NEXT) == MDBX_SUCCESS)
  {
    std::string keyStr(static_cast<const char *>(ckey.iov_base), ckey.iov_len);
    if (keyStr.rfind("pool_", 0) == 0)
      keysToDelete.push_back(keyStr);
  }
  mdbx_cursor_close(cursor);

  for (const auto &keyStr : keysToDelete)
  {
    MDBX_val mk = to_val(keyStr);
    mdbx_del(m_writeTxn, m_dbiMeta, &mk, nullptr);
  }

  // 2. Write transaction count
  uint32_t txCount = static_cast<uint32_t>(serializedTxs.size());
  MDBX_val countKey = to_val(std::string("pool_tx_count"));
  MDBX_val countVal = to_val(&txCount, sizeof(txCount));
  mdbx_put(m_writeTxn, m_dbiMeta, &countKey, &countVal, MDBX_UPSERT);

  // 3. Write each transaction
  for (size_t i = 0; i < serializedTxs.size(); ++i)
  {
    std::string keyStr = "pool_tx_" + std::to_string(i);
    MDBX_val mk = to_val(keyStr);
    MDBX_val mv = to_val(serializedTxs[i].data(), serializedTxs[i].size());
    mdbx_put(m_writeTxn, m_dbiMeta, &mk, &mv, MDBX_UPSERT);
  }

  // 4. Write spent key image count
  uint32_t kiCount = static_cast<uint32_t>(spentKeyImages.size());
  MDBX_val kiCountKey = to_val(std::string("pool_ki_count"));
  MDBX_val kiCountVal = to_val(&kiCount, sizeof(kiCount));
  mdbx_put(m_writeTxn, m_dbiMeta, &kiCountKey, &kiCountVal, MDBX_UPSERT);

  // 5. Write each spent key image
  for (size_t i = 0; i < spentKeyImages.size(); ++i)
  {
    std::string keyStr = "pool_ki_" + std::to_string(i);
    MDBX_val mk = to_val(keyStr);
    MDBX_val mv = to_val(spentKeyImages[i].data, sizeof(spentKeyImages[i].data));
    mdbx_put(m_writeTxn, m_dbiMeta, &mk, &mv, MDBX_UPSERT);
  }

  commitWriteTransaction(true); // Force durable commit — pool state must survive crashes
}

std::vector<cn::BinaryArray> MDBXBlockchainStorage::loadPoolTransactions() const
{
  // Reads all persisted pool transactions
  std::vector<cn::BinaryArray> result;

  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);

  // Read count
  MDBX_val countKey = to_val(std::string("pool_tx_count"));
  MDBX_val countVal;
  if (mdbx_get(txn, m_dbiMeta, &countKey, &countVal) != MDBX_SUCCESS ||
      countVal.iov_len < sizeof(uint32_t))
  {
    mdbx_txn_abort(txn);
    return result; // No saved pool state
  }

  uint32_t txCount = *static_cast<const uint32_t *>(countVal.iov_base);
  result.reserve(txCount);

  for (uint32_t i = 0; i < txCount; ++i)
  {
    std::string keyStr = "pool_tx_" + std::to_string(i);
    MDBX_val mk = to_val(keyStr);
    MDBX_val mv;
    if (mdbx_get(txn, m_dbiMeta, &mk, &mv) == MDBX_SUCCESS)
    {
      cn::BinaryArray ba(
          static_cast<const uint8_t *>(mv.iov_base),
          static_cast<const uint8_t *>(mv.iov_base) + mv.iov_len);
      result.push_back(std::move(ba));
    }
  }

  mdbx_txn_abort(txn);
  return result;
}

std::vector<crypto::KeyImage> MDBXBlockchainStorage::loadPoolSpentKeyImages() const
{
  // Reads all persisted pool spent key images
  std::vector<crypto::KeyImage> result;

  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);

  MDBX_val countKey = to_val(std::string("pool_ki_count"));
  MDBX_val countVal;
  if (mdbx_get(txn, m_dbiMeta, &countKey, &countVal) != MDBX_SUCCESS ||
      countVal.iov_len < sizeof(uint32_t))
  {
    mdbx_txn_abort(txn);
    return result;
  }

  uint32_t kiCount = *static_cast<const uint32_t *>(countVal.iov_base);
  result.reserve(kiCount);

  for (uint32_t i = 0; i < kiCount; ++i)
  {
    std::string keyStr = "pool_ki_" + std::to_string(i);
    MDBX_val mk = to_val(keyStr);
    MDBX_val mv;
    if (mdbx_get(txn, m_dbiMeta, &mk, &mv) == MDBX_SUCCESS &&
        mv.iov_len == sizeof(crypto::KeyImage))
    {
      crypto::KeyImage ki;
      std::memcpy(ki.data, mv.iov_base, sizeof(ki.data));
      result.push_back(ki);
    }
  }

  mdbx_txn_abort(txn);
  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Spent Key Images (MDBX-native — no more stubs)
// ─────────────────────────────────────────────────────────────────────────────

bool MDBXBlockchainStorage::isSpentKeyImage(const crypto::KeyImage &keyImage) const
{
  // Single read txn: check presence in the spent_key_images database
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);

  MDBX_val key = to_val(keyImage.data, sizeof(keyImage.data));
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiSpentKeyImages, &key, &value);

  mdbx_txn_abort(txn);
  return rc == MDBX_SUCCESS; // Key exists → spent
}

void MDBXBlockchainStorage::markKeyImageSpent(const crypto::KeyImage &keyImage)
{
  // Inserts a key image into the spent database. Value is empty — presence is the flag.
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  MDBX_val key = to_val(keyImage.data, sizeof(keyImage.data));
  MDBX_val value = to_val(nullptr, 0); // Empty value

  int rc = mdbx_put(m_writeTxn, m_dbiSpentKeyImages, &key, &value, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("markKeyImageSpent failed: " + std::string(mdbx_strerror(rc)));
  }

  ++m_opsSinceLastCommit;
  size_t batchSize = m_bulkSyncMode ? kCommitBatchSizeBulk : kCommitBatchSize;
  if (m_opsSinceLastCommit >= batchSize)
    commitWriteTransaction(false);
}

void MDBXBlockchainStorage::markKeyImagesSpent(const std::vector<crypto::KeyImage> &keyImages)
{
  // Batch insert — all key images are written in the same transaction
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  MDBX_val emptyVal = to_val(nullptr, 0);

  for (const auto &ki : keyImages)
  {
    MDBX_val key = to_val(ki.data, sizeof(ki.data));
    int rc = mdbx_put(m_writeTxn, m_dbiSpentKeyImages, &key, &emptyVal, MDBX_UPSERT);
    if (rc != MDBX_SUCCESS)
    {
      abortWriteTxn();
      throw std::runtime_error("markKeyImagesSpent failed: " + std::string(mdbx_strerror(rc)));
    }
  }

  m_opsSinceLastCommit += keyImages.size();
  size_t batchSize = m_bulkSyncMode ? kCommitBatchSizeBulk : kCommitBatchSize;
  if (m_opsSinceLastCommit >= batchSize)
    commitWriteTransaction(false);
}

bool MDBXBlockchainStorage::areAllKeyImagesUnspent(const std::vector<crypto::KeyImage> &keyImages) const
{
  // Check all key images in a single read transaction
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);

  for (const auto &ki : keyImages)
  {
    MDBX_val key = to_val(ki.data, sizeof(ki.data));
    MDBX_val value;
    if (mdbx_get(txn, m_dbiSpentKeyImages, &key, &value) == MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      return false; // Found a spent key image
    }
  }

  mdbx_txn_abort(txn);
  return true; // All unspent
}

// ─────────────────────────────────────────────────────────────────────────────
// Timestamp Index
// ─────────────────────────────────────────────────────────────────────────────

void MDBXBlockchainStorage::addTimestampIndex(uint64_t timestamp, uint32_t height)
{
  // Stores a timestamp → height mapping with padded key for ordered scans
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  std::string keyStr = timestampKey(timestamp, height);
  MDBX_val key = to_val(keyStr);
  MDBX_val value = to_val(&height, sizeof(height));

  int rc = mdbx_put(m_writeTxn, m_dbiTimestampIndex, &key, &value, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("addTimestampIndex failed: " + std::string(mdbx_strerror(rc)));
  }

  ++m_opsSinceLastCommit;
  size_t batchSize = m_bulkSyncMode ? kCommitBatchSizeBulk : kCommitBatchSize;
  if (m_opsSinceLastCommit >= batchSize)
    commitWriteTransaction(false);
}

uint32_t MDBXBlockchainStorage::getBlockHeightByTimestamp(uint64_t timestamp) const
{
  // Finds the block height closest to (but not exceeding) the given timestamp.
  // Uses cursor positioning to find the nearest entry.
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);

  MDBX_cursor *cursor;
  int rc = mdbx_cursor_open(txn, m_dbiTimestampIndex, &cursor);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    return 0;
  }

  // Seek to the first key >= our target prefix
  std::string seekKey = timestampKey(timestamp, 0);
  MDBX_val key = to_val(seekKey);
  MDBX_val value;
  rc = mdbx_cursor_get(cursor, &key, &value, MDBX_SET_RANGE);

  uint32_t result = 0;
  if (rc == MDBX_SUCCESS && value.iov_len >= sizeof(uint32_t))
  {
    // Found an exact or higher timestamp.
    // If the found key's timestamp is <= our target, use it.
    // Otherwise, step back one entry to get the nearest lower block.
    std::string foundKey(static_cast<const char *>(key.iov_base), key.iov_len);
    uint64_t foundTs = std::stoull(foundKey.substr(3, kTimestampKeyWidth));

    if (foundTs <= timestamp)
    {
      result = *static_cast<const uint32_t *>(value.iov_base);
    }
    else
    {
      // Step back to previous entry
      rc = mdbx_cursor_get(cursor, &key, &value, MDBX_PREV);
      if (rc == MDBX_SUCCESS && value.iov_len >= sizeof(uint32_t))
      {
        std::string prevKey(static_cast<const char *>(key.iov_base), key.iov_len);
        if (prevKey.rfind("ts_", 0) == 0)
          result = *static_cast<const uint32_t *>(value.iov_base);
      }
    }
  }
  else
  {
    // No entry >= target — go to the last entry in the database
    rc = mdbx_cursor_get(cursor, &key, &value, MDBX_LAST);
    if (rc == MDBX_SUCCESS && value.iov_len >= sizeof(uint32_t))
    {
      result = *static_cast<const uint32_t *>(value.iov_base);
    }
  }

  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  return result;
}

void MDBXBlockchainStorage::removeTimestampIndex(uint64_t timestamp, uint32_t height)
{
  // Removes a specific timestamp → height entry (used during reorg cleanup)
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  std::string keyStr = timestampKey(timestamp, height);
  MDBX_val key = to_val(keyStr);
  mdbx_del(m_writeTxn, m_dbiTimestampIndex, &key, nullptr);

  ++m_opsSinceLastCommit;
}

// ─────────────────────────────────────────────────────────────────────────────
// Cumulative Difficulty
// ─────────────────────────────────────────────────────────────────────────────

void MDBXBlockchainStorage::setCumulativeDifficulty(uint32_t height, uint64_t cumulativeDifficulty)
{
  // Stores cumulative difficulty for a given height
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  std::string keyStr = difficultyKey(height);
  MDBX_val key = to_val(keyStr);
  MDBX_val value = to_val(&cumulativeDifficulty, sizeof(cumulativeDifficulty));

  int rc = mdbx_put(m_writeTxn, m_dbiDifficulty, &key, &value, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("setCumulativeDifficulty failed: " + std::string(mdbx_strerror(rc)));
  }

  ++m_opsSinceLastCommit;
  size_t batchSize = m_bulkSyncMode ? kCommitBatchSizeBulk : kCommitBatchSize;
  if (m_opsSinceLastCommit >= batchSize)
    commitWriteTransaction(false);
}

uint64_t MDBXBlockchainStorage::getCumulativeDifficulty(uint32_t height) const
{
  // Reads cumulative difficulty for a given height
  MDBX_txn *txn;
  mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);

  std::string keyStr = difficultyKey(height);
  MDBX_val key = to_val(keyStr);
  MDBX_val value;
  int rc = mdbx_get(txn, m_dbiDifficulty, &key, &value);

  uint64_t result = 0;
  if (rc == MDBX_SUCCESS && value.iov_len >= sizeof(uint64_t))
    result = *static_cast<const uint64_t *>(value.iov_base);

  mdbx_txn_abort(txn);
  return result;
}

void MDBXBlockchainStorage::removeCumulativeDifficulty(uint32_t height)
{
  // Removes cumulative difficulty entry (used during reorg cleanup)
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  std::string keyStr = difficultyKey(height);
  MDBX_val key = to_val(keyStr);
  mdbx_del(m_writeTxn, m_dbiDifficulty, &key, nullptr);

  ++m_opsSinceLastCommit;
}

uint64_t MDBXBlockchainStorage::getCurrentCumulativeDifficulty() const
{
  // Returns the cumulative difficulty at the current chain tip
  uint32_t height = topBlockHeight();
  if (height == 0)
    return 0;
  return getCumulativeDifficulty(height);
}

// ─────────────────────────────────────────────────────────────────────────────
// Remove Meta Key (new utility)
// ─────────────────────────────────────────────────────────────────────────────

void MDBXBlockchainStorage::removeMeta(const std::string &key)
{
  // Deletes a key from the meta database
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  MDBX_val mk = to_val(key);
  mdbx_del(m_writeTxn, m_dbiMeta, &mk, nullptr);

  ++m_opsSinceLastCommit;
}

// ─────────────────────────────────────────────────────────────────────────────
// Remove Checkpoints Above Height (reorg support)
// ─────────────────────────────────────────────────────────────────────────────

void MDBXBlockchainStorage::removeCheckpointsAbove(uint32_t height)
{
  // Removes all checkpoints with height > the given height (reorg cleanup)
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  MDBX_cursor *cursor;
  int rc = mdbx_cursor_open(m_writeTxn, m_dbiCheckpoints, &cursor);
  if (rc != MDBX_SUCCESS)
  {
    abortWriteTxn();
    throw std::runtime_error("removeCheckpointsAbove: cursor open failed: " + std::string(mdbx_strerror(rc)));
  }

  // Seek to the first checkpoint key > our target
  std::string seekKey = checkpointKey(height + 1);
  MDBX_val key = to_val(seekKey);
  MDBX_val value;
  rc = mdbx_cursor_get(cursor, &key, &value, MDBX_SET_RANGE);

  // Delete all subsequent entries
  while (rc == MDBX_SUCCESS)
  {
    std::string keyStr(static_cast<const char *>(key.iov_base), key.iov_len);
    if (keyStr.rfind("checkpoint_", 0) != 0)
      break; // Reached end of checkpoint keys

    mdbx_cursor_del(cursor, MDBX_put_flags_t::MDBX_CURRENT); // ← Fixed: use MDBX_CURRENT flag
    rc = mdbx_cursor_get(cursor, &key, &value, MDBX_NEXT);
    ++m_opsSinceLastCommit;
  }

  mdbx_cursor_close(cursor);
  commitWriteTransaction(true); // Force durable commit for reorgs
}

// ─────────────────────────────────────────────────────────────────────────────
// Updated openDatabases (replaces existing method)
// ─────────────────────────────────────────────────────────────────────────────

void MDBXBlockchainStorage::openDatabases(MDBX_txn *txn)
{
  // Open (or create) each named database — dbi handles are stored for later use
  mdbx_dbi_open(txn, "heights", MDBX_CREATE, &m_dbiHeights);
  mdbx_dbi_open(txn, "block_heights", MDBX_CREATE, &m_dbiBlockHeights);
  mdbx_dbi_open(txn, "meta", MDBX_CREATE, &m_dbiMeta);
  mdbx_dbi_open(txn, "block_entries", MDBX_CREATE, &m_dbiBlockEntries);
  mdbx_dbi_open(txn, "block_headers", MDBX_CREATE, &m_dbiBlockHeaders);
  mdbx_dbi_open(txn, "checkpoints", MDBX_CREATE, &m_dbiCheckpoints);
  mdbx_dbi_open(txn, "transactions", MDBX_CREATE, &m_dbiTransactions);
  mdbx_dbi_open(txn, "spent_key_images", MDBX_CREATE, &m_dbiSpentKeyImages);
  mdbx_dbi_open(txn, "timestamp_index", MDBX_CREATE, &m_dbiTimestampIndex);
  mdbx_dbi_open(txn, "difficulty", MDBX_CREATE, &m_dbiDifficulty);
}

// ─────────────────────────────────────────────────────────────────────────────
// Updated printDatabaseStats (replaces existing method — adds new databases)
// ─────────────────────────────────────────────────────────────────────────────

std::string MDBXBlockchainStorage::printDatabaseStats() const
{
  // Returns a human-readable summary of all databases
  std::string result;

  MDBX_txn *txn;
  if (mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS)
    return "error: failed to begin transaction";

  MDBX_dbi dbis[] = {m_dbiHeights, m_dbiBlockHeights,
                     m_dbiMeta, m_dbiBlockEntries, m_dbiBlockHeaders, m_dbiCheckpoints,
                     m_dbiTransactions, m_dbiSpentKeyImages, m_dbiTimestampIndex, m_dbiDifficulty};
  const char *names[] = {"heights", "block_heights",
                         "meta", "block_entries", "block_headers", "checkpoints",
                         "transactions", "spent_key_images", "timestamp_index", "difficulty"};

  for (int i = 0; i < 10; i++)
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