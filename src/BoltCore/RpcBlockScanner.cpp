// RpcBlockScanner implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "RpcBlockScanner.h"
#include "BoltCore.h"
#include "BoltSync/CryptoHelpers.h"

#include <boost/variant.hpp>
#include <algorithm>
#include <cstring>
#include <future>
#include <system_error>

namespace BoltCore
{
  namespace
  {
    static const uint32_t MAX_BATCH_SIZE = 300;
    static const crypto::PublicKey NULL_PUB_KEY = {};

    // Extract the first tx public key from a TransactionExtraDetails.
    bool getTxPubKey(const cn::TransactionExtraDetails &extra, crypto::PublicKey &out)
    {
      if (extra.publicKey.empty())
        return false;
      out = extra.publicKey[0];
      return out != NULL_PUB_KEY;
    }

    // Compute key image for a legacy KeyOutput we own.
    crypto::KeyImage computeKeyImage(const crypto::KeyDerivation &derivation,
                                     size_t                       outputIndex,
                                     const crypto::PublicKey     &outputKey,
                                     const crypto::SecretKey     &spendKey)
    {
      crypto::SecretKey outSec = BoltSync::deriveOutputSecretKey(derivation, outputIndex, spendKey);
      crypto::KeyImage ki;
      crypto::generate_key_image(outputKey, outSec, ki);
      return ki;
    }
  }

  // ── Constructor ────────────────────────────────────────────────────────────

  RpcBlockScanner::RpcBlockScanner(cn::INode               &node,
                                   const crypto::SecretKey &viewKey,
                                   const crypto::PublicKey &spendPub,
                                   const crypto::SecretKey *spendKey)
      : m_node(node), m_viewKey(viewKey), m_spendPub(spendPub), m_spendKey(spendKey)
  {
  }

  // ── Public entry point ─────────────────────────────────────────────────────

  RpcScanResult RpcBlockScanner::scan(const RpcScanConfig &config, Wallet &wallet)
  {
    RpcScanResult result;

    const uint32_t batchSize = std::max(1u, std::min(config.batchSize, MAX_BATCH_SIZE));

    // Resolve end height from the node if not provided.
    uint32_t endHeight = config.endHeight;
    if (endHeight == 0)
    {
      endHeight = m_node.getLastKnownBlockHeight();
      if (endHeight == 0)
      {
        result.error = "Node reports block height 0 — not ready";
        return result;
      }
    }

    if (config.startHeight > endHeight)
    {
      // Nothing to do; already caught up.
      result.ok         = true;
      result.scannedTop = config.startHeight > 0 ? config.startHeight - 1 : 0;
      return result;
    }

    size_t totalFound = 0;
    size_t totalSpent = 0;

    for (uint32_t batchStart = config.startHeight; batchStart <= endHeight; batchStart += batchSize)
    {
      const uint32_t batchEnd = std::min(batchStart + batchSize - 1, endHeight);

      // Build the height list for this batch.
      std::vector<uint32_t> heights;
      heights.reserve(batchEnd - batchStart + 1);
      for (uint32_t h = batchStart; h <= batchEnd; ++h)
        heights.push_back(h);

      // Fetch from daemon.
      std::vector<std::vector<cn::BlockDetails>> rawBatch;
      if (const auto ec = fetchBatch(heights, rawBatch, config.rpcTimeout); ec)
      {
        result.error = "RPC getBlocks failed at height " + std::to_string(batchStart)
                       + ": " + ec.message();
        return result;
      }

      // Flatten to a single vector of non-orphaned blocks (one per height).
      std::vector<cn::BlockDetails> batch;
      batch.reserve(heights.size());
      for (auto &perHeight : rawBatch)
      {
        for (auto &bd : perHeight)
        {
          if (!bd.isOrphaned)
          {
            batch.push_back(std::move(bd));
            break;
          }
        }
      }

      // Collect spend evidence for the entire batch before ingesting anything.
      KeyImageSet   spentKIs;
      DepositRefSet spentDeposits;
      collectSpendEvidence(batch, spentKIs, spentDeposits);

      // Scan each block's transactions and feed them to the wallet.
      for (const auto &block : batch)
      {
        for (const auto &txDetails : block.transactions)
        {
          std::vector<OutputInfo> owned = scanTransaction(txDetails, block.height);
          if (owned.empty())
            continue;

          // Mark spends before ingestion so the wallet sees the right state.
          applySpends(owned, spentKIs, spentDeposits);

          for (const auto &o : owned)
          {
            if (o.spent)
              ++totalSpent;
          }
          totalFound += owned.size();

          wallet.addDiscoveredTransaction(txDetails.hash, owned, block.height);
        }
      }

      wallet.setCurrentHeight(batchEnd);
      result.scannedTop = batchEnd;

      if (config.onProgress)
        config.onProgress(batchEnd, totalFound);
    }

    result.ok           = true;
    result.outputsFound = totalFound;
    result.spendMarked  = totalSpent;
    return result;
  }

