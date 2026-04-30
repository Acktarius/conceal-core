#pragma once

#include "Storage/IBlockchainStorage.h"

namespace CryptoNote
{

  class LegacyBlockchainStorage : public IBlockchainStorage
  {
  public:
    explicit LegacyBlockchainStorage(const std::string &dataDir);

    // Implement all interface methods by delegating to
    // the existing BlockchainCache or file structures you have now.
    // Example:
    bool blockExists(const crypto::Hash &hash) const override;
    bool getBlock(const crypto::Hash &hash, Block &block) const override;
    // ... etc.

  private:
    // Whatever internal classes you already use (e.g., phmap, file streams)
    std::unique_ptr<ExistingBlockchainCache> m_cache;
  };

} // namespace CryptoNote