// BoltSync - Multi-threaded MDBX blockchain scanner
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "crypto/crypto.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"

namespace BoltSync
{
  struct FoundOutput
  {
    uint32_t blockHeight;
    crypto::Hash txHash;
    uint32_t outputIndex;
    uint64_t amount;
    crypto::PublicKey outputKey;
    crypto::PublicKey txPublicKey;
    crypto::KeyImage keyImage;
    bool spent = false;
  };

  struct ScanConfig
  {
    std::string dataDir;
    unsigned int numThreads = 0;
    uint32_t startBlock = 0;
    uint32_t endBlock = 0;
    std::string progressFile;
  };

  struct ScanState
  {
    std::atomic<uint64_t> blocksProcessed{0};
    std::atomic<uint32_t> lastCheckpointHeight{0};
    std::atomic<bool> progressDone{false};
    std::mutex resultsMutex;
    std::vector<FoundOutput> results;
  };

  class Scanner
  {
  public:
    Scanner(const crypto::SecretKey &viewKey,
            const crypto::PublicKey &viewPublicKey,
            const crypto::SecretKey *spendKey);
    ~Scanner();

    bool scan(const ScanConfig &config, ScanState &state);

  private:
    const crypto::SecretKey &m_viewKey;
    const crypto::PublicKey &m_viewPublicKey;
    const crypto::SecretKey *m_spendKey;
  };
}