  // ── RPC helper ─────────────────────────────────────────────────────────────

  std::error_code RpcBlockScanner::fetchBatch(
      const std::vector<uint32_t>                &heights,
      std::vector<std::vector<cn::BlockDetails>> &out,
      std::chrono::seconds                        timeout) const
  {
    std::promise<std::error_code> promise;
    auto future = promise.get_future();

    out.resize(heights.size());
    m_node.getBlocks(heights, out,
                     [&promise](std::error_code ec) { promise.set_value(ec); });

    if (future.wait_for(timeout) != std::future_status::ready)
      return std::make_error_code(std::errc::timed_out);

    return future.get();
  }

  // ── Per-transaction output scan ────────────────────────────────────────────

  std::vector<OutputInfo> RpcBlockScanner::scanTransaction(
      const cn::TransactionDetails &txDetails,
      uint32_t                      blockHeight) const
  {
    std::vector<OutputInfo> results;

    crypto::PublicKey txPubKey;
    if (!getTxPubKey(txDetails.extra, txPubKey))
      return results;

    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(txPubKey, m_viewKey, derivation))
      return results;

    for (size_t o = 0; o < txDetails.outputs.size(); ++o)
    {
      const auto &outDetail = txDetails.outputs[o];
      const uint32_t globalIndex = outDetail.globalIndex;

      using ToKey   = cn::TransactionOutputToKeyDetails;
      using MsigOld = cn::TransactionOutputMultisignatureDetails;
      using StdNew  = cn::TransactionOutputStandardPaymentDetails;
      using MsigNew = cn::TransactionOutputMultisigPaymentDetails;

      if (outDetail.output.type() == typeid(ToKey))
      {
        tryScanKeyOutput(boost::get<ToKey>(outDetail.output),
                         outDetail.amount, static_cast<uint32_t>(o), globalIndex,
                         blockHeight, txDetails.hash, txPubKey, derivation, results);
      }
      else if (outDetail.output.type() == typeid(MsigOld))
      {
        tryScanLegacyMultisigOutput(boost::get<MsigOld>(outDetail.output),
                                    outDetail.amount, static_cast<uint32_t>(o), globalIndex,
                                    blockHeight, txDetails.hash, txPubKey, derivation, results);
      }
      else if (outDetail.output.type() == typeid(StdNew))
      {
        tryScanStandardOutput(boost::get<StdNew>(outDetail.output),
                              outDetail.amount, static_cast<uint32_t>(o), globalIndex,
                              blockHeight, txDetails.hash, txPubKey, derivation, results);
      }
      else if (outDetail.output.type() == typeid(MsigNew))
      {
        tryScanNewMultisigOutput(boost::get<MsigNew>(outDetail.output),
                                 outDetail.amount, static_cast<uint32_t>(o), globalIndex,
                                 blockHeight, txDetails.hash, txPubKey, derivation, results);
      }
      // DomainRegistration / DomainDeletion are not spendable — skip.
    }

