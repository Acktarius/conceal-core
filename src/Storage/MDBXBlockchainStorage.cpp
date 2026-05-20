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

// MDBX_val helpers
MDBX_val MDBXBlockchainStorage::to_val(const void *data, size_t len)
{
  MDBX_val v;
  v.iov_base = const_cast<void *>(data);
  v.iov_len = len;
  return v;
}

MDBX_val MDBXBlockchainStorage::to_val(const std::string &s)
{
  return to_val(s.data(), s.size());
}

MDBX_val MDBXBlockchainStorage::to_val(const crypto::Hash &h)
{
  return to_val(h.data, sizeof(h));
}

MDBX_val MDBXBlockchainStorage::to_val(const cn::BlockHeaderPOD &hdr)
{
  return to_val(&hdr, sizeof(hdr));
}

// Key helpers
std::string MDBXBlockchainStorage::blockEntryKey(uint32_t height)
{
  std::ostringstream oss;
  oss << "be_" << std::setw(kHeightKeyWidth) << std::setfill('0') << height;
  return oss.str();
}

std::string MDBXBlockchainStorage::blockHeaderKey(uint32_t height)
{
  std::ostringstream oss;
  oss << "hdr_" << std::setw(kHeightKeyWidth) << std::setfill('0') << height;
  return oss.str();
}

std::string MDBXBlockchainStorage::makeOutputDetailsKey(uint32_t height, uint32_t tx_idx, uint16_t out_idx)
{
  std::ostringstream oss;
  oss << "od_"
      << std::setw(8) << std::setfill('0') << height << "_"
      << std::setw(6) << std::setfill('0') << tx_idx << "_"
      << std::setw(4) << std::setfill('0') << out_idx;
  return oss.str();
}

std::string MDBXBlockchainStorage::makeKeyImageOwnerKey(const crypto::KeyImage &ki)
{
  return "kio_" + common::podToHex(ki);
}

// Constructor / Destructor
MDBXBlockchainStorage::MDBXBlockchainStorage(const std::string &dataDir, bool enableWalletIndexes)
    : m_dataDir(dataDir), m_enableWalletIndexes(enableWalletIndexes)
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
    throw std::runtime_error("mdbx_env_create failed: " + std::string(mdbx_strerror(rc)));

  auto envGuard = std::unique_ptr<MDBX_env, decltype(&mdbx_env_close)>(m_env, mdbx_env_close);

  // 8 named databases max (4 core + 4 wallet indexes)
  mdbx_env_set_maxdbs(m_env, 8);

  // Sensible geometry: 256 MB initial, up to 128 GB growth
  mdbx_env_set_geometry(m_env, -1, -1, (intptr_t)1 << 37, 256 << 20, -1, -1);

  const MDBX_env_flags_t openFlags = MDBX_NOSUBDIR | MDBX_NORDAHEAD | MDBX_LIFORECLAIM;

  rc = mdbx_env_open(m_env, path.c_str(), openFlags, 0664);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("mdbx_env_open failed: " + std::string(mdbx_strerror(rc)));

  // Open a write transaction to create/verify all named databases
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
  mdbx_dbi_open(txn, "block_entries", MDBX_CREATE, &m_dbiBlockEntries);
  mdbx_dbi_open(txn, "block_headers", MDBX_CREATE, &m_dbiBlockHeaders);
  mdbx_dbi_open(txn, "heights", MDBX_CREATE, &m_dbiHeights);
  mdbx_dbi_open(txn, "pool_state", MDBX_CREATE, &m_dbiPoolState);

  if (m_enableWalletIndexes)
  {
    mdbx_dbi_open(txn, "tx_pubkey_outputs", MDBX_CREATE, &m_dbiTxPubKeyOutputs);
    mdbx_dbi_open(txn, "output_details", MDBX_CREATE, &m_dbiOutputDetails);
    mdbx_dbi_open(txn, "key_image_owner", MDBX_CREATE, &m_dbiKeyImageOwner);
    mdbx_dbi_open(txn, "tx_pubkey_seen", MDBX_CREATE, &m_dbiTxPubKeySeen);
  }
}

// Lifecycle
void MDBXBlockchainStorage::flush()
{
  // MDBX commits are immediate — flush is a no-op for data integrity.
  // We still sync the environment to disk if needed.
  if (m_env)
    mdbx_env_sync(m_env);
}

