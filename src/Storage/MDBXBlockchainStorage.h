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
#include <unordered_set>
#include <vector>

namespace CryptoNote
{

  class MDBXBlockchainStorage : public IBlockchainStorage
  {
  public:
    explicit MDBXBlockchainStorage(const std::string &dataDir, bool enableWalletIndexes = false);
    ~MDBXBlockchainStorage() override;

    // Lifecycle
    void flush() override;
    void close() override;

    // Atomic block write (single transaction, immediate commit)
    void pushCompleteBlock(uint32_t height,
                           const crypto::Hash &hash,
                           const cn::BinaryArray &serializedEntry,
                           const cn::BlockHeaderPOD &hdr) override;

    // Atomic block removal (single transaction, immediate commit)
    void removeCompleteBlock(uint32_t height, const crypto::Hash &hash) override;

    // Block reads
    bool getBlockEntry(uint32_t height, cn::BinaryArray &serializedEntry) const override;
    bool getBlockHeader(uint32_t height, cn::BlockHeaderPOD &hdr) const override;
    void getBlockHeadersRange(uint32_t startHeight, uint32_t count,
                              std::vector<cn::BlockHeaderPOD> &out) const override;

    // Height tracking (derived from 'heights' database)
    uint32_t topBlockHeight() const override;

    // Transaction pool persistence
    void storePoolState(const std::vector<cn::BinaryArray> &serializedTxs,
                        const std::vector<crypto::KeyImage> &spentKeyImages) override;
    std::vector<cn::BinaryArray> loadPoolTransactions() const override;
    std::vector<crypto::KeyImage> loadPoolSpentKeyImages() const override;

    // Wallet instant import - indexing (called during block processing)
    void indexOutputByTxPubKey(const crypto::PublicKey &tx_pub_key,
                               uint32_t height,
                               uint32_t tx_index,
                               uint16_t output_index,
                               const crypto::Hash &tx_hash,
                               uint64_t amount,
                               const crypto::PublicKey &output_key) override;

    void indexSpentKeyImage(const crypto::KeyImage &key_image,
                            const crypto::PublicKey &tx_pub_key,
                            uint32_t spent_height) override;

    // Wallet instant import - querying (called from RPC)
    bool getOutputsByTxPubKeys(const std::vector<crypto::PublicKey> &tx_pub_keys,
                               std::vector<WalletOutputInfo> &outputs,
                               std::unordered_set<std::string> &spent_key_images) const override;

    std::vector<crypto::PublicKey> getNewTxPubKeys(uint32_t startHeight,
                                                   uint32_t endHeight) const override;

    // Spent key image check (for wallet tools)
    bool isSpentKeyImage(const crypto::KeyImage &keyImage) const;

    // Diagnostics
    std::string printDatabaseStats() const;

    // Used in migration tool
    MDBX_env *getEnv() const { return m_env; }
    MDBX_dbi getDbiBlockEntries() const { return m_dbiBlockEntries; }
    MDBX_dbi getDbiBlockHeaders() const { return m_dbiBlockHeaders; }
    MDBX_dbi getDbiHeights() const { return m_dbiHeights; }

  private:
    // Environment & database setup
    void openEnvironment(const std::string &path);
    void openDatabases(MDBX_txn *txn);

    // Key helpers
    static std::string blockEntryKey(uint32_t height);
    static std::string blockHeaderKey(uint32_t height);
    static std::string makeOutputDetailsKey(uint32_t height, uint32_t tx_idx, uint16_t out_idx);
    static std::string makeKeyImageOwnerKey(const crypto::KeyImage &ki);

    static constexpr int kHeightKeyWidth = 8;

    // Safe MDBX_val constructors
    static MDBX_val to_val(const void *data, size_t len);
    static MDBX_val to_val(const std::string &s);
    static MDBX_val to_val(const crypto::Hash &h);
    static MDBX_val to_val(const cn::BlockHeaderPOD &hdr);

    // MDBX handles
    MDBX_env *m_env = nullptr;
    MDBX_dbi m_dbiBlockEntries; // "be_XXXXXXXX" → serialized BlockEntry
    MDBX_dbi m_dbiBlockHeaders; // "hdr_XXXXXXXX" → BlockHeaderPOD
    MDBX_dbi m_dbiHeights;      // uint32_t height → crypto::Hash
    MDBX_dbi m_dbiPoolState;    // "pool_tx_N" / "pool_ki_N" → binary blobs

    // Wallet instant import indexes
    bool m_enableWalletIndexes = false;
    MDBX_dbi m_dbiTxPubKeyOutputs; // "txpk_<hex>" → list of OutputRef
    MDBX_dbi m_dbiOutputDetails;   // "od_<height><txidx><outidx>" → WalletOutputInfo
    MDBX_dbi m_dbiKeyImageOwner;   // "kio_<hex>" → KeyImageOwner
    MDBX_dbi m_dbiTxPubKeySeen;    // "txpkseen_<hex>" → TxPubKeySeen

    // Thread safety
    mutable std::mutex m_txMutex;

    // Configuration
    std::string m_dataDir;
  };

} // namespace CryptoNote