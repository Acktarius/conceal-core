// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SyncMonitor.h"

#include "CryptoNoteCore/TransactionExtra.h"

#include <chrono>
#include <iostream>

namespace BoltRPC
{

// Out-of-class definitions required by C++11 for static constexpr members that are ODR-used
constexpr uint32_t SyncMonitor::POLL_INTERVAL_MS;
constexpr uint32_t SyncMonitor::MIN_BLOCKS_TO_SCAN;

  SyncMonitor::SyncMonitor(cn::INode &node,
                           const crypto::SecretKey &viewKey,
                           const crypto::PublicKey &spendPub,
                           const crypto::SecretKey *spendKey,
                           const std::string &dataDir,
                           uint32_t lastScannedHeight,
                           OutputCallback onNewOutputs)
      : m_node(node), m_viewKey(viewKey), m_spendPub(spendPub), m_spendKey(spendKey), m_dataDir(dataDir), m_lastScannedHeight(lastScannedHeight), m_onNewOutputs(std::move(onNewOutputs))
  {
  }

  SyncMonitor::~SyncMonitor() { stop(); }

  void SyncMonitor::start()
  {
    m_stop = false;
    m_thread = std::thread(&SyncMonitor::runLoop, this);
  }

  void SyncMonitor::stop()
  {
    m_stop = true;
    if (m_thread.joinable())
      m_thread.join();
  }

  void SyncMonitor::scanMempool()
  {
    // Get mempool transaction hashes from node
    std::vector<crypto::Hash> mempoolTxHashes = m_node.getPoolTransactions();
    if (mempoolTxHashes.empty())
      return;

    std::lock_guard<std::mutex> lock(m_mempoolMutex);

    for (const auto &txHash : mempoolTxHashes)
    {
      // Skip if already processed
      if (m_processedMempoolTxs.find(txHash) != m_processedMempoolTxs.end())
        continue;

      cn::Transaction tx;
      if (m_node.getTransactionSync(txHash, tx))
      {
        // Check if this transaction has outputs for our wallet
        std::vector<BoltSync::FoundOutput> outputs;

        crypto::PublicKey txPubKey = cn::getTransactionPublicKeyFromExtra(tx.extra);
        if (txPubKey != cn::NULL_PUBLIC_KEY)
        {
          crypto::KeyDerivation derivation;
          if (crypto::generate_key_derivation(txPubKey, m_viewKey, derivation))
          {
            size_t keyIndex = 0;
            for (size_t outIdx = 0; outIdx < tx.outputs.size(); ++outIdx)
            {
              const auto &out = tx.outputs[outIdx];

              if (out.target.type() == typeid(cn::KeyOutput))
              {
                const auto &keyOut = boost::get<cn::KeyOutput>(out.target);
                crypto::PublicKey derivedKey;
                if (crypto::derive_public_key(derivation, keyIndex, m_spendPub, derivedKey) &&
                    derivedKey == keyOut.key)
                {
                  BoltSync::FoundOutput fo;
                  fo.blockHeight = 0;
                  fo.txHash = txHash;
                  fo.outputIndex = static_cast<uint32_t>(outIdx);
                  fo.amount = out.amount;
                  fo.outputKey = keyOut.key;
                  fo.txPublicKey = txPubKey;
                  fo.isDeposit = false;
                  fo.term = 0;
                  outputs.push_back(fo);
                }
                ++keyIndex;
              }
              else if (out.target.type() == typeid(cn::MultisignatureOutput))
              {
                const auto &msigOut = boost::get<cn::MultisignatureOutput>(out.target);
                for (size_t ki = 0; ki < msigOut.keys.size(); ++ki)
                {
                  crypto::PublicKey recoveredSpend;
                  if (crypto::underive_public_key(derivation, outIdx, msigOut.keys[ki], recoveredSpend) &&
                      recoveredSpend == m_spendPub)
                  {
                    BoltSync::FoundOutput fo;
                    fo.blockHeight = 0;
                    fo.txHash = txHash;
                    fo.outputIndex = static_cast<uint32_t>(outIdx);
                    fo.amount = out.amount;
                    fo.outputKey = msigOut.keys[ki];
                    fo.txPublicKey = txPubKey;
                    fo.isDeposit = (msigOut.term > 0);
                    fo.term = msigOut.term;
                    outputs.push_back(fo);
                    break;
                  }
                }
                keyIndex += msigOut.keys.size();
              }
            }
          }
        }

        if (!outputs.empty())
        {
          std::vector<BoltCore::OutputInfo> newOutputs;
          for (const auto &fo : outputs)
          {
            BoltCore::OutputInfo info;
            info.blockHeight = 0;
            info.txHash = fo.txHash;
            info.outputIndex = fo.outputIndex;
            info.globalOutputIndex = fo.outputIndex;
            info.amount = fo.amount;
            info.outputKey = fo.outputKey;
            info.txPublicKey = fo.txPublicKey;
            info.spent = false;
            info.isDeposit = fo.isDeposit;
            info.term = fo.term;
            newOutputs.push_back(info);
          }
          m_onNewOutputs(newOutputs, 0);
        }
      }
      m_processedMempoolTxs.insert(txHash);
    }

    // Clean up old processed mempool transactions (keep last 1000)
    if (m_processedMempoolTxs.size() > 1000)
    {
      auto it = m_processedMempoolTxs.begin();
      std::advance(it, m_processedMempoolTxs.size() - 1000);
      m_processedMempoolTxs.erase(m_processedMempoolTxs.begin(), it);
    }
  }

