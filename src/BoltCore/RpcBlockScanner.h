// RpcBlockScanner - scans a height range via daemon RPC and feeds owned
// outputs into a BoltCore::Wallet.  Provides inline spend detection so
// the wallet sees the correct spent/unspent state from the first ingest.
//
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltCoreTypes.h"
#include "BlockchainExplorerData.h"
#include "INode.h"
#include "crypto/crypto.h"

#include <boost/functional/hash.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace BoltCore
{
  class Wallet;

  // ── Config ─────────────────────────────────────────────────────────────────

  struct RpcScanConfig
  {
    uint32_t startHeight   = 0;
    uint32_t endHeight     = 0;    // 0 = query node for current tip
    uint32_t batchSize     = 100;  // heights per getBlocks call, clamped to [1, 300]
    std::chrono::seconds rpcTimeout{30};

    // Called after each batch; args: (lastScannedHeight, totalOutputsFound)
    std::function<void(uint32_t, size_t)> onProgress;
  };

  struct RpcScanResult
  {
    bool        ok           = false;
    uint32_t    scannedTop   = 0;  // highest height actually processed
    size_t      outputsFound = 0;
    size_t      spendMarked  = 0;
    std::string error;
  };

  // ── Scanner ────────────────────────────────────────────────────────────────

  // Fetches BlockDetails from the daemon in batches, identifies wallet-owned
  // outputs using the same crypto as OwnedOutputScanner / NewOutputScanner,
  // detects spends inline by walking every tx input in the scanned range,
  // and ingests confirmed txs via Wallet::addDiscoveredTransaction().
  //
  // Thread-safety: not thread-safe; use one instance per thread.
  class RpcBlockScanner
  {
  public:
    // spendKey may be nullptr for view-only wallets (no key-image generation).
    RpcBlockScanner(cn::INode               &node,
                    const crypto::SecretKey &viewKey,
                    const crypto::PublicKey &spendPub,
                    const crypto::SecretKey *spendKey);

    RpcScanResult scan(const RpcScanConfig &config, Wallet &wallet);

  private:
    // ── Spend-evidence containers ─────────────────────────────────────────

    struct KeyImageHash
    {
      size_t operator()(const crypto::KeyImage &ki) const noexcept
      {
        // XOR 64-bit words — fast, good distribution for 32-byte keys
        const auto *p = reinterpret_cast<const size_t *>(ki.data);
        size_t h = 0;
        for (size_t i = 0; i < sizeof(ki.data) / sizeof(size_t); ++i)
          h ^= p[i];
        return h;
      }
    };

    // (txHash, outputIndex) for legacy multisig / deposit inputs
    struct DepositRefHash
    {
      size_t operator()(const std::pair<crypto::Hash, uint32_t> &r) const noexcept
      {
        return boost::hash<crypto::Hash>()(r.first) ^ std::hash<uint32_t>()(r.second);
      }
    };

    using KeyImageSet    = std::unordered_set<crypto::KeyImage, KeyImageHash>;
    using DepositRefSet  = std::unordered_set<std::pair<crypto::Hash, uint32_t>, DepositRefHash>;

    // ── RPC helpers ───────────────────────────────────────────────────────

    // Synchronous wrapper around INode::getBlocks(heights, …).
    // Returns a system error code; `out` is filled only on success.
    std::error_code fetchBatch(
        const std::vector<uint32_t>                &heights,
        std::vector<std::vector<cn::BlockDetails>> &out,
        std::chrono::seconds                        timeout) const;

    // ── Per-transaction crypto scan (works on BlockDetails types) ─────────

    // Returns all wallet-owned outputs found in txDetails.
    // globalIndex for each output comes directly from BlockDetails (free via RPC).
    std::vector<OutputInfo> scanTransaction(
        const cn::TransactionDetails &txDetails,
        uint32_t                      blockHeight) const;

    // Legacy KeyOutput (0x02)
    bool tryScanKeyOutput(
        const cn::TransactionOutputToKeyDetails &out,
        uint64_t                                 amount,
        uint32_t                                 outputIndex,
        uint32_t                                 globalIndex,
        uint32_t                                 blockHeight,
        const crypto::Hash                      &txHash,
        const crypto::PublicKey                 &txPubKey,
        const crypto::KeyDerivation             &derivation,
        std::vector<OutputInfo>                 &results) const;

    // Legacy MultisignatureOutput (0x03, deposits)
    bool tryScanLegacyMultisigOutput(
        const cn::TransactionOutputMultisignatureDetails &out,
        uint64_t                                          amount,
        uint32_t                                          outputIndex,
        uint32_t                                          globalIndex,
        uint32_t                                          blockHeight,
        const crypto::Hash                               &txHash,
        const crypto::PublicKey                          &txPubKey,
        const crypto::KeyDerivation                      &derivation,
        std::vector<OutputInfo>                          &results) const;

    // New StandardPaymentOutput (0x04) — view-tag + key_index
    bool tryScanStandardOutput(
        const cn::TransactionOutputStandardPaymentDetails &out,
        uint64_t                                           amount,
        uint32_t                                           outputIndex,
        uint32_t                                           globalIndex,
        uint32_t                                           blockHeight,
        const crypto::Hash                                &txHash,
        const crypto::PublicKey                           &txPubKey,
        const crypto::KeyDerivation                       &derivation,
        std::vector<OutputInfo>                           &results) const;

    // New MultisigPaymentOutput (0x05) — deposits with view-tag
    bool tryScanNewMultisigOutput(
        const cn::TransactionOutputMultisigPaymentDetails &out,
        uint64_t                                           amount,
        uint32_t                                           outputIndex,
        uint32_t                                           globalIndex,
        uint32_t                                           blockHeight,
        const crypto::Hash                                &txHash,
        const crypto::PublicKey                           &txPubKey,
        const crypto::KeyDerivation                       &derivation,
        std::vector<OutputInfo>                           &results) const;

    // ── Inline spend detection ────────────────────────────────────────────

    // Walks every tx input in the batch; populates spentKIs and spentDeposits.
    void collectSpendEvidence(
        const std::vector<cn::BlockDetails> &blocks,
        KeyImageSet                         &spentKIs,
        DepositRefSet                       &spentDeposits) const;

    // Marks outputs as spent if their key-image / deposit-ref was collected above.
    // Called before ingesting the batch into the wallet so addDiscoveredTransaction
    // sees the correct spent flag from the start.
    static void applySpends(
        std::vector<OutputInfo> &outputs,
        const KeyImageSet       &spentKIs,
        const DepositRefSet     &spentDeposits);

    // ── Members ───────────────────────────────────────────────────────────

    cn::INode               &m_node;
    const crypto::SecretKey &m_viewKey;
    const crypto::PublicKey &m_spendPub;
    const crypto::SecretKey *m_spendKey;  // nullptr → view-only
  };

} // namespace BoltCore
