#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "BoltSync.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "Storage/IBlockchainStorage.h"

namespace BoltSync
{
  struct ScanContext
  {
    CryptoNote::IBlockchainStorage &storage;
    const crypto::SecretKey &viewKey;
    const crypto::PublicKey &viewPublicKey;
    const crypto::SecretKey *spendKey;
    std::atomic<uint64_t> &blocksProcessed;
    std::atomic<uint32_t> &lastCheckpointHeight;
    std::mutex &resultsMutex;
    std::vector<FoundOutput> &results;
    std::function<void(uint32_t)> saveCheckpoint;
  };

  bool deserializeBlockEntry(const cn::BinaryArray &rawEntry,
                             cn::Block &block,
                             std::vector<cn::Transaction> &transactions);

  void scanSingleBlock(uint32_t height, ScanContext &ctx);
}