  void SyncMonitor::runLoop()
  {
    // ── Initial full scan if starting from 0 ──
    if (m_lastScannedHeight.load(std::memory_order_relaxed) == 0 && !m_dataDir.empty())
    {
      uint32_t nodeHeight = m_node.getLastKnownBlockHeight();
      if (nodeHeight == 0)
        nodeHeight = m_node.getLastLocalBlockHeight();

      if (nodeHeight > 0)
      {
        std::cerr << "SyncMonitor: starting initial scan 0-" << nodeHeight << std::endl;

        BoltSync::Scanner scanner(m_viewKey, m_spendPub, m_spendKey);
        BoltSync::ScanConfig cfg;
        cfg.dataDir = m_dataDir;
        cfg.numThreads = 1;
        cfg.startBlock = 0;
        cfg.endBlock = nodeHeight;
        cfg.onProgress = [this](uint32_t h)
        {
          m_lastScannedHeight.store(h, std::memory_order_relaxed);
        };

        // Move ScanState to heap to control lifetime
        std::unique_ptr<BoltSync::ScanState> state(new BoltSync::ScanState());

        try
        {
          if (scanner.scan(cfg, *state))
          {
            if (!state->results.empty())
            {
              std::vector<BoltCore::OutputInfo> newOutputs;
              newOutputs.reserve(state->results.size());
              for (const auto &fo : state->results)
              {
                BoltCore::OutputInfo info;
                info.blockHeight = fo.blockHeight;
                info.txHash = fo.txHash;
                info.outputIndex = fo.outputIndex;
                info.globalOutputIndex = fo.outputIndex;
                info.amount = fo.amount;
                info.outputKey = fo.outputKey;
                info.txPublicKey = fo.txPublicKey;
                info.keyImage = fo.keyImage;
                info.spent = fo.spent;
                info.isDeposit = fo.isDeposit;
                info.term = fo.term;
                newOutputs.push_back(std::move(info));
              }
              m_onNewOutputs(newOutputs, nodeHeight);
            }
            m_lastScannedHeight.store(nodeHeight, std::memory_order_relaxed);
          }
        }
        catch (const std::exception &e)
        {
          std::cerr << "SyncMonitor: scan failed with exception: " << e.what() << std::endl;
        }
        // state destructor called here, but ProgressWriter::stop() already joined threads
      }
    }
  }
} // namespace BoltRPC