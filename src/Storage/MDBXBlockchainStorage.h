#pragma once

#include "IBlockchainStorage.h"
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

    // IBlockchainStorage interface
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

  private:
    void openEnvironment(const std::string &path);
    void openDatabases(MDBX_txn *txn);
    void commitWriteTransaction(bool force = false);
    void ensureWriteTxn();
    void setTopBlockHeightInternal(uint32_t height);

    // MDBX handles
    MDBX_env *m_env = nullptr;
    MDBX_dbi m_dbiBlocks;
    MDBX_dbi m_dbiHeights;
    MDBX_dbi m_dbiBlockHeights;
    MDBX_dbi m_dbiSpentKeys;
    MDBX_dbi m_dbiMeta;

    // Transaction management
    mutable std::mutex m_txMutex;
    MDBX_txn *m_writeTxn = nullptr;
    size_t m_opsSinceLastCommit = 0;
    static constexpr size_t kCommitBatchSize = 1000;
    uint32_t m_cachedTopHeight = 0;

    std::string m_dataDir;
  };

} // namespace CryptoNote