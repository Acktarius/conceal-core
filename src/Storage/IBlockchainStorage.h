// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>
#include "crypto/hash.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "Serialization/BinarySerializationTools.h"
#include "Serialization/SerializationOverloads.h"

namespace CryptoNote
{
  // ──────────────────────────────────────────────────────────
  // Wallet import structs
  // ──────────────────────────────────────────────────────────

  struct OutputRef
  {
    uint32_t block_height;
    uint32_t tx_index;
    uint16_t output_index;

    OutputRef() : block_height(0), tx_index(0), output_index(0) {}
    OutputRef(uint32_t h, uint32_t t, uint16_t o) : block_height(h), tx_index(t), output_index(o) {}
  };

  struct WalletOutputInfo
  {
    uint32_t block_height;
    crypto::Hash tx_hash;
    uint64_t amount;
    uint16_t output_index;
    crypto::PublicKey output_key;
    crypto::PublicKey tx_public_key;

    void serialize(cn::ISerializer &s)
    {
      s(block_height, "height");
      s.binary(tx_hash.data, sizeof(tx_hash.data), "tx_hash");
      s(amount, "amount");
      s(output_index, "output_index");
      s.binary(output_key.data, sizeof(output_key.data), "output_key");
      s.binary(tx_public_key.data, sizeof(tx_public_key.data), "tx_pubkey");
    }
  };

  struct KeyImageOwner
  {
    crypto::PublicKey tx_pub_key;
    uint32_t spent_height;

    void serialize(cn::ISerializer &s)
    {
      s.binary(tx_pub_key.data, sizeof(tx_pub_key.data), "tx_pubkey");
      s(spent_height, "spent_height");
    }
  };

  struct TxPubKeySeen
  {
    uint32_t first_seen;
    uint32_t last_seen;

    void serialize(cn::ISerializer &s)
    {
      KV_MEMBER(first_seen)
      KV_MEMBER(last_seen)
    }
  };

  // ──────────────────────────────────────────────────────────
  // Interface — slimmed to only what MDBX storage provides
  // ──────────────────────────────────────────────────────────

  class IBlockchainStorage
  {
  public:
    virtual ~IBlockchainStorage() = default;

    // ── Lifecycle
    virtual void flush() = 0;
    virtual void close() = 0;

    // ── Atomic block write (single transaction, immediate commit)
    virtual void pushCompleteBlock(uint32_t height,
                                   const crypto::Hash &hash,
                                   const cn::BinaryArray &serializedEntry,
                                   const cn::BlockHeaderPOD &hdr) = 0;

    // ── Atomic block removal
    virtual void removeCompleteBlock(uint32_t height, const crypto::Hash &hash) = 0;

    // ── Block reads
    virtual bool getBlockEntry(uint32_t height, cn::BinaryArray &serializedEntry) const = 0;
    virtual bool getBlockHeader(uint32_t height, cn::BlockHeaderPOD &hdr) const = 0;
    virtual void getBlockHeadersRange(uint32_t startHeight, uint32_t count,
                                      std::vector<cn::BlockHeaderPOD> &out) const = 0;

    // ── Height tracking
    virtual uint32_t topBlockHeight() const = 0;

    // ── Transaction pool persistence
    virtual void storePoolState(const std::vector<cn::BinaryArray> &serializedTxs,
                                const std::vector<crypto::KeyImage> &spentKeyImages) = 0;
    virtual std::vector<cn::BinaryArray> loadPoolTransactions() const = 0;
    virtual std::vector<crypto::KeyImage> loadPoolSpentKeyImages() const = 0;

    // ── Wallet instant import — indexing (called during block processing)
    virtual void indexOutputByTxPubKey(const crypto::PublicKey &tx_pub_key,
                                       uint32_t height,
                                       uint32_t tx_index,
                                       uint16_t output_index,
                                       const crypto::Hash &tx_hash,
                                       uint64_t amount,
                                       const crypto::PublicKey &output_key) = 0;

    virtual void indexSpentKeyImage(const crypto::KeyImage &key_image,
                                    const crypto::PublicKey &tx_pub_key,
                                    uint32_t spent_height) = 0;

    // ── Wallet instant import — querying (called from RPC)
    virtual bool getOutputsByTxPubKeys(const std::vector<crypto::PublicKey> &tx_pub_keys,
                                       std::vector<WalletOutputInfo> &outputs,
                                       std::unordered_set<std::string> &spent_key_images) const = 0;

    virtual std::vector<crypto::PublicKey> getNewTxPubKeys(uint32_t startHeight,
                                                           uint32_t endHeight) const = 0;

    virtual bool isSpentKeyImage(const crypto::KeyImage &keyImage) const = 0;
  };

} // namespace CryptoNote