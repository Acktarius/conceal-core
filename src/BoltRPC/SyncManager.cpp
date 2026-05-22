// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#include "SyncManager.h"
#include "BoltSync/CryptoHelpers.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/TransactionExtra.h"
#include "INode.h"
#include "Rpc/HttpClient.h"

#include <algorithm>
#include <cstring>
#include <future>
#include <fstream>
#include <set>
#include <sstream>

using namespace cn;

namespace BoltRPC
{

  // ─── JSON helpers (no external dependency) ─────────────────────────────────

  namespace
  {

    bool jsonGetUint(const std::string &json, const std::string &key, uint32_t &out)
    {
      std::string search = "\"" + key + "\":";
      size_t pos = json.find(search);
      if (pos == std::string::npos)
        return false;
      pos += search.size();
      while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n'))
        ++pos;
      char *end = nullptr;
      unsigned long val = std::strtoul(json.c_str() + pos, &end, 10);
      if (end == json.c_str() + pos)
        return false;
      out = static_cast<uint32_t>(val);
      return true;
    }

    std::string jsonGetString(const std::string &json, const std::string &key)
    {
      std::string search = "\"" + key + "\":\"";
      size_t pos = json.find(search);
      if (pos == std::string::npos)
        return "";
      pos += search.size();
      size_t end = pos;
      while (end < json.size())
      {
        if (json[end] == '"' && (end == pos || json[end - 1] != '\\'))
          break;
        ++end;
      }
      return json.substr(pos, end - pos);
    }

    std::string jsonExtractArray(const std::string &json, const std::string &key)
    {
      std::string search = "\"" + key + "\":[";
      size_t pos = json.find(search);
      if (pos == std::string::npos)
        return "";
      pos += search.size();
      size_t start = pos;
      int depth = 1;
      bool inString = false;
      while (pos < json.size() && depth > 0)
      {
        char c = json[pos];
        if (c == '"' && (pos == start || json[pos - 1] != '\\'))
          inString = !inString;
        if (!inString)
        {
          if (c == '[')
            ++depth;
          else if (c == ']')
            --depth;
        }
        ++pos;
      }
      return json.substr(start, pos - start - 1);
    }

    std::vector<std::string> jsonSplitObjects(const std::string &arrayContent)
    {
      std::vector<std::string> result;
      if (arrayContent.empty())
        return result;
      int depth = 0;
      bool inString = false;
      size_t start = 0;
      for (size_t i = 0; i < arrayContent.size(); ++i)
      {
        char c = arrayContent[i];
        if (c == '"' && (i == 0 || arrayContent[i - 1] != '\\'))
          inString = !inString;
        if (!inString)
        {
          if (c == '{')
          {
            if (depth == 0)
              start = i;
            ++depth;
          }
          else if (c == '}')
          {
            --depth;
            if (depth == 0)
              result.push_back(arrayContent.substr(start, i - start + 1));
          }
        }
      }
      return result;
    }

    // Parse a single FilterEntry from JSON object
    cn::FilterEntry parseFilterEntry(const std::string &obj)
    {
      cn::FilterEntry entry;
      std::memset(&entry, 0, sizeof(entry));

      uint32_t view_tag = 0, output_index = 0, tx_index = 0;
      jsonGetUint(obj, "view_tag", view_tag);
      jsonGetUint(obj, "output_index", output_index);
      jsonGetUint(obj, "tx_index", tx_index);

      entry.view_tag = static_cast<uint8_t>(view_tag);
      entry.output_index = static_cast<uint8_t>(output_index);
      entry.tx_index = static_cast<uint16_t>(tx_index);

      // Parse nullifier hex string
      std::string nullifierHex = jsonGetString(obj, "nullifier");
      if (!nullifierHex.empty())
      {
        // nullifier is serialized as a hex string in JSON
        common::podFromHex(nullifierHex, entry.nullifier);
      }

      return entry;
    }

