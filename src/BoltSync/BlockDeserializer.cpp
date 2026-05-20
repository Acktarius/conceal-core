#include "BlockDeserializer.h"
#include "CryptoHelpers.h"

#include "CryptoNoteCore/CryptoNoteBasic.h"
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

      auto scanTx = [&](cn::Transaction &tx, uint32_t blockHeight)
      {
        crypto::PublicKey txPubKey = cn::getTransactionPublicKeyFromExtra(tx.extra);
        if (txPubKey == NULL_KEY)
          return;

        crypto::KeyDerivation derivation;
        if (!crypto::generate_key_derivation(txPubKey, ctx.viewKey, derivation))
          return;

        crypto::Hash txHash;
        if (!getTxHash(tx, txHash))
          return;

        // Sequential key counter across all output keys in the tx (KeyOutput counts 1,
        // MultisignatureOutput counts N = number of keys it contains).
        size_t keyIndex = 0;

        for (size_t outIdx = 0; outIdx < tx.outputs.size(); ++outIdx)
        {
          const auto &out = tx.outputs[outIdx];

          if (out.target.type() == typeid(cn::KeyOutput))
          {
            const auto &keyOut = boost::get<cn::KeyOutput>(out.target);
            crypto::PublicKey derivedKey;
            if (crypto::derive_public_key(derivation, keyIndex, ctx.spendPublicKey, derivedKey) &&
                derivedKey == keyOut.key)
            {
              FoundOutput fo;
              fo.blockHeight = blockHeight;
              fo.txHash = txHash;
              fo.outputIndex = static_cast<uint32_t>(outIdx);
              fo.amount = out.amount;
              fo.outputKey = keyOut.key;
              fo.txPublicKey = txPubKey;
              fo.txExtra = tx.extra;
              fo.isDeposit = false;
              fo.term = 0;
              if (ctx.spendKey)
              {
                crypto::SecretKey outSec = deriveOutputSecretKey(derivation, keyIndex, *ctx.spendKey);
                crypto::generate_key_image(keyOut.key, outSec, fo.keyImage);
              }
              std::lock_guard<std::mutex> lock(ctx.resultsMutex);
              ctx.results.push_back(std::move(fo));
            }
            ++keyIndex;
          }
          else if (out.target.type() == typeid(cn::MultisignatureOutput))
          {
            // Deposits use MultisignatureOutput (term > 0 means it is a deposit).
            // Ownership: underive_public_key(derivation, outIdx, key) must recover our spendPublicKey.
            // The reference (TransfersConsumer) uses outIdx (not the running keyIndex) for multisig.
            const auto &msigOut = boost::get<cn::MultisignatureOutput>(out.target);
            for (size_t ki = 0; ki < msigOut.keys.size(); ++ki)
            {
              crypto::PublicKey recoveredSpend;
              if (crypto::underive_public_key(derivation, outIdx, msigOut.keys[ki], recoveredSpend) &&
                  recoveredSpend == ctx.spendPublicKey)
              {
                FoundOutput fo;
                fo.blockHeight = blockHeight;
                fo.txHash = txHash;
                fo.outputIndex = static_cast<uint32_t>(outIdx);
                fo.amount = out.amount;
                fo.outputKey = msigOut.keys[ki];
                fo.txPublicKey = txPubKey;
                fo.txExtra = tx.extra;
                fo.isDeposit = (msigOut.term > 0);
                fo.term = msigOut.term;
                std::lock_guard<std::mutex> lock(ctx.resultsMutex);
                ctx.results.push_back(std::move(fo));
                break;
              }
            }
            keyIndex += msigOut.keys.size();
          }
        }
      };

      // Process base transaction (coinbase)
      scanTx(block.baseTransaction, h);

      // Process regular transactions
      for (auto &tx : transactions)
        scanTx(tx, h);
    }
    catch (const std::exception &)
    {
    }
    ctx.blocksProcessed.fetch_add(1, std::memory_order_relaxed);
  }

  void markSpentOutputs(CryptoNote::MDBXBlockchainStorage &storage,
                        uint32_t topHeight,
                        std::vector<FoundOutput> &results)
  {
    if (results.empty())
      return;

    // Build keyImage → result index map for all non-deposit outputs we found.
    // Deposits (MultisignatureOutput) are spent via MultisignatureInput which
    // references by global output index — a separate concern handled later.
    std::unordered_map<crypto::KeyImage, size_t> keyImageIndex;
    keyImageIndex.reserve(results.size());
    for (size_t i = 0; i < results.size(); ++i)
    {
      const auto &fo = results[i];
      if (!fo.isDeposit)
      {
        static const crypto::KeyImage NULL_KI = {};
        if (!(fo.keyImage == NULL_KI))
          keyImageIndex[fo.keyImage] = i;
      }
    }

    if (keyImageIndex.empty())
      return;

    // Sequential pass: for every KeyInput in every transaction, check if
    // its keyImage matches one of our outputs. Mark matched outputs as spent.
    for (uint32_t h = 0; h <= topHeight; ++h)
    {
      cn::BinaryArray serializedEntry;
      if (!storage.getBlockEntry(h, serializedEntry))
        continue;

      cn::Block block;
      std::vector<cn::Transaction> transactions;
      if (!deserializeBlockEntry(serializedEntry, block, transactions))
        continue;

      auto checkInputs = [&](const cn::Transaction &tx)
      {
        for (const auto &input : tx.inputs)
        {
          if (input.type() == typeid(cn::KeyInput))
          {
            const auto &ki = boost::get<cn::KeyInput>(input).keyImage;
            auto it = keyImageIndex.find(ki);
            if (it != keyImageIndex.end())
              results[it->second].spent = true;
          }
        }
      };

      checkInputs(block.baseTransaction);
      for (const auto &tx : transactions)
        checkInputs(tx);
    }
  }
}