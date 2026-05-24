// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include "pow_service.hpp"

#include <cstring>
#include <sstream>

#include "../CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "../CryptoNoteCore/Difficulty.h"
#include "pow_sync_log.hpp"

namespace cn
{

PowService& PowService::instance()
{
  static PowService inst;
  return inst;
}

void PowService::init(std::unique_ptr<PowVerifyBackend> backend, const GpuPowConfig& gpuConfig,
                      logging::LoggerRef* syncLogger)
{
  m_backend = std::move(backend);
  m_gpuPrefetchEnabled = m_backend && m_backend->gpuAsyncOffload();
  m_backlogThreshold = gpuConfig.backlogThreshold;
  m_trustGpuCache = gpuConfig.trustGpuCache;
  m_prefetch.configure(m_gpuPrefetchEnabled ? gpuConfig.prefetchQueueDepth : 0,
                       m_gpuPrefetchEnabled ? gpuConfig.prefetchWindow : 0,
                       m_gpuPrefetchEnabled ? gpuConfig.backlogThreshold : 0,
                       m_gpuPrefetchEnabled ? m_backend.get() : nullptr);
  setPowSyncLogger(m_gpuPrefetchEnabled ? syncLogger : nullptr);
}

void PowService::shutdown()
{
  m_gpuPrefetchEnabled = false;
  setPowSyncLogger(nullptr);
  m_prefetch.configure(0, 0, 0, nullptr);
  if (m_backend)
    m_backend->shutdown();
}

PowVerifyBackend& PowService::backend()
{
  return *m_backend;
}

PowPrefetchCache& PowService::prefetch() { return m_prefetch; }

bool PowService::gpuSpeculativeActive() const
{
  const size_t backlog = std::max(m_syncBatchRemainder, static_cast<size_t>(m_syncCatchupGap));
  return m_gpuPrefetchEnabled && backlog >= m_backlogThreshold;
}

void PowService::updateSyncContext(size_t batchRemainder, uint32_t catchupGap)
{
  m_syncBatchRemainder = batchRemainder;
  m_syncCatchupGap = catchupGap;
}

void PowService::onBlockValidated(uint32_t height)
{
  m_prefetch.setValidatedTip(height);
  if (m_backend)
  {
    m_backend->dropWorkAtOrBelow(height);
    const uint32_t keepBack = 32;
    const uint32_t floor = height > keepBack ? height - keepBack : 0;
    m_backend->pruneResultsBelow(floor);
  }
}

bool PowService::verifyCnGpu(crypto::cn_context& ctx, const Block& block, difficulty_type difficulty,
                             crypto::Hash& proofOfWork, uint32_t blockHeight)
{
  crypto::Hash blockId = get_block_hash(block);

  crypto::Hash gpuCached;
  const bool hadGpuCache =
      !m_trustGpuCache && gpuSpeculativeActive() && blockHeight != UINT32_MAX &&
      m_backend->tryConsumeLonghash(blockHeight, blockId, gpuCached);

  if (m_trustGpuCache && gpuSpeculativeActive() && blockHeight != UINT32_MAX &&
      m_backend->tryConsumeLonghash(blockHeight, blockId, proofOfWork))
  {
    ++m_profileMetrics.gpuHits;
    if (m_gpuPrefetchEnabled && powSyncLogEnabled(blockHeight))
    {
      std::ostringstream oss;
      oss << "CN-GPU verify height=" << blockHeight << " id=" << powHashShort(blockId)
          << " PoW=GPU (cached OpenCL inner, CPU finish_hash)";
      powSyncLogDebug(oss.str());
    }
    return check_hash(proofOfWork, difficulty);
  }

  BinaryArray blob;
  if (!get_block_hashing_blob(block, blob))
    return false;

  if (!m_backend->computeLonghash(ctx, blob.data(), blob.size(), proofOfWork))
    return false;

  ++m_profileMetrics.cpuPowUsed;

  if (hadGpuCache)
  {
    if (memcmp(gpuCached.data, proofOfWork.data, sizeof(proofOfWork.data)) == 0)
      ++m_profileMetrics.gpuHits;
    else
    {
      ++m_profileMetrics.gpuCacheMismatch;
      if (m_gpuPrefetchEnabled && powSyncLogEnabled(blockHeight))
      {
        std::ostringstream oss;
        oss << "CN-GPU verify height=" << blockHeight << " id=" << powHashShort(blockId)
            << " GPU/CPU PoW MISMATCH — using CPU hash";
        powSyncLogDebug(oss.str());
      }
    }
  }

  if (m_gpuPrefetchEnabled && powSyncLogEnabled(blockHeight))
  {
    std::ostringstream oss;
    oss << "CN-GPU verify height=" << blockHeight << " id=" << powHashShort(blockId)
        << " PoW=CPU (" << powCpuInnerName();
    if (!gpuSpeculativeActive())
      oss << ", speculative off (catchup=" << m_syncCatchupGap << " batch=" << m_syncBatchRemainder
          << " < " << m_backlogThreshold << ")";
    else
      oss << ", no GPU result in cache";
    oss << ")";
    powSyncLogDebug(oss.str());
  }

  return check_hash(proofOfWork, difficulty);
}

void PowService::recordPushBlockTiming(uint64_t powNs, uint64_t txNs, uint64_t dbNs)
{
  m_profileMetrics.powVerifyNs += powNs;
  m_profileMetrics.txValidateNs += txNs;
  m_profileMetrics.dbCommitNs += dbNs;
}

const PowVerifyMetrics& PowService::metrics() const
{
  if (m_backend)
  {
    static PowVerifyMetrics combined;
    combined = m_backend->metrics();
    combined.powVerifyNs = m_profileMetrics.powVerifyNs;
    combined.txValidateNs = m_profileMetrics.txValidateNs;
    combined.dbCommitNs = m_profileMetrics.dbCommitNs;
    combined.cpuPowUsed += m_profileMetrics.cpuPowUsed;
    combined.gpuHits += m_profileMetrics.gpuHits;
    combined.gpuCacheMismatch += m_profileMetrics.gpuCacheMismatch;
    return combined;
  }
  return m_profileMetrics;
}

} // namespace cn
