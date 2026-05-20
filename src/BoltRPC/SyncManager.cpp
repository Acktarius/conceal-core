// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#include "SyncManager.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "INode.h"
#include "Rpc/HttpClient.h"

#include <algorithm>
#include <cstring>
#include <future>
#include <fstream>
#include <sstream>

using namespace cn;

namespace BoltRPC
{

  // ─── JSON helpers (no external dependency) ─────────────────────────────────

  namespace
  {

    std::string jsonEscape(const std::string &s)
    {
      std::string out;
      out.reserve(s.size() + 4);
      for (char c : s)
      {
        if (c == '"' || c == '\\')
          out += '\\';
        out += c;
      }
      return out;
    }

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

    bool jsonGetUint64(const std::string &json, const std::string &key, uint64_t &out)
    {
      std::string search = "\"" + key + "\":";
      size_t pos = json.find(search);
      if (pos == std::string::npos)
        return false;
      pos += search.size();
      while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n'))
        ++pos;
      char *end = nullptr;
      out = std::strtoull(json.c_str() + pos, &end, 10);
      return end != json.c_str() + pos;
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

    std::vector<std::string> jsonGetStringArray(const std::string &json, const std::string &key)
    {
      std::vector<std::string> result;
      std::string search = "\"" + key + "\":[";
      size_t pos = json.find(search);
      if (pos == std::string::npos)
        return result;
      pos += search.size();
      while (pos < json.size())
      {
        if (json[pos] == ']')
          break;
        if (json[pos] == ',' || json[pos] == ' ' || json[pos] == '\n' || json[pos] == '\t')
        {
          ++pos;
          continue;
        }
        if (json[pos] == '"')
        {
          size_t start = pos + 1;
          size_t end = start;
          while (end < json.size() && json[end] != '"')
            ++end;
          if (end < json.size())
          {
            result.push_back(json.substr(start, end - start));
            pos = end + 1;
            continue;
          }
        }
        ++pos;
      }
      return result;
    }

    // Extract the content of a JSON array (raw text between [ and ], handles nesting)
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

    // Split JSON array of objects into individual object strings
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

    OutputInfo parseOutputObject(const std::string &obj)
    {
      OutputInfo info;
      std::memset(&info, 0, sizeof(info));

      jsonGetUint(obj, "height", info.blockHeight);
      jsonGetUint64(obj, "amount", info.amount);
      jsonGetUint(obj, "output_index", info.outputIndex);

      std::string txHashHex = jsonGetString(obj, "tx_hash");
      if (!txHashHex.empty())
        common::podFromHex(txHashHex, info.txHash);

      std::string outputKeyHex = jsonGetString(obj, "output_key");
      if (!outputKeyHex.empty())
        common::podFromHex(outputKeyHex, info.outputKey);

      std::string txPubkeyHex = jsonGetString(obj, "tx_pubkey");
      if (!txPubkeyHex.empty())
        common::podFromHex(txPubkeyHex, info.txPublicKey);

      return info;
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
    loadCachedKeys();
  }

  SyncManager::~SyncManager()
  {
    stop();
    saveCachedKeys();
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

    // ── Determine if this is first sync ────────────────────────────────────
    {
      std::lock_guard<std::mutex> lock(m_keysMutex);
      if (m_knownTxPubKeys.empty())
      {
        doBootstrap(progress);
      }
    }

    // ── Background incremental loop ────────────────────────────────────────
    while (!m_stop.load())
    {
      // Wait for poll interval or manual trigger
      for (uint32_t i = 0; i < POLL_INTERVAL_SECONDS * 2 && !m_stop.load() && !m_triggerSync.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

      if (m_stop.load())
        break;

      doIncrementalSync(progress);

      m_triggerSync.store(false);
    }
  }

  // ─── Bootstrap Sync ────────────────────────────────────────────────────────

  void SyncManager::doBootstrap(SyncProgress &progress)
  {
    // Step 1: Fetch all tx_pub_keys from daemon
    progress = SyncProgress();
    progress.phase = SyncProgress::FETCHING_KEYS;
    if (m_onProgress)
      m_onProgress(progress);

    std::vector<crypto::PublicKey> allKeys;
    uint32_t chainHeight = 0;

    {
      std::vector<OutputInfo> dummyOutputs;
      std::unordered_set<std::string> dummySpent;
      std::vector<crypto::PublicKey> newKeys;

      // Empty tx_pub_keys → daemon returns all keys from tx_pubkey_seen index
      std::vector<crypto::PublicKey> emptyKeys;
      if (!callGetWalletSnapshot(emptyKeys, 0, dummyOutputs, dummySpent, newKeys, chainHeight))
      {
        progress.errorMessage = "Failed to fetch tx_pub_keys from daemon";
        progress.phase = SyncProgress::COMPLETE;
        if (m_onProgress)
          m_onProgress(progress);
        return;
      }

      allKeys = std::move(newKeys);
      progress.totalKeys = static_cast<uint32_t>(allKeys.size());
      progress.currentHeight = chainHeight;
    }

    if (m_stop.load())
      return;
    if (m_onProgress)
      m_onProgress(progress);

    // Cache keys now so we don't lose them if interrupted
    {
      std::lock_guard<std::mutex> lock(m_keysMutex);
      m_knownTxPubKeys = allKeys;
    }
    saveCachedKeys();

    // Step 2: Fetch all candidate outputs
    progress.phase = SyncProgress::FETCHING_OUTPUTS;
    if (m_onProgress)
      m_onProgress(progress);

    std::vector<OutputInfo> allOutputs;
    std::unordered_set<std::string> allSpentImages;
    std::vector<crypto::PublicKey> additionalKeys;

    if (!callGetWalletSnapshot(allKeys, 0, allOutputs, allSpentImages, additionalKeys, chainHeight))
    {
      progress.errorMessage = "Failed to fetch outputs from daemon";
      progress.phase = SyncProgress::COMPLETE;
      if (m_onProgress)
        m_onProgress(progress);
      return;
    }

    progress.totalOutputs = static_cast<uint32_t>(allOutputs.size());
    progress.currentHeight = chainHeight;
    m_lastScannedHeight.store(chainHeight);

    if (!additionalKeys.empty())
    {
      std::lock_guard<std::mutex> lock(m_keysMutex);
      for (const auto &pk : additionalKeys)
      {
        if (std::find(m_knownTxPubKeys.begin(), m_knownTxPubKeys.end(), pk) == m_knownTxPubKeys.end())
          m_knownTxPubKeys.push_back(pk);
      }
      saveCachedKeys();
    }

    if (m_stop.load())
      return;
    if (m_onProgress)
      m_onProgress(progress);

    // Step 3: Derive ownership locally
    progress.phase = SyncProgress::DERIVING;
    if (m_onProgress)
      m_onProgress(progress);

    std::vector<OutputInfo> ownedOutputs;
    deriveOwnedOutputs(allOutputs, allSpentImages, ownedOutputs);

    progress.processedOutputs = progress.totalOutputs;
    progress.ownedOutputs = static_cast<uint32_t>(ownedOutputs.size());
    progress.phase = SyncProgress::COMPLETE;
    if (m_onProgress)
      m_onProgress(progress);

    // Step 4: Report to wallet
    if (m_onOutputs && !ownedOutputs.empty())
    {
      std::vector<crypto::KeyImage> empty;
      m_onOutputs(ownedOutputs, empty);
    }
  }

  // ─── Incremental Sync ──────────────────────────────────────────────────────

  void SyncManager::doIncrementalSync(SyncProgress &progress)
  {
    progress = SyncProgress();
    progress.phase = SyncProgress::INCREMENTAL;

    std::vector<crypto::PublicKey> keys;
    {
      std::lock_guard<std::mutex> lock(m_keysMutex);
      keys = m_knownTxPubKeys;
    }

    uint32_t walletHeight = m_lastScannedHeight.load();

    std::vector<OutputInfo> newOutputs;
    std::unordered_set<std::string> spentImages;
    std::vector<crypto::PublicKey> newKeys;
    uint32_t chainHeight = 0;

    if (!callGetWalletSnapshot(keys, walletHeight, newOutputs, spentImages, newKeys, chainHeight))
    {
      // Silently retry on next poll
      return;
    }

    progress.totalOutputs = static_cast<uint32_t>(newOutputs.size());
    progress.currentHeight = chainHeight;

    if (!newOutputs.empty())
    {
      std::vector<OutputInfo> owned;
      deriveOwnedOutputs(newOutputs, spentImages, owned);

      progress.processedOutputs = progress.totalOutputs;
      progress.ownedOutputs = static_cast<uint32_t>(owned.size());

      if (m_onOutputs && !owned.empty())
      {
        std::vector<crypto::KeyImage> empty;
        m_onOutputs(owned, empty);
      }
    }

    // Add new keys to cache
    if (!newKeys.empty())
    {
      std::lock_guard<std::mutex> lock(m_keysMutex);
      bool changed = false;
      for (const auto &pk : newKeys)
      {
        if (std::find(m_knownTxPubKeys.begin(), m_knownTxPubKeys.end(), pk) == m_knownTxPubKeys.end())
        {
          m_knownTxPubKeys.push_back(pk);
          changed = true;
        }
      }
      if (changed)
        saveCachedKeys();
    }

    m_lastScannedHeight.store(chainHeight);
    progress.phase = SyncProgress::COMPLETE;
    if (m_onProgress)
      m_onProgress(progress);
  }

  // ─── Daemon RPC Call ───────────────────────────────────────────────────────

  bool SyncManager::callGetWalletSnapshot(
      const std::vector<crypto::PublicKey> &txPubKeys,
      uint32_t walletHeight,
      std::vector<OutputInfo> &outputs,
      std::unordered_set<std::string> &spentKeyImages,
      std::vector<crypto::PublicKey> &newTxPubKeys,
      uint32_t &currentHeight)
  {
    // Build JSON-RPC params
    std::ostringstream paramsJson;
    paramsJson << "{";
    paramsJson << R"("tx_pub_keys":[)";
    for (size_t i = 0; i < txPubKeys.size(); ++i)
    {
      if (i > 0)
        paramsJson << ",";
      paramsJson << R"(")" << common::podToHex(txPubKeys[i]) << R"(")";
    }
    paramsJson << R"(],"wallet_height":)" << walletHeight;
    paramsJson << "}";

    // Build full JSON-RPC request
    std::ostringstream requestBody;
    requestBody << R"({"jsonrpc":"2.0","id":1,"method":"get_wallet_snapshot","params":)"
                << paramsJson.str() << "}";

    // Send to daemon via HTTP POST to /json_rpc
    if (!m_rpcCallback)
      return false;

    std::string response = m_rpcCallback("get_wallet_snapshot", paramsJson.str());

    if (response.empty())
      return false;

    // Parse result
    // Extract "result" object from JSON-RPC response
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
        // Maybe the response IS the result (no JSON-RPC wrapper)
        resultJson = response;
      }
    }

    // Parse fields
    jsonGetUint(resultJson, "current_height", currentHeight);

    // Parse outputs
    std::string outputsArr = jsonExtractArray(resultJson, "outputs");
    std::vector<std::string> outputObjs = jsonSplitObjects(outputsArr);
    outputs.reserve(outputObjs.size());
    for (const auto &obj : outputObjs)
      outputs.push_back(parseOutputObject(obj));

    // Parse spent_key_images
    spentKeyImages.clear();
    std::vector<std::string> spentArr = jsonGetStringArray(resultJson, "spent_key_images");
    for (const auto &kiHex : spentArr)
      spentKeyImages.insert(kiHex);

    // Parse new_tx_pub_keys
    std::vector<std::string> newKeysArr = jsonGetStringArray(resultJson, "new_tx_pub_keys");
    newTxPubKeys.clear();
    newTxPubKeys.reserve(newKeysArr.size());
    for (const auto &pkHex : newKeysArr)
    {
      crypto::PublicKey pk;
      if (common::podFromHex(pkHex, pk))
        newTxPubKeys.push_back(pk);
    }

    return true;
  }