    // Parse a single BlockFilterRecord from JSON object
    cn::BlockFilterRecord parseFilterRecord(const std::string &obj)
    {
      cn::BlockFilterRecord record;
      record.block_height = 0;

      jsonGetUint(obj, "block_height", record.block_height);

      std::string entriesArr = jsonExtractArray(obj, "entries");
      std::vector<std::string> entryObjs = jsonSplitObjects(entriesArr);
      record.entries.reserve(entryObjs.size());
      for (const auto &entryObj : entryObjs)
        record.entries.push_back(parseFilterEntry(entryObj));

      return record;
    }

  } // anonymous namespace

  // ─── Constructor / Destructor ──────────────────────────────────────────────

  SyncManager::SyncManager(cn::INode &node,
                           const crypto::SecretKey &viewSecretKey,
                           const crypto::PublicKey &spendPublicKey,
                           const std::string &dataDir,
                           DaemonRpcCallback rpcCallback)
      : m_node(node),
        m_viewSecretKey(viewSecretKey),
        m_spendPublicKey(spendPublicKey),
        m_dataDir(dataDir),
        m_rpcCallback(std::move(rpcCallback))
  {
    loadProgress();
  }

  SyncManager::~SyncManager()
  {
    stop();
    saveProgress();
  }

  // ─── Public API ────────────────────────────────────────────────────────────

  void SyncManager::start(ProgressCallback onProgress, OutputCallback onOutputs)
  {
    if (m_active.exchange(true))
      return;

    m_onProgress = std::move(onProgress);
    m_onOutputs = std::move(onOutputs);
    m_stop.store(false);
    m_triggerSync.store(false);
    m_thread = std::thread(&SyncManager::runLoop, this);
  }

  void SyncManager::stop()
  {
    m_stop.store(true);
    if (m_thread.joinable())
      m_thread.join();
    m_active.store(false);
  }

  void SyncManager::syncNow()
  {
    m_triggerSync.store(true);
  }

  // ─── Main Loop ─────────────────────────────────────────────────────────────

