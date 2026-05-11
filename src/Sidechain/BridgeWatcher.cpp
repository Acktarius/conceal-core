// BridgeWatcher implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BridgeWatcher.h"
#include "SidechainConfig.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/Account.h"
#include "CryptoNoteCore/TransactionExtra.h"
#include "Common/StringTools.h"
#include "BoltSync/CryptoHelpers.h"
#include "NodeRpcProxy/NodeRpcProxy.h"

#include "Rpc/HttpClient.h"
#include <System/Dispatcher.h>

#include <chrono>
#include <iostream>

namespace Sidechain
{
  BridgeWatcher::BridgeWatcher(SidechainStorage &storage,
                               cn::INode &node,
                               const crypto::PublicKey &bridgeViewPub,
                               const crypto::SecretKey &bridgeViewKey,
                               const crypto::PublicKey &bridgeSpendPub,
                               const crypto::SecretKey &bridgeSpendKey)
      : m_storage(storage),
        m_node(node),
        m_bridgeViewPub(bridgeViewPub),
        m_bridgeViewKey(bridgeViewKey),
        m_bridgeSpendPub(bridgeSpendPub),
        m_bridgeSpendKey(bridgeSpendKey),
        m_hasSpendKey(true)
  {
  }

  BridgeWatcher::~BridgeWatcher()
  {
    stop();
  }

  void BridgeWatcher::start(const DepositCallback &onDeposit)
  {
    if (m_running)
      return;
    m_running = true;
    m_watchThread = std::thread(&BridgeWatcher::watchLoop, this, onDeposit);
    if (m_hasSpendKey)
      m_unlockThread = std::thread(&BridgeWatcher::unlockLoop, this);
  }

  void BridgeWatcher::stop()
  {
    m_running = false;
    if (m_watchThread.joinable())
      m_watchThread.join();
    if (m_unlockThread.joinable())
      m_unlockThread.join();
  }

  uint64_t BridgeWatcher::getLockedAmount(uint64_t tokenId) const
  {
    std::lock_guard<std::mutex> lock(m_lockedAmountsMutex);
    auto it = m_lockedAmounts.find(tokenId);
    return it != m_lockedAmounts.end() ? it->second : 0;
  }

  void BridgeWatcher::handleBurn(const Transaction &burnTx)
  {
    uint64_t remainingToUnlock = burnTx.amount;
    uint64_t lockId = 1;
    BridgeLockEntry lockEntry;

    while (remainingToUnlock > 0 && m_storage.getBridgeLock(lockId, lockEntry))
    {
      if (!lockEntry.unlocked &&
          lockEntry.tokenId == burnTx.tokenId &&
          lockEntry.userAddress == burnTx.from)
      {
        uint64_t unlockAmount = std::min(remainingToUnlock, lockEntry.amount);

        {
          std::lock_guard<std::mutex> lock(m_unlockMutex);
          PendingUnlock pending;
          pending.userAddress = burnTx.from;
          pending.amount = unlockAmount;
          pending.burnTxHash = burnTx.txHash;
          pending.tokenId = burnTx.tokenId;
          m_pendingUnlocks.push(pending);
        }

        if (unlockAmount == lockEntry.amount)
        {
          m_storage.markBridgeLockUnlocked(lockId);
        }

        remainingToUnlock -= unlockAmount;
        std::cout << "BridgeWatcher: queued unlock of " << unlockAmount
                  << " CCX to " << common::podToHex(burnTx.from).substr(0, 16)
                  << " (burn tx: " << common::podToHex(burnTx.txHash).substr(0, 16) << ")"
                  << std::endl;
      }
      ++lockId;
    }
  }

  void BridgeWatcher::processUnlocks()
  {
    if (!m_hasSpendKey)
      return;

    std::lock_guard<std::mutex> lock(m_unlockMutex);
    while (!m_pendingUnlocks.empty())
    {
      PendingUnlock pending = m_pendingUnlocks.front();
      m_pendingUnlocks.pop();

      m_unlockMutex.unlock();
      bool success = submitUnlockTransaction(pending.userAddress, pending.amount, pending.burnTxHash);
      m_unlockMutex.lock();

      if (!success)
      {
        m_pendingUnlocks.push(pending);
        break;
      }
    }
  }

  size_t BridgeWatcher::getPendingUnlockCount() const
  {
    std::lock_guard<std::mutex> lock(m_unlockMutex);
    return m_pendingUnlocks.size();
  }

