#pragma once

#include <cstdint>
#include <vector>
#include "crypto/hash.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"

namespace CryptoNote
{

  class IBlockchainStorage
  {
  public:
    virtual ~IBlockchainStorage() = default;

    // ---------- Block-level access ----------
    virtual bool blockExists(const crypto::Hash &hash) const = 0;
    virtual bool getBlock(const crypto::Hash &hash, cn::Block &block) const = 0;
    virtual uint32_t getBlockHeight(const crypto::Hash &hash) const = 0;
    virtual crypto::Hash getBlockHash(uint32_t height) const = 0;

    // ---------- Batch write operations (used during sync) ----------
    virtual void addBlock(const cn::Block &block, const crypto::Hash &hash, uint32_t height) = 0;
    virtual void removeBlock(const crypto::Hash &hash) = 0; // for reorgs

    // ---------- Spent key images (double-spend protection) ----------
    virtual bool isSpentKeyImage(const crypto::KeyImage &keyImage) const = 0;
    virtual void markKeyImageSpent(const crypto::KeyImage &keyImage) = 0;

    // ---------- Global state ----------
    virtual uint32_t topBlockHeight() const = 0;
    virtual void setTopBlockHeight(uint32_t height) = 0;

    // ---------- Persistence & lifecycle ----------
    virtual void flush() = 0;
    virtual void close() = 0;

    // ---------- BlockEntry storage (serialized to/from BinaryArray) ----------
    virtual void pushBlockEntry(uint32_t height, const cn::BinaryArray &serializedEntry) = 0;
    virtual bool getBlockEntry(uint32_t height, cn::BinaryArray &serializedEntry) const = 0;
    virtual void popBlockEntry(uint32_t height) = 0;
  };

} // namespace CryptoNote