  void SyncManager::runLoop()
  {
    SyncProgress progress;

    // ── First sync: full bootstrap via filter records ──────────────────────
    if (m_lastScannedHeight.load() == 0)
    {
      doBootstrap(progress);
    }

    // ── Background incremental loop ────────────────────────────────────────
    while (!m_stop.load())
    {
      for (uint32_t i = 0; i < POLL_INTERVAL_SECONDS * 2 && !m_stop.load() && !m_triggerSync.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

      if (m_stop.load())
        break;

      doIncrementalSync(progress);
      m_triggerSync.store(false);
    }
  }

  // ─── Bootstrap Sync ────────────────────────────────────────────────────────
  //
  // 1. Download filter records for the entire chain in batches
  // 2. Run two-pass filter locally to find candidate outputs
  // 3. Fetch full blocks only for blocks that contain candidates
  // 4. Do full ECDH derivation to confirm ownership

  void SyncManager::doBootstrap(SyncProgress &progress)
  {
    progress = SyncProgress();
    progress.phase = SyncProgress::FETCHING_FILTERS;
    if (m_onProgress)
      m_onProgress(progress);

    // Step 1: Get chain height
    uint32_t chainHeight = 0;
    {
      std::vector<cn::BlockFilterRecord> dummy;
      if (!callGetFilterRecords(0, 0, dummy, chainHeight))
      {
        progress.errorMessage = "Failed to connect to daemon";
        progress.phase = SyncProgress::COMPLETE;
        if (m_onProgress)
          m_onProgress(progress);
        return;
      }
    }

    progress.totalBlocks = chainHeight;
    progress.currentHeight = chainHeight;

    if (chainHeight == 0)
    {
      progress.phase = SyncProgress::COMPLETE;
      if (m_onProgress)
        m_onProgress(progress);
      return;
    }

    // Step 2: Download all filter records in batches
    std::vector<cn::BlockFilterRecord> allRecords;
    allRecords.reserve(chainHeight + 1);

    for (uint32_t h = 0; h <= chainHeight; h += FILTER_BATCH_SIZE)
    {
      if (m_stop.load())
        return;

      uint32_t end = std::min(h + FILTER_BATCH_SIZE - 1, chainHeight);
      std::vector<cn::BlockFilterRecord> batch;
      uint32_t dummyHeight;

      if (!callGetFilterRecords(h, end, batch, dummyHeight))
      {
        progress.errorMessage = "Failed to fetch filter records at height " + std::to_string(h);
        progress.phase = SyncProgress::COMPLETE;
        if (m_onProgress)
          m_onProgress(progress);
        return;
      }

      allRecords.insert(allRecords.end(), batch.begin(), batch.end());
      progress.processedBlocks = end;
      if (m_onProgress)
        m_onProgress(progress);
    }

    if (m_stop.load())
      return;

    // Step 3: Run two-pass filter locally
    progress.phase = SyncProgress::FILTERING;
    if (m_onProgress)
      m_onProgress(progress);

    std::vector<FilterCandidate> candidates;
    runFilterPass(allRecords, candidates);

    // Free filter records — no longer needed
    allRecords.clear();
    allRecords.shrink_to_fit();

    progress.candidatesFound = static_cast<uint32_t>(candidates.size());
    if (m_onProgress)
      m_onProgress(progress);

    if (candidates.empty())
    {
      // No candidates found — wallet is empty or filters eliminated everything
      m_lastScannedHeight.store(chainHeight);
      saveProgress();
      progress.phase = SyncProgress::COMPLETE;
      if (m_onProgress)
        m_onProgress(progress);
      return;
    }

    // Step 4: Fetch full blocks for candidate heights
    progress.phase = SyncProgress::FETCHING_BLOCKS;
    if (m_onProgress)
      m_onProgress(progress);

    // Collect unique block heights that contain candidates
    std::set<uint32_t> candidateHeights;
    for (const auto &c : candidates)
      candidateHeights.insert(c.blockHeight);

    std::vector<uint32_t> heights(candidateHeights.begin(), candidateHeights.end());

    std::vector<cn::Block> blocks;
    std::vector<std::vector<cn::Transaction>> transactions;

    for (size_t i = 0; i < heights.size(); i += BLOCK_BATCH_SIZE)
    {
      if (m_stop.load())
        return;

      size_t endIdx = std::min(i + BLOCK_BATCH_SIZE, heights.size());
      std::vector<uint32_t> batch(heights.begin() + i, heights.begin() + endIdx);

      std::vector<cn::Block> batchBlocks;
      std::vector<std::vector<cn::Transaction>> batchTxs;

      if (!callGetBlocks(batch, batchBlocks, batchTxs))
      {
        progress.errorMessage = "Failed to fetch blocks for candidates";
        progress.phase = SyncProgress::COMPLETE;
        if (m_onProgress)
          m_onProgress(progress);
        return;
      }

      blocks.insert(blocks.end(), batchBlocks.begin(), batchBlocks.end());
      transactions.insert(transactions.end(), batchTxs.begin(), batchTxs.end());

      progress.processedBlocks = static_cast<uint32_t>(endIdx);
      if (m_onProgress)
        m_onProgress(progress);
    }

    if (m_stop.load())
      return;

    // Step 5: Full ECDH derivation on candidates
    progress.phase = SyncProgress::DERIVING;
    if (m_onProgress)
      m_onProgress(progress);

    std::vector<OutputInfo> owned;
    deriveFromCandidates(candidates, blocks, transactions, owned);

    progress.ownedOutputs = static_cast<uint32_t>(owned.size());
    progress.phase = SyncProgress::COMPLETE;
    if (m_onProgress)
      m_onProgress(progress);

    // Step 6: Report to wallet
    if (m_onOutputs && !owned.empty())
    {
      std::vector<crypto::KeyImage> empty;
      m_onOutputs(owned, empty);
    }

    m_lastScannedHeight.store(chainHeight);
    saveProgress();
  }

  // ─── Incremental Sync ──────────────────────────────────────────────────────

  void SyncManager::doIncrementalSync(SyncProgress &progress)
  {
    progress = SyncProgress();
    progress.phase = SyncProgress::INCREMENTAL;

    uint32_t walletHeight = m_lastScannedHeight.load();
    uint32_t chainHeight = 0;

    // Get current chain height
    {
      std::vector<cn::BlockFilterRecord> dummy;
      if (!callGetFilterRecords(0, 0, dummy, chainHeight))
        return; // Silently retry next poll
    }

    if (chainHeight <= walletHeight)
      return; // Nothing new

    progress.totalBlocks = chainHeight;
    progress.currentHeight = chainHeight;

    // Fetch new filter records
    std::vector<cn::BlockFilterRecord> newRecords;
    uint32_t startHeight = walletHeight + 1;

    for (uint32_t h = startHeight; h <= chainHeight; h += FILTER_BATCH_SIZE)
    {
      if (m_stop.load())
        return;

      uint32_t end = std::min(h + FILTER_BATCH_SIZE - 1, chainHeight);
      std::vector<cn::BlockFilterRecord> batch;
      uint32_t dummyHeight;

      if (!callGetFilterRecords(h, end, batch, dummyHeight))
        return;

      newRecords.insert(newRecords.end(), batch.begin(), batch.end());
    }

    // Run filter
    std::vector<FilterCandidate> candidates;
    runFilterPass(newRecords, candidates);

    if (candidates.empty())
    {
      m_lastScannedHeight.store(chainHeight);
      saveProgress();
      progress.phase = SyncProgress::COMPLETE;
      if (m_onProgress)
        m_onProgress(progress);
      return;
    }

    // Fetch blocks for candidates
    std::set<uint32_t> candidateHeights;
    for (const auto &c : candidates)
      candidateHeights.insert(c.blockHeight);

    std::vector<uint32_t> heights(candidateHeights.begin(), candidateHeights.end());
    std::vector<cn::Block> blocks;
    std::vector<std::vector<cn::Transaction>> transactions;

    if (!callGetBlocks(heights, blocks, transactions))
      return;

    // Derive
    std::vector<OutputInfo> owned;
    deriveFromCandidates(candidates, blocks, transactions, owned);

    progress.ownedOutputs = static_cast<uint32_t>(owned.size());
    progress.candidatesFound = static_cast<uint32_t>(candidates.size());
    progress.phase = SyncProgress::COMPLETE;
    if (m_onProgress)
      m_onProgress(progress);

    if (m_onOutputs && !owned.empty())
    {
      std::vector<crypto::KeyImage> empty;
      m_onOutputs(owned, empty);
    }

    m_lastScannedHeight.store(chainHeight);
    saveProgress();
  }

  // ─── Daemon RPC: get_filter_records ────────────────────────────────────────

  bool SyncManager::callGetFilterRecords(uint32_t startHeight, uint32_t endHeight,
                                         std::vector<cn::BlockFilterRecord> &records,
                                         uint32_t &chainHeight)
  {
    if (!m_rpcCallback)
      return false;

    // Build params
    std::ostringstream paramsJson;
    paramsJson << "{"
               << "\"start_height\":" << startHeight << ","
               << "\"end_height\":" << endHeight
               << "}";

    std::string response = m_rpcCallback("get_filter_records", paramsJson.str());
    if (response.empty())
      return false;

    // Extract result object
    std::string resultJson;
    {
      std::string search = "\"result\":";
      size_t pos = response.find(search);
      if (pos != std::string::npos)
      {
        pos += search.size();
        int depth = 0;
        bool inStr = false;
        size_t start = pos;
        while (pos < response.size())
        {
          char c = response[pos];
          if (c == '"' && (pos == start || response[pos - 1] != '\\'))
            inStr = !inStr;
          if (!inStr)
          {
            if (c == '{')
              ++depth;
            else if (c == '}')
            {
              if (depth == 0)
              {
                ++pos;
                break;
              }
              --depth;
            }
          }
          ++pos;
        }
        resultJson = response.substr(start, pos - start);
      }
      else
      {
        resultJson = response;
      }
    }

    // Parse status
    std::string status = jsonGetString(resultJson, "status");
    if (status != "OK" && !resultJson.empty())
    {
      // If we got records, parse them anyway; status field is advisory
    }

    // Parse records array
    std::string recordsArr = jsonExtractArray(resultJson, "records");
    std::vector<std::string> recordObjs = jsonSplitObjects(recordsArr);
    records.clear();
    records.reserve(recordObjs.size());
    for (const auto &obj : recordObjs)
      records.push_back(parseFilterRecord(obj));

    // Try to get current_height from response (may not be present if just fetching records)
    if (!jsonGetUint(resultJson, "current_height", chainHeight))
    {
      // Fallback: derive from the last record
      if (!records.empty())
        chainHeight = records.back().block_height;
    }

    return true;
  }

  // ─── Daemon RPC: get_blocks (by height) ────────────────────────────────────

  bool SyncManager::callGetBlocks(const std::vector<uint32_t> &heights,
                                  std::vector<cn::Block> &blocks,
                                  std::vector<std::vector<cn::Transaction>> &transactions)
  {
    blocks.clear();
    transactions.clear();

    if (heights.empty())
      return true;

    std::vector<std::vector<cn::BlockDetails>> blockDetailsVec;
    std::error_code ec;
    bool done = false;

    m_node.getBlocks(heights, blockDetailsVec, [&](std::error_code e)
                     {
      ec = e;
      done = true; });

    while (!done && !m_stop.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (ec || m_stop.load())
      return false;

    for (const auto &detailsVec : blockDetailsVec)
    {
      for (const auto &details : detailsVec)
      {
        // Convert BlockDetails → Block
        cn::Block block;
        block.majorVersion = details.majorVersion;
        block.minorVersion = details.minorVersion;
        block.timestamp = details.timestamp;
        block.previousBlockHash = details.prevBlockHash;
        block.nonce = details.nonce;
        block.baseTransaction = cn::Transaction(); // coinbase not in details

        std::vector<cn::Transaction> txs;
        for (const auto &txDetail : details.transactions)
        {
          // Convert TransactionDetails → Transaction
          cn::Transaction tx;
          // Fetch the raw transaction via sync method
          if (m_node.getTransactionSync(txDetail.hash, tx))
          {
            block.transactionHashes.push_back(txDetail.hash);
            txs.push_back(tx);
          }
        }

        blocks.push_back(block);
        transactions.push_back(std::move(txs));
      }
    }

    return !blocks.empty();
  }

  // ─── Two-pass filter ───────────────────────────────────────────────────────

  void SyncManager::runFilterPass(const std::vector<cn::BlockFilterRecord> &records,
                                  std::vector<FilterCandidate> &candidates)
  {
    candidates.clear();

    // Pre-allocate: expect ~1/256 of outputs to pass view tag
    size_t totalEntries = 0;
    for (const auto &rec : records)
      totalEntries += rec.entries.size();
    candidates.reserve(totalEntries / 256 + 100);

    for (const auto &record : records)
    {
      for (const auto &entry : record.entries)
      {
        // Compute derivation from the txPubKey in the filter entry
        crypto::KeyDerivation derivation;
        if (!crypto::generate_key_derivation(entry.txPubKey, m_viewSecretKey, derivation))
          continue;

        // Pass 1: View tag check
        uint8_t computedTag = BoltSync::computeWalletViewTag(derivation, entry.output_index);
        if (computedTag != entry.view_tag)
          continue;

        // Pass 1 passed! Store as candidate with the expected nullifier
        // for Pass 2 verification once we have the output key from the block.
        FilterCandidate candidate;
        candidate.blockHeight = record.block_height;
        candidate.txIndex = entry.tx_index;
        candidate.outputIndex = entry.output_index;
        std::memcpy(candidate.expectedNullifier, entry.nullifier, 4);
        candidates.push_back(candidate);
      }
    }
  }

  // ─── Full derivation on candidates ─────────────────────────────────────────

  void SyncManager::deriveFromCandidates(
      const std::vector<FilterCandidate> &candidates,
      const std::vector<cn::Block> &blocks,
      const std::vector<std::vector<cn::Transaction>> &transactions,
      std::vector<OutputInfo> &owned)
  {
    owned.clear();

    // Build height → (block, transactions) map
    std::unordered_map<uint32_t, std::pair<const cn::Block *, const std::vector<cn::Transaction> *>> blockMap;
    for (size_t i = 0; i < blocks.size(); ++i)
    {
      if (!blocks[i].baseTransaction.inputs.empty() &&
          blocks[i].baseTransaction.inputs[0].type() == typeid(cn::BaseInput))
      {
        uint32_t h = boost::get<cn::BaseInput>(blocks[i].baseTransaction.inputs[0]).blockIndex;
        blockMap[h] = {&blocks[i], &transactions[i]};
      }
    }

    for (const auto &candidate : candidates)
    {
      if (m_stop.load())
        break;

      auto it = blockMap.find(candidate.blockHeight);
      if (it == blockMap.end())
        continue;

      const cn::Block &block = *it->second.first;
      const std::vector<cn::Transaction> &txs = *it->second.second;

      // Get the right transaction
      const cn::Transaction *tx = nullptr;
      if (candidate.txIndex == 0)
        tx = &block.baseTransaction;
      else if (static_cast<size_t>(candidate.txIndex) - 1 < txs.size())
        tx = &txs[candidate.txIndex - 1];
      else
        continue;

      // Get the right output
      if (candidate.outputIndex >= tx->outputs.size())
        continue;

      const auto &output = tx->outputs[candidate.outputIndex];
      if (output.target.type() != typeid(cn::KeyOutput))
        continue;

      const auto &keyOut = boost::get<cn::KeyOutput>(output.target);
      crypto::PublicKey txPubKey = getTransactionPublicKeyFromExtra(tx->extra);

      if (txPubKey == NULL_PUBLIC_KEY)
        continue;

      // ── Pass 2: Nullifier check before expensive ECDH ────────────────
      uint8_t computedNullifier[4];
      BoltSync::computeWalletNullifier(keyOut.key, candidate.blockHeight, computedNullifier);
      if (std::memcmp(computedNullifier, candidate.expectedNullifier, 4) != 0)
        continue;

      // ── Full ECDH derivation ─────────────────────────────────────────
      crypto::KeyDerivation derivation;
      if (!crypto::generate_key_derivation(txPubKey, m_viewSecretKey, derivation))
        continue;

      crypto::PublicKey derivedKey;
      if (!crypto::derive_public_key(derivation, candidate.outputIndex, m_spendPublicKey, derivedKey))
        continue;

      if (derivedKey == keyOut.key)
      {
        OutputInfo info;
        info.blockHeight = candidate.blockHeight;
        info.txHash = getObjectHash(*tx);
        info.amount = output.amount;
        info.outputIndex = candidate.outputIndex;
        info.outputKey = keyOut.key;
        info.txPublicKey = txPubKey;
        info.spent = false;
        info.isDeposit = false;
        info.term = 0;
        owned.push_back(info);
      }
    }
  }

  bool SyncManager::isOurOutput(const crypto::PublicKey &txPubKey,
                                size_t outputIndex,
                                const crypto::PublicKey &outputKey) const
  {
    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(txPubKey, m_viewSecretKey, derivation))
      return false;

    crypto::PublicKey derivedKey;
    if (!crypto::derive_public_key(derivation, outputIndex, m_spendPublicKey, derivedKey))
      return false;

    return std::memcmp(&derivedKey, &outputKey, sizeof(crypto::PublicKey)) == 0;
  }

  // ─── Persistence ───────────────────────────────────────────────────────────

  std::string SyncManager::progressPath() const
  {
    return m_dataDir + "/bolt_sync_progress.bin";
  }

  void SyncManager::loadProgress()
  {
    std::ifstream file(progressPath(), std::ios::binary);
    if (!file.is_open())
      return;

    uint32_t height = 0;
    file.read(reinterpret_cast<char *>(&height), sizeof(height));
    if (file)
      m_lastScannedHeight.store(height);
  }

  void SyncManager::saveProgress()
  {
    std::ofstream file(progressPath(), std::ios::binary | std::ios::trunc);
    if (!file.is_open())
      return;

    uint32_t height = m_lastScannedHeight.load();
    file.write(reinterpret_cast<const char *>(&height), sizeof(height));
  }

} // namespace BoltRPC