void MDBXBlockchainStorage::close()
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  if (m_env)
  {
    mdbx_env_close(m_env);
    m_env = nullptr;
  }
}

// Height tracking
uint32_t MDBXBlockchainStorage::topBlockHeight() const
{
  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return 0;

  MDBX_stat stat;
  rc = mdbx_dbi_stat(txn, m_dbiHeights, &stat, sizeof(stat));
  mdbx_txn_abort(txn);

  if (rc != MDBX_SUCCESS || stat.ms_entries == 0)
    return 0;

  // heights DB stores entries for heights 0..N, so count-1 = top height
  return static_cast<uint32_t>(stat.ms_entries - 1);
}

// Atomic block write — single transaction, immediate commit
void MDBXBlockchainStorage::pushCompleteBlock(uint32_t height,
                                              const crypto::Hash &hash,
                                              const cn::BinaryArray &serializedEntry,
                                              const cn::BlockHeaderPOD &hdr)
{
  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("pushCompleteBlock: txn_begin failed: " + std::string(mdbx_strerror(rc)));

  // 1. Height -> Hash (binary key for fast cursor scan at startup)
  MDBX_val hkey = to_val(&height, sizeof(height));
  MDBX_val hashVal = to_val(hash);
  rc = mdbx_put(txn, m_dbiHeights, &hkey, &hashVal, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("pushCompleteBlock: heights put failed: " + std::string(mdbx_strerror(rc)));
  }

  // 2. Block entry (full serialized block)
  std::string beKey = blockEntryKey(height);
  MDBX_val bkey = to_val(beKey);
  MDBX_val bval = to_val(serializedEntry.data(), serializedEntry.size());
  rc = mdbx_put(txn, m_dbiBlockEntries, &bkey, &bval, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("pushCompleteBlock: block_entries put failed: " + std::string(mdbx_strerror(rc)));
  }

  // 3. Block header
  std::string hdrKey = blockHeaderKey(height);
  MDBX_val hdrk = to_val(hdrKey);
  MDBX_val hdrv = to_val(hdr);
  rc = mdbx_put(txn, m_dbiBlockHeaders, &hdrk, &hdrv, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("pushCompleteBlock: block_headers put failed: " + std::string(mdbx_strerror(rc)));
  }

  // 4. Commit immediately — no batching, no visibility window
  rc = mdbx_txn_commit(txn);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("pushCompleteBlock: commit failed: " + std::string(mdbx_strerror(rc)));
  }
}

// Atomic block removal — single transaction, immediate commit
void MDBXBlockchainStorage::removeCompleteBlock(uint32_t height, const crypto::Hash &hash)
{
  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("removeCompleteBlock: txn_begin failed: " + std::string(mdbx_strerror(rc)));

  // 1. Remove height -> hash
  MDBX_val hkey = to_val(&height, sizeof(height));
  mdbx_del(txn, m_dbiHeights, &hkey, nullptr);

  // 2. Remove block entry
  std::string beKey = blockEntryKey(height);
  MDBX_val bkey = to_val(beKey);
  mdbx_del(txn, m_dbiBlockEntries, &bkey, nullptr);

  // 3. Remove block header
  std::string hdrKey = blockHeaderKey(height);
  MDBX_val hdrk = to_val(hdrKey);
  mdbx_del(txn, m_dbiBlockHeaders, &hdrk, nullptr);

  rc = mdbx_txn_commit(txn);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("removeCompleteBlock: commit failed: " + std::string(mdbx_strerror(rc)));
  }
}

