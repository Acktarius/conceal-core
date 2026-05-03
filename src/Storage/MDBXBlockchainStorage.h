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
    explicit MDBXBlockchainStorage(const std::string &dataDir);
    ~MDBXBlockchainStorage() override;

    bool blockExists(const crypto::Hash &hash) const override;
    bool getBlock(const crypto::Hash &hash, cn::Block &block) const override;
    uint32_t getBlockHeight(const crypto::Hash &hash) const override;
    crypto::Hash getBlockHash(uint32_t height) const override;

    void addBlock(const cn::Block &block, const crypto::Hash &hash, uint32_t height) override;
    void removeBlock(const crypto::Hash &hash) override;

    bool isSpentKeyImage(const crypto::KeyImage &keyImage) const override;
    void markKeyImageSpent(const crypto::KeyImage &keyImage) override;

    uint32_t topBlockHeight() const override;
    void setTopBlockHeight(uint32_t height) override;

    void flush() override;
    void close() override;

    void pushBlockEntry(uint32_t height, const cn::BinaryArray &serializedEntry) override;
    bool getBlockEntry(uint32_t height, cn::BinaryArray &serializedEntry) const override;
    void popBlockEntry(uint32_t height) override;

    void setInitialized();
    bool isInitialized() const;

    void pushBlockHeader(uint32_t height, const cn::BlockHeaderPOD &hdr);
    bool getBlockHeader(uint32_t height, cn::BlockHeaderPOD &hdr) const;

    void putMeta(const std::string &key, const std::vector<uint8_t> &value);
    bool getMeta(const std::string &key, std::vector<uint8_t> &value) const;

    void abortWriteTxn();

    std::string printDatabaseStats() const;

  private:
    void openEnvironment(const std::string &path);
    void openDatabases(MDBX_txn *txn);
    void commitWriteTransaction(bool force = false);
    void ensureWriteTxn();
    void setTopBlockHeightInternal(uint32_t height);

    MDBX_env *m_env = nullptr;
    MDBX_dbi m_dbiHeights;
    MDBX_dbi m_dbiBlockHeights;
    MDBX_dbi m_dbiMeta;
    MDBX_dbi m_dbiBlockEntries;
    MDBX_dbi m_dbiBlockHeaders;

    mutable std::mutex m_txMutex;
    MDBX_txn *m_writeTxn = nullptr;
    size_t m_opsSinceLastCommit = 0;
    static constexpr size_t kCommitBatchSize = 1000;
    uint32_t m_cachedTopHeight = 0;

    std::string m_dataDir;
  };

} // namespace CryptoNote