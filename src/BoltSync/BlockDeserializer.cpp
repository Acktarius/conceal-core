// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BlockDeserializer.h"
#include "CryptoHelpers.h"

#include "CryptoNoteCore/TransactionExtra.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Serialization/BinaryInputStreamSerializer.h"
#include "Common/MemoryInputStream.h"

namespace BoltSync
{
  // Match the daemon's BlockEntry exactly
  struct TransactionEntry
  {
    cn::Transaction tx;
    std::vector<uint32_t> m_global_output_indexes;

    void serialize(cn::ISerializer &s)
    {
      s(tx, "tx");
      s(m_global_output_indexes, "indexes");
    }
  };

  struct BlockEntry
  {
    cn::Block bl;
    uint32_t height;
    uint64_t block_cumulative_size;
    uint64_t cumulative_difficulty;
    uint64_t already_generated_coins;
    std::vector<TransactionEntry> transactions;

    void serialize(cn::ISerializer &s)
    {
      s(bl, "block");
      s(height, "height");
      s(block_cumulative_size, "block_cumulative_size");
      s(cumulative_difficulty, "cumulative_difficulty");
      s(already_generated_coins, "already_generated_coins");
      s(transactions, "transactions");
    }
  };

  bool deserializeBlockEntry(const cn::BinaryArray &rawEntry,
                             cn::Block &block,
                             std::vector<cn::Transaction> &transactions)
  {
    try
    {
      common::MemoryInputStream stream(rawEntry.data(), rawEntry.size());
      cn::BinaryInputStreamSerializer serializer(stream);

      BlockEntry entry;
      entry.serialize(serializer);

      block = entry.bl;
      transactions.clear();
      transactions.reserve(entry.transactions.size());
      for (const auto &txe : entry.transactions)
        transactions.push_back(txe.tx);

      return true;
    }
    catch (const std::exception &)
    {
      return false;
    }
  }

  void scanSingleBlock(uint32_t h, ScanContext &ctx)
  {
    try
    {
      cn::BinaryArray serializedEntry;
      if (!ctx.storage.getBlockEntry(h, serializedEntry))
      {
        ctx.blocksProcessed.fetch_add(1, std::memory_order_relaxed);
        return;
      }

      cn::Block block;
      std::vector<cn::Transaction> transactions;
      if (!deserializeBlockEntry(serializedEntry, block, transactions))
      {
        ctx.blocksProcessed.fetch_add(1, std::memory_order_relaxed);
        return;
      }

      static const crypto::PublicKey NULL_KEY = {};

      // Process base transaction (coinbase)
      {
        cn::Transaction &tx = block.baseTransaction;
        crypto::PublicKey txPubKey = cn::getTransactionPublicKeyFromExtra(tx.extra);
        if (!(txPubKey == NULL_KEY))
        {
          for (size_t outIdx = 0; outIdx < tx.outputs.size(); ++outIdx)
          {
            const auto &out = tx.outputs[outIdx];
            if (out.target.type() != typeid(cn::KeyOutput))
              continue;
            const auto &keyOut = boost::get<cn::KeyOutput>(out.target);
            if (isOutputOurs(txPubKey, outIdx, keyOut.key, ctx.viewKey, ctx.viewPublicKey))
            {
              crypto::Hash txHash;
              if (!getTxHash(tx, txHash))
                continue;
              FoundOutput fo;
              fo.blockHeight = h;
              fo.txHash = txHash;
              fo.outputIndex = static_cast<uint32_t>(outIdx);
              fo.amount = out.amount;
              fo.outputKey = keyOut.key;
              fo.txPublicKey = txPubKey;
              if (ctx.spendKey)
              {
                crypto::KeyDerivation derivation;
                crypto::generate_key_derivation(txPubKey, ctx.viewKey, derivation);
                crypto::SecretKey outSec = deriveOutputSecretKey(derivation, outIdx, *ctx.spendKey);
                crypto::generate_key_image(keyOut.key, outSec, fo.keyImage);
              }
              std::lock_guard<std::mutex> lock(ctx.resultsMutex);
              ctx.results.push_back(std::move(fo));
            }
          }
        }
      }

      // Process regular transactions
      for (size_t txIdx = 0; txIdx < transactions.size(); ++txIdx)
      {
        cn::Transaction &tx = transactions[txIdx];
        crypto::PublicKey txPubKey = cn::getTransactionPublicKeyFromExtra(tx.extra);
        if (txPubKey == NULL_KEY)
          continue;
        for (size_t outIdx = 0; outIdx < tx.outputs.size(); ++outIdx)
        {
          const auto &out = tx.outputs[outIdx];
          if (out.target.type() != typeid(cn::KeyOutput))
            continue;
          const auto &keyOut = boost::get<cn::KeyOutput>(out.target);
          if (isOutputOurs(txPubKey, outIdx, keyOut.key, ctx.viewKey, ctx.viewPublicKey))
          {
            crypto::Hash txHash;
            if (!getTxHash(tx, txHash))
              continue;
            FoundOutput fo;
            fo.blockHeight = h;
            fo.txHash = txHash;
            fo.outputIndex = static_cast<uint32_t>(outIdx);
            fo.amount = out.amount;
            fo.outputKey = keyOut.key;
            fo.txPublicKey = txPubKey;
            if (ctx.spendKey)
            {
              crypto::KeyDerivation derivation;
              crypto::generate_key_derivation(txPubKey, ctx.viewKey, derivation);
              crypto::SecretKey outSec = deriveOutputSecretKey(derivation, outIdx, *ctx.spendKey);
              crypto::generate_key_image(keyOut.key, outSec, fo.keyImage);
            }
            std::lock_guard<std::mutex> lock(ctx.resultsMutex);
            ctx.results.push_back(std::move(fo));
          }
        }
      }
    }
    catch (const std::exception &)
    {
    }
    ctx.blocksProcessed.fetch_add(1, std::memory_order_relaxed);
  }
}