// Block reads
bool MDBXBlockchainStorage::getBlockEntry(uint32_t height, cn::BinaryArray &serializedEntry) const
{
  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return false;

  std::string key = blockEntryKey(height);
  MDBX_val mkey = to_val(key);
  MDBX_val mval;
  rc = mdbx_get(txn, m_dbiBlockEntries, &mkey, &mval);

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

bool MDBXBlockchainStorage::getBlockHeader(uint32_t height, cn::BlockHeaderPOD &hdr) const
{
  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return false;

  std::string key = blockHeaderKey(height);
  MDBX_val mkey = to_val(key);
  MDBX_val mval;
  rc = mdbx_get(txn, m_dbiBlockHeaders, &mkey, &mval);

  if (rc == MDBX_SUCCESS && mval.iov_len == sizeof(cn::BlockHeaderPOD))
  {
    memcpy(&hdr, mval.iov_base, sizeof(hdr));
    mdbx_txn_abort(txn);
    return true;
  }

  mdbx_txn_abort(txn);
  return false;
}

void MDBXBlockchainStorage::getBlockHeadersRange(uint32_t startHeight, uint32_t count,
                                                 std::vector<cn::BlockHeaderPOD> &out) const
{
  out.clear();
  out.reserve(count);

  std::lock_guard<std::mutex> lock(m_txMutex);

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

  std::string startKey = blockHeaderKey(startHeight);
  MDBX_val mkey = to_val(startKey);
  MDBX_val mval;
  rc = mdbx_cursor_get(cursor, &mkey, &mval, MDBX_SET_RANGE);

  while (rc == MDBX_SUCCESS && out.size() < count)
  {
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

// Transaction pool persistence
void MDBXBlockchainStorage::storePoolState(
    const std::vector<cn::BinaryArray> &serializedTxs,
    const std::vector<crypto::KeyImage> &spentKeyImages)
{
  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("storePoolState: txn_begin failed: " + std::string(mdbx_strerror(rc)));

  // 1. Clear old pool data
  MDBX_cursor *cursor;
  rc = mdbx_cursor_open(txn, m_dbiPoolState, &cursor);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("storePoolState: cursor open failed: " + std::string(mdbx_strerror(rc)));
  }

  std::vector<std::string> keysToDelete;
  MDBX_val ckey, cval;
  while (mdbx_cursor_get(cursor, &ckey, &cval, MDBX_NEXT) == MDBX_SUCCESS)
  {
    std::string keyStr(static_cast<const char *>(ckey.iov_base), ckey.iov_len);
    keysToDelete.push_back(keyStr);
  }
  mdbx_cursor_close(cursor);

  for (const auto &keyStr : keysToDelete)
  {
    MDBX_val mk = to_val(keyStr);
    mdbx_del(txn, m_dbiPoolState, &mk, nullptr);
  }

  // 2. Write transaction count
  uint32_t txCount = static_cast<uint32_t>(serializedTxs.size());
  MDBX_val countKey = to_val(std::string("pool_tx_count"));
  MDBX_val countVal = to_val(&txCount, sizeof(txCount));
  mdbx_put(txn, m_dbiPoolState, &countKey, &countVal, MDBX_UPSERT);

  // 3. Write each transaction
  for (size_t i = 0; i < serializedTxs.size(); ++i)
  {
    std::string keyStr = "pool_tx_" + std::to_string(i);
    MDBX_val mk = to_val(keyStr);
    MDBX_val mv = to_val(serializedTxs[i].data(), serializedTxs[i].size());
    mdbx_put(txn, m_dbiPoolState, &mk, &mv, MDBX_UPSERT);
  }

  // 4. Write spent key image count
  uint32_t kiCount = static_cast<uint32_t>(spentKeyImages.size());
  MDBX_val kiCountKey = to_val(std::string("pool_ki_count"));
  MDBX_val kiCountVal = to_val(&kiCount, sizeof(kiCount));
  mdbx_put(txn, m_dbiPoolState, &kiCountKey, &kiCountVal, MDBX_UPSERT);

  // 5. Write each spent key image
  for (size_t i = 0; i < spentKeyImages.size(); ++i)
  {
    std::string keyStr = "pool_ki_" + std::to_string(i);
    MDBX_val mk = to_val(keyStr);
    MDBX_val mv = to_val(spentKeyImages[i].data, sizeof(spentKeyImages[i].data));
    mdbx_put(txn, m_dbiPoolState, &mk, &mv, MDBX_UPSERT);
  }

  rc = mdbx_txn_commit(txn);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("storePoolState: commit failed: " + std::string(mdbx_strerror(rc)));
  }
}

std::vector<cn::BinaryArray> MDBXBlockchainStorage::loadPoolTransactions() const
{
  std::vector<cn::BinaryArray> result;

  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return result;

  MDBX_val countKey = to_val(std::string("pool_tx_count"));
  MDBX_val countVal;
  if (mdbx_get(txn, m_dbiPoolState, &countKey, &countVal) != MDBX_SUCCESS ||
      countVal.iov_len < sizeof(uint32_t))
  {
    mdbx_txn_abort(txn);
    return result;
  }

  uint32_t txCount = *static_cast<const uint32_t *>(countVal.iov_base);
  result.reserve(txCount);

  for (uint32_t i = 0; i < txCount; ++i)
  {
    std::string keyStr = "pool_tx_" + std::to_string(i);
    MDBX_val mk = to_val(keyStr);
    MDBX_val mv;
    if (mdbx_get(txn, m_dbiPoolState, &mk, &mv) == MDBX_SUCCESS)
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
  std::vector<crypto::KeyImage> result;

  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return result;

  MDBX_val countKey = to_val(std::string("pool_ki_count"));
  MDBX_val countVal;
  if (mdbx_get(txn, m_dbiPoolState, &countKey, &countVal) != MDBX_SUCCESS ||
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
    if (mdbx_get(txn, m_dbiPoolState, &mk, &mv) == MDBX_SUCCESS &&
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

// Wallet instant import — indexing (called during block processing)
void MDBXBlockchainStorage::indexOutputByTxPubKey(const crypto::PublicKey &tx_pub_key,
                                                  uint32_t height,
                                                  uint32_t tx_index,
                                                  uint16_t output_index,
                                                  const crypto::Hash &tx_hash,
                                                  uint64_t amount,
                                                  const crypto::PublicKey &output_key)
{
  if (!m_enableWalletIndexes)
    return;

  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("indexOutputByTxPubKey: txn_begin failed: " + std::string(mdbx_strerror(rc)));

  std::string pk_hex = common::podToHex(tx_pub_key);

  // 1. Append OutputRef to the tx_pub_key's list
  std::string pk_key_str = "txpk_" + pk_hex;
  MDBX_val pk_key = to_val(pk_key_str);
  MDBX_val pk_val;

  std::vector<OutputRef> refs;
  if (mdbx_get(txn, m_dbiTxPubKeyOutputs, &pk_key, &pk_val) == MDBX_SUCCESS)
  {
    refs.resize(pk_val.iov_len / sizeof(OutputRef));
    memcpy(refs.data(), pk_val.iov_base, pk_val.iov_len);
  }

  OutputRef ref(height, tx_index, output_index);
  refs.push_back(ref);

  MDBX_val new_pk_val = to_val(refs.data(), refs.size() * sizeof(OutputRef));
  rc = mdbx_put(txn, m_dbiTxPubKeyOutputs, &pk_key, &new_pk_val, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("indexOutputByTxPubKey: failed to store ref list: " + std::string(mdbx_strerror(rc)));
  }

  // 2. Store full output details
  std::string details_key = makeOutputDetailsKey(height, tx_index, output_index);
  MDBX_val dkey = to_val(details_key);

  WalletOutputInfo info;
  info.block_height = height;
  info.tx_hash = tx_hash;
  info.amount = amount;
  info.output_index = output_index;
  info.output_key = output_key;
  info.tx_public_key = tx_pub_key;

  cn::BinaryArray ba = cn::toBinaryArray(info);
  MDBX_val dval = to_val(ba.data(), ba.size());
  rc = mdbx_put(txn, m_dbiOutputDetails, &dkey, &dval, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("indexOutputByTxPubKey: failed to store details: " + std::string(mdbx_strerror(rc)));
  }

  // 3. Update tx_pub_key seen range
  std::string seen_key_str = "txpkseen_" + pk_hex;
  MDBX_val seen_key = to_val(seen_key_str);
  MDBX_val seen_val;

  TxPubKeySeen seen = {height, height};
  if (mdbx_get(txn, m_dbiTxPubKeySeen, &seen_key, &seen_val) == MDBX_SUCCESS &&
      seen_val.iov_len == sizeof(TxPubKeySeen))
  {
    memcpy(&seen, seen_val.iov_base, sizeof(seen));
    if (height < seen.first_seen)
      seen.first_seen = height;
    if (height > seen.last_seen)
      seen.last_seen = height;
  }

  MDBX_val new_seen_val = to_val(&seen, sizeof(seen));
  rc = mdbx_put(txn, m_dbiTxPubKeySeen, &seen_key, &new_seen_val, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("indexOutputByTxPubKey: failed to store seen range: " + std::string(mdbx_strerror(rc)));
  }

  rc = mdbx_txn_commit(txn);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("indexOutputByTxPubKey: commit failed: " + std::string(mdbx_strerror(rc)));
  }
}

void MDBXBlockchainStorage::indexSpentKeyImage(const crypto::KeyImage &key_image,
                                               const crypto::PublicKey &tx_pub_key,
                                               uint32_t spent_height)
{
  if (!m_enableWalletIndexes)
    return;

  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("indexSpentKeyImage: txn_begin failed: " + std::string(mdbx_strerror(rc)));

  std::string ki_key_str = makeKeyImageOwnerKey(key_image);
  MDBX_val ki_key = to_val(ki_key_str);

  KeyImageOwner owner;
  owner.tx_pub_key = tx_pub_key;
  owner.spent_height = spent_height;

  cn::BinaryArray ba = cn::toBinaryArray(owner);
  MDBX_val ki_val = to_val(ba.data(), ba.size());

  rc = mdbx_put(txn, m_dbiKeyImageOwner, &ki_key, &ki_val, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("indexSpentKeyImage: put failed: " + std::string(mdbx_strerror(rc)));
  }

  rc = mdbx_txn_commit(txn);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("indexSpentKeyImage: commit failed: " + std::string(mdbx_strerror(rc)));
  }
}

// Wallet instant import — querying (called from RPC)
bool MDBXBlockchainStorage::getOutputsByTxPubKeys(
    const std::vector<crypto::PublicKey> &tx_pub_keys,
    std::vector<WalletOutputInfo> &outputs,
    std::unordered_set<std::string> &spent_key_images) const
{
  if (!m_enableWalletIndexes)
    return false;

  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return false;

  // First pass: collect all OutputRefs for the requested tx_pub_keys
  std::vector<OutputRef> all_refs;
  for (const auto &pk : tx_pub_keys)
  {
    std::string pk_key_str = "txpk_" + common::podToHex(pk);
    MDBX_val pk_key = to_val(pk_key_str);
    MDBX_val pk_val;

    if (mdbx_get(txn, m_dbiTxPubKeyOutputs, &pk_key, &pk_val) == MDBX_SUCCESS)
    {
      size_t count = pk_val.iov_len / sizeof(OutputRef);
      const OutputRef *refs_ptr = static_cast<const OutputRef *>(pk_val.iov_base);
      all_refs.insert(all_refs.end(), refs_ptr, refs_ptr + count);
    }
  }

  // Second pass: fetch full details for each OutputRef
  outputs.reserve(all_refs.size());
  for (const auto &ref : all_refs)
  {
    std::string details_key = makeOutputDetailsKey(ref.block_height, ref.tx_index, ref.output_index);
    MDBX_val dkey = to_val(details_key);
    MDBX_val dval;

    if (mdbx_get(txn, m_dbiOutputDetails, &dkey, &dval) == MDBX_SUCCESS)
    {
      cn::BinaryArray ba(static_cast<const uint8_t *>(dval.iov_base),
                         static_cast<const uint8_t *>(dval.iov_base) + dval.iov_len);
      WalletOutputInfo info;
      if (cn::fromBinaryArray(info, ba))
      {
        outputs.push_back(std::move(info));
      }
    }
  }

  // Third pass: collect spent key images associated with the requested tx_pub_keys
  for (const auto &pk : tx_pub_keys)
  {
    MDBX_cursor *cursor;
    if (mdbx_cursor_open(txn, m_dbiKeyImageOwner, &cursor) != MDBX_SUCCESS)
      continue;

    MDBX_val ckey, cval;
    int crc = mdbx_cursor_get(cursor, &ckey, &cval, MDBX_FIRST);
    while (crc == MDBX_SUCCESS)
    {
      if (cval.iov_len > 0)
      {
        cn::BinaryArray ba(static_cast<const uint8_t *>(cval.iov_base),
                           static_cast<const uint8_t *>(cval.iov_base) + cval.iov_len);
        KeyImageOwner owner;
        if (cn::fromBinaryArray(owner, ba) && owner.tx_pub_key == pk)
        {
          std::string key_str(static_cast<const char *>(ckey.iov_base), ckey.iov_len);
          if (key_str.rfind("kio_", 0) == 0)
          {
            spent_key_images.insert(key_str.substr(4));
          }
        }
      }
      crc = mdbx_cursor_get(cursor, &ckey, &cval, MDBX_NEXT);
    }
    mdbx_cursor_close(cursor);
  }

  mdbx_txn_abort(txn);
  return true;
}

bool MDBXBlockchainStorage::isSpentKeyImage(const crypto::KeyImage &keyImage) const
{
  if (!m_enableWalletIndexes)
    return false;

  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return false;

  std::string ki_key_str = makeKeyImageOwnerKey(keyImage);
  MDBX_val ki_key = to_val(ki_key_str);
  MDBX_val ki_val;
  rc = mdbx_get(txn, m_dbiKeyImageOwner, &ki_key, &ki_val);

  mdbx_txn_abort(txn);
  return rc == MDBX_SUCCESS;
}

std::vector<crypto::PublicKey> MDBXBlockchainStorage::getNewTxPubKeys(uint32_t startHeight,
                                                                      uint32_t endHeight) const
{
  if (!m_enableWalletIndexes)
    return {};

  std::vector<crypto::PublicKey> result;

  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return result;

  MDBX_cursor *cursor;
  rc = mdbx_cursor_open(txn, m_dbiTxPubKeySeen, &cursor);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    return result;
  }

  MDBX_val ckey, cval;
  int crc = mdbx_cursor_get(cursor, &ckey, &cval, MDBX_FIRST);
  while (crc == MDBX_SUCCESS)
  {
    if (cval.iov_len == sizeof(TxPubKeySeen))
    {
      TxPubKeySeen seen;
      memcpy(&seen, cval.iov_base, sizeof(seen));

      if (seen.first_seen >= startHeight && seen.first_seen <= endHeight)
      {
        std::string key_str(static_cast<const char *>(ckey.iov_base), ckey.iov_len);
        if (key_str.rfind("txpkseen_", 0) == 0)
        {
          std::string pk_hex = key_str.substr(9);
          crypto::PublicKey pk;
          if (common::podFromHex(pk_hex, pk))
            result.push_back(pk);
        }
      }
    }
    crc = mdbx_cursor_get(cursor, &ckey, &cval, MDBX_NEXT);
  }

  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  return result;
}

// Diagnostics
std::string MDBXBlockchainStorage::printDatabaseStats() const
{
  std::string result;

  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  if (mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS)
    return "error: failed to begin transaction";

  // Core databases
  MDBX_dbi coreDbis[] = {m_dbiBlockEntries, m_dbiBlockHeaders, m_dbiHeights, m_dbiPoolState};
  const char *coreNames[] = {"block_entries", "block_headers", "heights", "pool_state"};

  for (int i = 0; i < 4; i++)
  {
    MDBX_stat stat;
    if (mdbx_dbi_stat(txn, coreDbis[i], &stat, sizeof(stat)) == MDBX_SUCCESS)
    {
      size_t dataMB = (stat.ms_leaf_pages * 4096) >> 20;
      char line[128];
      snprintf(line, sizeof(line), "  %-18s %8lu entries  %6zu MB\n",
               coreNames[i], (unsigned long)stat.ms_entries, dataMB);
      result += line;
    }
  }

  // Wallet index databases (only if enabled)
  if (m_enableWalletIndexes)
  {
    MDBX_dbi walletDbis[] = {m_dbiTxPubKeyOutputs, m_dbiOutputDetails, m_dbiKeyImageOwner, m_dbiTxPubKeySeen};
    const char *walletNames[] = {"tx_pubkey_outputs", "output_details", "key_image_owner", "tx_pubkey_seen"};

    for (int i = 0; i < 4; i++)
    {
      MDBX_stat stat;
      if (mdbx_dbi_stat(txn, walletDbis[i], &stat, sizeof(stat)) == MDBX_SUCCESS)
      {
        size_t dataMB = (stat.ms_leaf_pages * 4096) >> 20;
        char line[128];
        snprintf(line, sizeof(line), "  %-18s %8lu entries  %6zu MB\n",
                 walletNames[i], (unsigned long)stat.ms_entries, dataMB);
        result += line;
      }
    }
  }

  mdbx_txn_abort(txn);
  return result;
}