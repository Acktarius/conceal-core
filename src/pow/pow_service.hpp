// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#pragma once

#include <cstdint>
#include <memory>

#include "../CryptoNoteCore/CryptoNoteBasic.h"
#include "../Logging/LoggerRef.h"
#include "backend.hpp"
#include "GpuPowConfig.h"
#include "prefetch_cache.hpp"

namespace cn
{

class PowService
{
public:
  static PowService& instance();

  void init(std::unique_ptr<PowVerifyBackend> backend, const GpuPowConfig& gpuConfig,
            logging::LoggerRef* syncLogger = nullptr);

  /** Stop prefetch, drain GPU queue, and join the OpenCL worker. */
  void shutdown();

  PowVerifyBackend& backend();
  PowPrefetchCache& prefetch();

  /** OpenCL async prefetch is active (--gpu-device N and backend ready). */
  bool gpuPrefetchEnabled() const { return m_gpuPrefetchEnabled; }

  /** GPU result cache may be used (sync backlog above threshold). */
  bool gpuSpeculativeActive() const;

  /** @a batchRemainder: blocks left in current P2P batch; @a catchupGap: peer height minus local tip. */
  void updateSyncContext(size_t batchRemainder, uint32_t catchupGap);

  void onBlockValidated(uint32_t height);

  bool verifyCnGpu(crypto::cn_context& ctx, const Block& block, difficulty_type difficulty,
                   crypto::Hash& proofOfWork, uint32_t blockHeight);

  void recordPushBlockTiming(uint64_t powNs, uint64_t txNs, uint64_t dbNs);

  const PowVerifyMetrics& metrics() const;

private:
  PowService() = default;
  std::unique_ptr<PowVerifyBackend> m_backend;
  PowPrefetchCache m_prefetch;
  PowVerifyMetrics m_profileMetrics;
  bool m_gpuPrefetchEnabled = false;
  bool m_trustGpuCache = true;
  uint32_t m_backlogThreshold = 128;
  size_t m_syncBatchRemainder = 0;
  uint32_t m_syncCatchupGap = 0;
};

} // namespace cn
