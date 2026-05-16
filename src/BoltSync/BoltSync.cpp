#include "BoltSync.h"
#include "BlockDeserializer.h"
#include "CryptoHelpers.h"
#include "ProgressWriter.h"
#include "Common/PathHelpers.h"

#include "Storage/MDBXBlockchainStorage.h"
#include "Common/Util.h"

#include <thread>

namespace BoltSync
{
  Scanner::Scanner(const crypto::SecretKey &viewKey,
                   const crypto::PublicKey &spendPublicKey,
                   const crypto::SecretKey *spendKey)
      : m_viewKey(viewKey), m_spendPublicKey(spendPublicKey), m_spendKey(spendKey) {}

  Scanner::~Scanner() {}

  bool Scanner::scan(const ScanConfig &config, ScanState &state)
  {
    std::string dbPath = PathHelpers::appendPath(config.dataDir, "mdbx_blocks");
    CryptoNote::MDBXBlockchainStorage storage(dbPath, 0);

    uint32_t topHeight = storage.topBlockHeight();
    if (topHeight == 0)
      return false;

    // Check for resume
    uint32_t lastScannedHeight = 0;
    {
      std::vector<uint8_t> resumeBuf;
      if (storage.getMeta("wallet_init_progress", resumeBuf) && resumeBuf.size() >= sizeof(uint32_t))
      {
        memcpy(&lastScannedHeight, resumeBuf.data(), sizeof(lastScannedHeight));
        if (lastScannedHeight >= topHeight)
          lastScannedHeight = 0;
      }
    }

    state.blocksProcessed = lastScannedHeight;
    state.lastCheckpointHeight = lastScannedHeight;

    auto saveCheckpoint = [&](uint32_t height)
    {
      std::vector<uint8_t> buf(sizeof(height));
      memcpy(buf.data(), &height, sizeof(height));
      storage.putMeta("wallet_init_progress", buf);
      storage.flush();
      state.lastCheckpointHeight.store(height, std::memory_order_relaxed);
    };

    unsigned int numThreads = config.numThreads;
    if (numThreads == 0)
    {
      numThreads = std::thread::hardware_concurrency();
      if (numThreads == 0)
        numThreads = 4;
    }
    if (numThreads > 16)
      numThreads = 16;

    // Progress writer
    ProgressWriter progress(config.progressFile, topHeight, state.blocksProcessed, state.results, lastScannedHeight);
    progress.start();

    uint32_t scanTopHeight = (config.endBlock > 0 && config.endBlock < topHeight) ? config.endBlock : topHeight;
    uint32_t scanStartHeight = std::max(config.startBlock, lastScannedHeight + 1);

    state.scannedTopHeight = scanTopHeight;

    if (scanStartHeight > scanTopHeight)
      return false;

    uint32_t blocksPerThread = (scanTopHeight - scanStartHeight + 1 + numThreads - 1) / numThreads;

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < numThreads; ++t)
    {
      uint32_t start = scanStartHeight + static_cast<uint32_t>(t * blocksPerThread);
      uint32_t end = std::min(start + blocksPerThread - 1, scanTopHeight);
      if (start > end)
        break;

      threads.emplace_back([&, start, end]()
                           {
            ScanContext ctx{ storage, m_viewKey, m_spendPublicKey, m_spendKey,
                             state.blocksProcessed, state.lastCheckpointHeight,
                             state.resultsMutex, state.results, saveCheckpoint };
            for (uint32_t h = start; h <= end; ++h)
            {
                scanSingleBlock(h, ctx);
                uint32_t reportInterval = (end - start) / 100; // every 1%
                if (reportInterval < 1000)
                    reportInterval = 1000; // minimum every 1000 blocks
                if (h % reportInterval == 0 || h == end)
                {
                    ctx.saveCheckpoint(h);
                    if (config.onProgress) config.onProgress(h);
                }
            } });
    }

    // Wait for all scan threads to complete
    for (auto &t : threads)
    {
      if (t.joinable())
        t.join();
    }

    state.progressDone = true;
    progress.stop();

    // CRITICAL: Wait for ProgressWriter threads to fully exit before continuing
    progress.waitForThreads();

    // Second pass: mark outputs as spent by scanning KeyInput keyImages.
    markSpentOutputs(storage, scanTopHeight, state.results);

    // Clear checkpoint on success
    storage.putMeta("wallet_init_progress", std::vector<uint8_t>());
    storage.flush();

    return true;
  }
}