  void BridgeWatcher::watchLoop(const DepositCallback &onDeposit)
  {
    uint32_t lastScannedHeight = 0;

    while (m_running)
    {
      try
      {
        uint32_t currentHeight = m_node.getLastLocalBlockHeight();

        if (currentHeight > lastScannedHeight)
        {
          BoltSync::Scanner scanner(m_bridgeViewKey, m_bridgeViewPub, nullptr);
          BoltSync::ScanConfig scanCfg;
          scanCfg.startBlock = lastScannedHeight + 1;
          scanCfg.endBlock = currentHeight;

          BoltSync::ScanState state;
          if (scanner.scan(scanCfg, state))
          {
            for (const auto &output : state.results)
            {
              // Extract sidechain destination from transaction extra
              // The output struct carries the txPublicKey and txExtra if available
              std::string sidechainDestHex;

              // Try to get the destination from the output's metadata
              // The bridge deposit format: tx_extra contains "bridge:<64-char-pubkey-hex>"
              if (!output.txExtra.empty())
              {
                sidechainDestHex = extractSidechainDestination(output.txExtra);
              }

              // Fallback: use the output's public key as destination
              // This works when the user sends directly to the bridge without extra data
              if (sidechainDestHex.empty())
              {
                sidechainDestHex = common::podToHex(output.outputKey);
              }

              // Create a Mint transaction
              Transaction depositTx;
              depositTx.type = TransactionType::Mint;
              depositTx.from = m_bridgeSpendPub;
              depositTx.amount = output.amount;
              depositTx.txHash = output.txHash;
              depositTx.fee = SidechainConfig::MINIMUM_FEE;
              depositTx.feeTokenId = 0; // SCCX
              depositTx.timestamp = static_cast<uint64_t>(std::time(nullptr));

              // Set the destination
              crypto::PublicKey destPub;
              if (common::podFromHex(sidechainDestHex, destPub))
              {
                depositTx.to = destPub;
              }
              else
              {
                // If we can't determine the destination, skip this deposit
                std::cout << "BridgeWatcher: could not determine destination for deposit, skipping"
                          << std::endl;
                continue;
              }

              // Look up existing CCX-backed token or flag for new creation
              uint64_t assetTokenId = 0;
              bool isNewAsset = false;
              if (!m_storage.getAssetBySource("conceal", "native", m_bridgeSpendPub, assetTokenId))
              {
                // No CCX-backed token exists yet — flag for new asset creation
                isNewAsset = true;
                assetTokenId = 0; // The Mint handler will create a new token
              }
              depositTx.tokenId = assetTokenId;

              // Store main chain tx hash for audit trail
              std::string txHashHex = common::podToHex(output.txHash);
              std::string combinedExtra = txHashHex + ":" + sidechainDestHex;
              if (isNewAsset)
                combinedExtra += ":new_asset";
              depositTx.extra.assign(combinedExtra.begin(), combinedExtra.end());

              std::cout << "BridgeWatcher: detected deposit of " << output.amount
                        << " CCX from main chain tx " << txHashHex.substr(0, 16)
                        << "... destination: " << sidechainDestHex.substr(0, 16)
                        << "..." << std::endl;

              if (onDeposit)
                onDeposit(depositTx);

              // Update locked amount tracking
              {
                std::lock_guard<std::mutex> lock(m_lockedAmountsMutex);
                m_lockedAmounts[assetTokenId] += output.amount;
              }
            }
          }

          lastScannedHeight = currentHeight;
        }
      }
      catch (const std::exception &e)
      {
        std::cout << "BridgeWatcher: error in watch loop: " << e.what() << std::endl;
      }
      catch (...)
      {
        // Ignore errors, retry on next iteration
      }

      // Poll every 5 seconds
      for (int i = 0; i < 50 && m_running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  void BridgeWatcher::unlockLoop()
  {
    while (m_running)
    {
      processUnlocks();
      // Check every 2 seconds
      for (int i = 0; i < 20 && m_running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  bool BridgeWatcher::submitUnlockTransaction(const crypto::PublicKey &toAddress,
                                              uint64_t amount,
                                              const crypto::Hash &burnTxHash)
  {
    if (!m_hasSpendKey)
    {
      std::cout << "BridgeWatcher: no spend key, cannot submit unlock" << std::endl;
      return false;
    }

    try
    {
      std::cout << "BridgeWatcher: building unlock transaction: "
                << amount << " CCX > "
                << common::podToHex(toAddress).substr(0, 16) << "..."
                << std::endl;

      // Scan bridge wallet for available outputs
      BoltSync::Scanner scanner(m_bridgeViewKey, m_bridgeViewPub, &m_bridgeSpendKey);
      BoltSync::ScanConfig scanCfg;
      scanCfg.startBlock = 0;
      scanCfg.endBlock = m_node.getLastLocalBlockHeight();
      scanCfg.numThreads = 1;

      BoltSync::ScanState scanState;
      if (!scanner.scan(scanCfg, scanState))
      {
        std::cout << "BridgeWatcher: failed to scan bridge wallet outputs" << std::endl;
        return false;
      }

      if (scanState.results.empty())
      {
        std::cout << "BridgeWatcher: no bridge wallet outputs available" << std::endl;
        return false;
      }

      // Select outputs to cover the unlock amount
      uint64_t accumulatedAmount = 0;
      std::vector<BoltSync::FoundOutput> selectedOutputs;

      for (const auto &output : scanState.results)
      {
        if (!output.spent)
        {
          selectedOutputs.push_back(output);
          accumulatedAmount += output.amount;

          if (accumulatedAmount >= amount)
            break;
        }
      }

      if (accumulatedAmount < amount)
      {
        std::cout << "BridgeWatcher: insufficient bridge wallet balance. "
                  << "Have: " << accumulatedAmount << " Need: " << amount << std::endl;
        return false;
      }

      // Build the unlock transaction
      cn::Transaction tx;
      tx.version = cn::TRANSACTION_VERSION_1;
      tx.unlockTime = 0;

      // Add inputs from selected outputs
      for (const auto &output : selectedOutputs)
      {
        cn::KeyInput input;
        input.amount = output.amount;
        input.keyImage = output.keyImage;
        input.outputIndexes.push_back(output.outputIndex);
        tx.inputs.push_back(input);
      }

      // Add output to user
      cn::TransactionOutput txOut;
      txOut.amount = amount;
      cn::KeyOutput keyOut;
      keyOut.key = toAddress;
      txOut.target = keyOut;
      tx.outputs.push_back(txOut);

      // Add change output back to bridge if needed
      uint64_t change = accumulatedAmount - amount;
      if (change > 0)
      {
        cn::TransactionOutput changeOut;
        changeOut.amount = change;
        cn::KeyOutput changeKeyOut;
        changeKeyOut.key = m_bridgeSpendPub;
        changeOut.target = changeKeyOut;
        tx.outputs.push_back(changeOut);
      }

      // Sign the transaction
      crypto::Hash txPrefixHash = cn::getObjectHash(
          *static_cast<const cn::TransactionPrefix *>(&tx));

      for (size_t i = 0; i < selectedOutputs.size(); ++i)
      {
        const auto &output = selectedOutputs[i];

        // Derive the ephemeral secret key for this output
        crypto::KeyDerivation derivation;
        crypto::generate_key_derivation(
            output.txPublicKey, m_bridgeViewKey, derivation);

        crypto::SecretKey ephemeralSecretKey;
        crypto::derive_secret_key(
            derivation, output.outputIndex, m_bridgeSpendKey, ephemeralSecretKey);

        // Build ring with just this output (1-member ring)
        std::vector<const crypto::PublicKey *> ring;
        ring.push_back(&output.outputKey);

        // Generate ring signature
        std::vector<crypto::Signature> sigs(1);
        crypto::generate_ring_signature(
            txPrefixHash,
            output.keyImage,
            ring,
            ephemeralSecretKey,
            0,
            sigs.data());

        tx.signatures.push_back(sigs);
      }

      // Submit to daemon via /sendrawtransaction endpoint
      cn::BinaryArray txBytes = cn::toBinaryArray(tx);
      std::string txHex = common::toHex(txBytes);

      std::cout << "BridgeWatcher: unlock transaction built: "
                << amount << " CCX > " << common::podToHex(toAddress).substr(0, 16)
                << " (change: " << change << " CCX)"
                << " | size: " << txBytes.size() << " bytes"
                << std::endl;

      // The daemon's /sendrawtransaction endpoint accepts:
      // {"tx_as_hex": "<hex-encoded-transaction>"}
      cn::HttpRequest req;
      cn::HttpResponse res;

      std::string jsonBody = R"({"tx_as_hex":")" + txHex + R"("})";

      req.setUrl("/sendrawtransaction");
      req.setBody(jsonBody);
      req.addHeader("Content-Type", "application/json");

      platform_system::Dispatcher dispatcher;
      cn::HttpClient httpClient(dispatcher, "127.0.0.1", 16000);

      try
      {
        httpClient.request(req, res);
        std::string responseBody = res.getBody();

        std::cout << "BridgeWatcher: daemon response: " << responseBody << std::endl;

        // The daemon returns: {"status":"OK"} or {"status":"Failed"}
        if (responseBody.find("\"status\":\"OK\"") != std::string::npos)
        {
          std::cout << "BridgeWatcher: unlock transaction submitted successfully"
                    << " (burn tx: " << common::podToHex(burnTxHash).substr(0, 16) << ")"
                    << std::endl;
          return true;
        }
        else
        {
          std::cout << "BridgeWatcher: daemon rejected transaction: " << responseBody << std::endl;
          return false;
        }
      }
      catch (const std::exception &e)
      {
        std::cout << "BridgeWatcher: failed to submit to daemon: " << e.what() << std::endl;
        return false;
      }
    }
    catch (const std::exception &e)
    {
      std::cout << "BridgeWatcher: failed to submit unlock: " << e.what() << std::endl;
      return false;
    }
  }

  std::string BridgeWatcher::extractSidechainDestination(const std::vector<uint8_t> &extra) const
  {
    // Parse "bridge:<64-char-hex>" from transaction extra
    std::string extraStr(extra.begin(), extra.end());
    const std::string prefix = "bridge:";
    size_t pos = extraStr.find(prefix);
    if (pos != std::string::npos)
    {
      std::string dest = extraStr.substr(pos + prefix.length(), 64);
      if (dest.size() == 64 &&
          dest.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
      {
        return dest;
      }
    }
    return "";
  }
}