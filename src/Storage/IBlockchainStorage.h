// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

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

    // Block-level access
    virtual bool blockExists(const crypto::Hash &hash) const = 0;                // Check if a block hash exists in the database
    virtual bool getBlock(const crypto::Hash &hash, cn::Block &block) const = 0; // Fetch a full block by hash
    virtual uint32_t getBlockHeight(const crypto::Hash &hash) const = 0;         // Hash > height lookup
    virtual crypto::Hash getBlockHash(uint32_t height) const = 0;                // Height > hash lookup

    // Atomic complete block write
    // Writes block entry, header, bi‑directional height mappings, and updates top height in ONE transaction
    // No orphaned data on crash — either all of it lands or none of it does
    virtual void pushCompleteBlock(uint32_t height,
                                   const crypto::Hash &hash,
                                   const cn::BinaryArray &serializedEntry,
                                   const cn::BlockHeaderPOD &hdr) = 0;

    // Atomic complete block removal (reorg cleanup)
    // Removes header, block entry, and both height mappings in ONE transaction
    virtual void removeCompleteBlock(uint32_t height, const crypto::Hash &hash) = 0;

    // Batch write operations (legacy — still used during reorgs)
    virtual void addBlock(const cn::Block &block, const crypto::Hash &hash, uint32_t height) = 0; // Height mappings only
    virtual void removeBlock(const crypto::Hash &hash) = 0;                                       // Removes height mappings for reorgs

    // Batch header retrieval (avoids N separate read transactions)
    // Fetches [startHeight, startHeight+count) headers with a single cursor scan
    virtual void getBlockHeadersRange(uint32_t startHeight, uint32_t count,
                                      std::vector<cn::BlockHeaderPOD> &out) const = 0;

    // Spent key images (double-spend protection)
    virtual bool isSpentKeyImage(const crypto::KeyImage &keyImage) const = 0; // Check if key image already spent
    virtual void markKeyImageSpent(const crypto::KeyImage &keyImage) = 0;     // Record a spent key image

    // Global state
    virtual uint32_t topBlockHeight() const = 0;         // Returns the current chain tip height
    virtual void setTopBlockHeight(uint32_t height) = 0; // Persists the new chain tip height

    // Persistence & lifecycle
    virtual void flush() = 0; // Force all buffered writes to disk
    virtual void close() = 0; // Abort pending writes and close the environment

    // BlockEntry storage (legacy — prefer pushCompleteBlock)
    virtual void pushBlockEntry(uint32_t height, const cn::BinaryArray &serializedEntry) = 0; // Store a serialised block entry
    virtual bool getBlockEntry(uint32_t height, cn::BinaryArray &serializedEntry) const = 0;  // Read a serialised block entry
    virtual void popBlockEntry(uint32_t height) = 0;                                          // Delete a block entry (reorg pop)

    // Block header storage
    virtual void pushBlockHeader(uint32_t height, const cn::BlockHeaderPOD &hdr) = 0; // Store a block header (legacy)
    virtual bool getBlockHeader(uint32_t height, cn::BlockHeaderPOD &hdr) const = 0;  // Read a single block header

    // Generic metadata store
    virtual void putMeta(const std::string &key, const std::vector<uint8_t> &value) = 0; // Store an arbitrary blob in meta
    virtual bool getMeta(const std::string &key, std::vector<uint8_t> &value) const = 0; // Read an arbitrary blob from meta

    // Initialisation flag
    virtual void setInitialized() = 0;      // Mark database as fully initialised
    virtual bool isInitialized() const = 0; // Check if database is initialised

    // Checkpoint storage
    virtual void storeCheckpoint(uint32_t height, const crypto::Hash &hash) = 0;       // Persist a checkpoint
    virtual std::vector<std::pair<uint32_t, crypto::Hash>> getCheckpoints() const = 0; // Scan all checkpoints

    // Migration
    virtual void migrateToPaddedKeys() = 0; // One‑time conversion to zero‑padded keys
  };

} // namespace CryptoNote