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

// ---------- Safe MDBX_val constructors (class static members) ----------

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

// ---------- Zero‑padded key helpers (lexicographic order == numeric order → MDBX_APPEND) ----------

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
  oss << "checkpoint_" << std::setw(7) << std::setfill('0') << height;
  return oss.str();
}

// ---------- Constructor / destructor ----------

MDBXBlockchainStorage::MDBXBlockchainStorage(const std::string &dataDir, bool bulkSyncMode, uint64_t sizeLimitBytes)
    : m_dataDir(dataDir), m_sizeLimitBytes(sizeLimitBytes)
{
  m_bulkSyncMode = bulkSyncMode;
  openEnvironment(dataDir); // Creates env, opens all databases, and commits the initialisation transaction
}

MDBXBlockchainStorage::~MDBXBlockchainStorage()
{
  close(); // Aborts any pending write txn and closes the environment
}

// ---------- Environment setup ----------

void MDBXBlockchainStorage::openEnvironment(const std::string &path)
{
  // Create the MDBX environment handle
  int rc = mdbx_env_create(&m_env);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("mdbx_env_create failed: " + std::string(mdbx_strerror(rc)));

  // RAII guard: closes environment if we throw before releasing it
  auto envGuard = std::unique_ptr<MDBX_env, decltype(&mdbx_env_close)>(m_env, mdbx_env_close);

  // Allow up to 8 named databases (we currently use 6)
  mdbx_env_set_maxdbs(m_env, 8);

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

void MDBXBlockchainStorage::openDatabases(MDBX_txn *txn)
{
  // Open (or create) each named database — dbi handles are stored for later use
  mdbx_dbi_open(txn, "heights", MDBX_CREATE, &m_dbiHeights);            // height (uint32_t) → block hash
  mdbx_dbi_open(txn, "block_heights", MDBX_CREATE, &m_dbiBlockHeights); // block hash → height
  mdbx_dbi_open(txn, "meta", MDBX_CREATE, &m_dbiMeta);                  // string key → arbitrary blob
  mdbx_dbi_open(txn, "block_entries", MDBX_CREATE, &m_dbiBlockEntries); // "be_<height>" → serialised block
  mdbx_dbi_open(txn, "block_headers", MDBX_CREATE, &m_dbiBlockHeaders); // "hdr_<height>" → BlockHeaderPOD
  mdbx_dbi_open(txn, "checkpoints", MDBX_CREATE, &m_dbiCheckpoints);    // "checkpoint_<height>" → hash
}

// ---------- Block existence and retrieval ----------

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

// ---------- Header retrieval (single and batch) ----------

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

// ---------- Spent key images (placeholder — in‑memory tracking lives in Blockchain) ----------

bool MDBXBlockchainStorage::isSpentKeyImage(const crypto::KeyImage &keyImage) const
{
  return false; // Currently handled in‑memory; will move to native DB in a future patch
}

void MDBXBlockchainStorage::markKeyImageSpent(const crypto::KeyImage &keyImage)
{
  // Handled in‑memory via Blockchain::m_spent_keys and persisted to meta blobs by storeCache
  // TODO: migrate to a dedicated "spent_keys" MDBX database for instant startup
}

// ---------- Atomic complete block write ----------

void MDBXBlockchainStorage::pushCompleteBlock(uint32_t height,
                                              const crypto::Hash &hash,
                                              const cn::BinaryArray &serializedEntry,
                                              const cn::BlockHeaderPOD &hdr)
{
  // All block data is written in ONE explicit transaction — no partial writes, no orphans
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  // 1. Bi‑directional height ↔ hash mapping
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
  putFlags = MDBX_UPSERT;
  if (height == m_cachedTopHeight + 1)
    putFlags = static_cast<MDBX_put_flags_t>(putFlags | MDBX_APPEND);
  rc = mdbx_put(m_writeTxn, m_dbiBlockHeaders, &hdrk, &hdrv, putFlags);
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

// ---------- Legacy single‑operation writes ----------

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

// ---------- Atomic block removal ----------

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

void MDBXBlockchainStorage::cleanupBlockData(uint32_t height)
{
  // Removes both block entry and header — used after a failed block processing
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  std::string beKey = blockEntryKey(height);
  MDBX_val mk = to_val(beKey);
  mdbx_del(m_writeTxn, m_dbiBlockEntries, &mk, nullptr);

  std::string hdrKey = blockHeaderKey(height);
  MDBX_val hk = to_val(hdrKey);
  mdbx_del(m_writeTxn, m_dbiBlockHeaders, &hk, nullptr);

  commitWriteTransaction(true); // Force durable commit — must not leave orphaned data
}

// ---------- Global state ----------

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

// ---------- Write transaction management ----------

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

// ---------- Block entry serialisation (legacy — prefer pushCompleteBlock) ----------

void MDBXBlockchainStorage::pushBlockEntry(uint32_t height, const cn::BinaryArray &serializedEntry)
{
  // Stores a serialised block entry — uses padded key now
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  std::string key = blockEntryKey(height);
  MDBX_val mkey = to_val(key);
  MDBX_val mval = to_val(serializedEntry.data(), serializedEntry.size());
  MDBX_put_flags_t putFlags = MDBX_UPSERT;
  if (height == m_cachedTopHeight + 1)
    putFlags = static_cast<MDBX_put_flags_t>(putFlags | MDBX_APPEND); // Sequential write optimisation
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

// ---------- Initialisation flag ----------

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

// ---------- Generic metadata store ----------

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

// ---------- Push block header (legacy) ----------

void MDBXBlockchainStorage::pushBlockHeader(uint32_t height, const cn::BlockHeaderPOD &hdr)
{
  // Stores a block header using padded key — prefer pushCompleteBlock() for new blocks
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

// ---------- Checkpoint storage ----------

void MDBXBlockchainStorage::storeCheckpoint(uint32_t height, const crypto::Hash &hash)
{
  // Stores a checkpoint with zero‑padded key for correct lexical ordering
  std::lock_guard<std::mutex> lock(m_txMutex);
  ensureWriteTxn();

  std::string key = checkpointKey(height);
  MDBX_val mkey = to_val(key);
  MDBX_val mval = to_val(hash);

  MDBX_put_flags_t putFlags = MDBX_UPSERT;
  // MDBX_APPEND can be used during initial sync when heights are monotonically increasing
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

// ---------- One‑time migration to padded keys ----------

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

// ---------- Diagnostics ----------

std::string MDBXBlockchainStorage::printDatabaseStats() const
{
  // Returns a human‑readable summary of all databases: name, entry count, data size in MB
  std::string result;

  MDBX_txn *txn;
  if (mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS)
    return "error: failed to begin transaction";

  MDBX_dbi dbis[] = {m_dbiHeights, m_dbiBlockHeights,
                     m_dbiMeta, m_dbiBlockEntries, m_dbiBlockHeaders, m_dbiCheckpoints};
  const char *names[] = {"heights", "block_heights",
                         "meta", "block_entries", "block_headers", "checkpoints"};

  for (int i = 0; i < 6; i++)
  {
    MDBX_stat stat;
    if (mdbx_dbi_stat(txn, dbis[i], &stat, sizeof(stat)) == MDBX_SUCCESS)
    {
      size_t dataMB = (stat.ms_leaf_pages * 4096) >> 20; // Approximate: leaf pages × page size
      char line[128];
      snprintf(line, sizeof(line), "  %-18s %8lu entries  %6zu MB\n",
               names[i], (unsigned long)stat.ms_entries, dataMB);
      result += line;
    }
  }

  mdbx_txn_abort(txn);
  return result;
}