    return results;
  }

  // ── Output type scanners ───────────────────────────────────────────────────

  bool RpcBlockScanner::tryScanKeyOutput(
      const cn::TransactionOutputToKeyDetails &out,
      uint64_t                                 amount,
      uint32_t                                 outputIndex,
      uint32_t                                 globalIndex,
      uint32_t                                 blockHeight,
      const crypto::Hash                      &txHash,
      const crypto::PublicKey                 &txPubKey,
      const crypto::KeyDerivation             &derivation,
      std::vector<OutputInfo>                 &results) const
  {
    crypto::PublicKey derivedKey;
    if (!crypto::derive_public_key(derivation, outputIndex, m_spendPub, derivedKey))
      return false;
    if (derivedKey != out.txOutKey)
      return false;

    OutputInfo info = {};
    info.blockHeight          = blockHeight;
    info.txHash               = txHash;
    info.outputIndex          = outputIndex;
    info.globalOutputIndex    = globalIndex;
    info.hasGlobalOutputIndex = true;
    info.amount               = amount;
    info.outputKey            = out.txOutKey;
    info.txPublicKey          = txPubKey;
    info.spent                = false;
    info.isDeposit            = false;
    info.term                 = 0;

    if (m_spendKey)
      info.keyImage = computeKeyImage(derivation, outputIndex, out.txOutKey, *m_spendKey);

    results.push_back(std::move(info));
    return true;
  }

  bool RpcBlockScanner::tryScanLegacyMultisigOutput(
      const cn::TransactionOutputMultisignatureDetails &out,
      uint64_t                                          amount,
      uint32_t                                          outputIndex,
      uint32_t                                          globalIndex,
      uint32_t                                          blockHeight,
      const crypto::Hash                               &txHash,
      const crypto::PublicKey                          &txPubKey,
      const crypto::KeyDerivation                      &derivation,
      std::vector<OutputInfo>                          &results) const
  {
    for (size_t ki = 0; ki < out.keys.size(); ++ki)
    {
      crypto::PublicKey recovered;
      if (!crypto::underive_public_key(derivation, outputIndex, out.keys[ki], recovered))
        continue;
      if (recovered != m_spendPub)
        continue;

      OutputInfo info = {};
      info.blockHeight          = blockHeight;
      info.txHash               = txHash;
      info.outputIndex          = outputIndex;
      info.globalOutputIndex    = globalIndex;
      info.hasGlobalOutputIndex = true;
      info.amount               = amount;
      info.outputKey            = out.keys[ki];
      info.txPublicKey          = txPubKey;
      info.spent                = false;
      info.isDeposit            = true;   // legacy multisig outputs are deposits
      info.term                 = 0;      // term not encoded in legacy type

      results.push_back(std::move(info));
      return true;
    }
    return false;
  }

  bool RpcBlockScanner::tryScanStandardOutput(
      const cn::TransactionOutputStandardPaymentDetails &out,
      uint64_t                                           amount,
      uint32_t                                           outputIndex,
      uint32_t                                           globalIndex,
      uint32_t                                           blockHeight,
      const crypto::Hash                                &txHash,
      const crypto::PublicKey                           &txPubKey,
      const crypto::KeyDerivation                       &derivation,
      std::vector<OutputInfo>                           &results) const
  {
    // Step 1: cheap view-tag filter (eliminates ~255/256 outputs).
    const uint8_t expected = BoltSync::computeWalletViewTag(derivation, outputIndex);
    if (expected != out.viewTag)
      return false;

    // Step 2: full key derivation using on-chain key_index.
    crypto::PublicKey derivedKey;
    if (!crypto::derive_public_key(derivation, out.keyIndex, m_spendPub, derivedKey))
      return false;
    if (derivedKey != out.txOutKey)
      return false;

    OutputInfo info = {};
    info.blockHeight          = blockHeight;
    info.txHash               = txHash;
    info.outputIndex          = outputIndex;
    info.globalOutputIndex    = globalIndex;
    info.hasGlobalOutputIndex = true;
    info.keyDerivationIndex   = out.keyIndex;
    info.hasKeyDerivationIndex = true;
    info.amount               = amount;
    info.outputKey            = out.txOutKey;
    info.txPublicKey          = txPubKey;
    info.spent                = false;
    info.isDeposit            = false;
    info.term                 = 0;

    if (m_spendKey)
      info.keyImage = computeKeyImage(derivation, out.keyIndex, out.txOutKey, *m_spendKey);

    results.push_back(std::move(info));
    return true;
  }

  bool RpcBlockScanner::tryScanNewMultisigOutput(
      const cn::TransactionOutputMultisigPaymentDetails &out,
      uint64_t                                           amount,
      uint32_t                                           outputIndex,
      uint32_t                                           globalIndex,
      uint32_t                                           blockHeight,
      const crypto::Hash                                &txHash,
      const crypto::PublicKey                           &txPubKey,
      const crypto::KeyDerivation                       &derivation,
      std::vector<OutputInfo>                           &results) const
  {
    // Step 1: view-tag filter.
    const uint8_t expected = BoltSync::computeWalletViewTag(derivation, outputIndex);
    if (expected != out.viewTag)
      return false;

    // Step 2: check each key in the multisig group using key_index + slot.
    for (size_t ki = 0; ki < out.keys.size(); ++ki)
    {
      crypto::PublicKey derivedKey;
      if (!crypto::derive_public_key(derivation, out.keyIndex + static_cast<uint16_t>(ki),
                                     m_spendPub, derivedKey))
        continue;
      if (derivedKey != out.keys[ki])
        continue;

      OutputInfo info = {};
      info.blockHeight           = blockHeight;
      info.txHash                = txHash;
      info.outputIndex           = outputIndex;
      info.globalOutputIndex     = globalIndex;
      info.hasGlobalOutputIndex  = true;
      info.keyDerivationIndex    = out.keyIndex + static_cast<uint32_t>(ki);
      info.hasKeyDerivationIndex = true;
      info.amount                = amount;
      info.outputKey             = out.keys[ki];
      info.txPublicKey           = txPubKey;
      info.spent                 = false;
      info.isDeposit             = (out.term > 0);
      info.term                  = out.term;

      results.push_back(std::move(info));
      return true;
    }
    return false;
  }

  // ── Spend detection ────────────────────────────────────────────────────────

  void RpcBlockScanner::collectSpendEvidence(
      const std::vector<cn::BlockDetails> &blocks,
      KeyImageSet                         &spentKIs,
      DepositRefSet                       &spentDeposits) const
  {
    using ToKey   = cn::TransactionInputToKeyDetails;
    using MsigIn  = cn::TransactionInputMultisignatureDetails;

    for (const auto &block : blocks)
    {
      for (const auto &tx : block.transactions)
      {
        for (const auto &inputDetail : tx.inputs)
        {
          if (inputDetail.input.type() == typeid(ToKey))
          {
            const auto &ki = boost::get<ToKey>(inputDetail.input).keyImage;
            static const crypto::KeyImage NULL_KI = {};
            if (std::memcmp(&ki, &NULL_KI, sizeof(ki)) != 0)
              spentKIs.insert(ki);
          }
          else if (inputDetail.input.type() == typeid(MsigIn))
          {
            const auto &ref = boost::get<MsigIn>(inputDetail.input).output;
            spentDeposits.insert({ref.transactionHash, static_cast<uint32_t>(ref.number)});
          }
        }
      }
    }
  }

  void RpcBlockScanner::applySpends(
      std::vector<OutputInfo> &outputs,
      const KeyImageSet       &spentKIs,
      const DepositRefSet     &spentDeposits)
  {
    static const crypto::KeyImage NULL_KI = {};
    for (auto &out : outputs)
    {
      if (out.spent)
        continue;

      if (!out.isDeposit)
      {
        if (std::memcmp(&out.keyImage, &NULL_KI, sizeof(crypto::KeyImage)) != 0 &&
            spentKIs.count(out.keyImage))
        {
          out.spent = true;
        }
      }
      else
      {
        if (spentDeposits.count({out.txHash, out.outputIndex}))
          out.spent = true;
      }
    }
  }

} // namespace BoltCore