  // ─── Local Derivation ──────────────────────────────────────────────────────

  bool SyncManager::isOurOutput(const OutputInfo &candidate) const
  {
    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(candidate.txPublicKey, m_viewSecretKey, derivation))
      return false;

    crypto::PublicKey derivedKey;
    if (!crypto::derive_public_key(derivation, candidate.outputIndex, m_spendPublicKey, derivedKey))
      return false;

    return std::memcmp(&derivedKey, &candidate.outputKey, sizeof(crypto::PublicKey)) == 0;
  }

  crypto::KeyImage SyncManager::deriveKeyImage(const OutputInfo &output) const
  {
    crypto::KeyImage ki;
    std::memset(&ki, 0, sizeof(ki));

    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(output.txPublicKey, m_viewSecretKey, derivation))
      return ki;

    crypto::PublicKey ephemeralPub;
    if (!crypto::derive_public_key(derivation, output.outputIndex, m_spendPublicKey, ephemeralPub))
      return ki;

    // derive_public_key with spend secret gives us the output secret key equivalent
    // But we only have the spend PUBLIC key, not the secret.
    // To derive a key_image we need the spend SECRET key.
    // If we only have the spend public key, we cannot derive key images locally.
    // The wallet must provide the spend secret key for full spent detection.

    // For now, return empty key image — spent detection requires the spend secret.
    return ki;
  }

  void SyncManager::deriveOwnedOutputs(
      const std::vector<OutputInfo> &candidates,
      const std::unordered_set<std::string> &spentKeyImages,
      std::vector<OutputInfo> &owned)
  {
    owned.clear();
    owned.reserve(std::min(candidates.size(), size_t(10000)));

    size_t batchSize = std::max(size_t(1), candidates.size() / std::thread::hardware_concurrency());
    std::mutex ownedMutex;

    auto processBatch = [&](size_t start, size_t end)
    {
      std::vector<OutputInfo> localOwned;
      for (size_t i = start; i < end && i < candidates.size(); ++i)
      {
        if (m_stop.load())
          break;

        const auto &candidate = candidates[i];
        if (isOurOutput(candidate))
        {
          OutputInfo ownedOutput = candidate;
          ownedOutput.spent = false;
          localOwned.push_back(ownedOutput);

          // Track this tx_pub_key — it produced one of our outputs
          addKnownKey(candidate.txPublicKey);
        }
      }

      if (!localOwned.empty())
      {
        std::lock_guard<std::mutex> lock(ownedMutex);
        owned.insert(owned.end(), localOwned.begin(), localOwned.end());
      }
    };

    std::vector<std::future<void>> futures;
    for (size_t i = 0; i < candidates.size(); i += batchSize)
    {
      size_t end = std::min(i + batchSize, candidates.size());
      futures.push_back(std::async(std::launch::async, processBatch, i, end));
    }

    for (auto &f : futures)
      f.get();

    std::sort(owned.begin(), owned.end(),
              [](const OutputInfo &a, const OutputInfo &b)
              { return a.blockHeight < b.blockHeight; });

    // Save keys after derivation (batch save)
    saveCachedKeys();
  }

  // ─── Persistence ────────────────────────────────────────────────────────────

  std::string SyncManager::keysCachePath() const
  {
    return m_dataDir + "/tx_pub_keys_cache.bin";
  }

  void SyncManager::loadCachedKeys()
  {
    std::ifstream file(keysCachePath(), std::ios::binary);
    if (!file.is_open())
      return;

    uint32_t count = 0;
    file.read(reinterpret_cast<char *>(&count), sizeof(count));
    if (!file || count == 0 || count > 1000000) // sanity: max 1M keys = 32 MB
      return;

    std::lock_guard<std::mutex> lock(m_keysMutex);
    m_knownTxPubKeys.resize(count);
    file.read(reinterpret_cast<char *>(m_knownTxPubKeys.data()), count * sizeof(crypto::PublicKey));

    // Also load last scanned height
    uint32_t height = 0;
    file.read(reinterpret_cast<char *>(&height), sizeof(height));
    if (file)
      m_lastScannedHeight.store(height);
  }

  void SyncManager::saveCachedKeys()
  {
    std::lock_guard<std::mutex> lock(m_keysMutex);

    std::ofstream file(keysCachePath(), std::ios::binary | std::ios::trunc);
    if (!file.is_open())
      return;

    uint32_t count = static_cast<uint32_t>(m_knownTxPubKeys.size());
    file.write(reinterpret_cast<const char *>(&count), sizeof(count));
    if (!m_knownTxPubKeys.empty())
      file.write(reinterpret_cast<const char *>(m_knownTxPubKeys.data()),
                 m_knownTxPubKeys.size() * sizeof(crypto::PublicKey));

    uint32_t height = m_lastScannedHeight.load();
    file.write(reinterpret_cast<const char *>(&height), sizeof(height));
  }

  // Called by deriveOwnedOutputs when an output is ours
  void SyncManager::addKnownKey(const crypto::PublicKey &txPubKey)
  {
    std::lock_guard<std::mutex> lock(m_keysMutex);
    if (std::find(m_knownTxPubKeys.begin(), m_knownTxPubKeys.end(), txPubKey) == m_knownTxPubKeys.end())
    {
      m_knownTxPubKeys.push_back(txPubKey);
      // Don't save to disk immediately — batch save on shutdown or periodically
    }
  }

} // namespace BoltRPC