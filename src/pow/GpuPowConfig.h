// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license.

#pragma once

#include <cstdint>
#include <boost/program_options.hpp>

namespace cn
{

struct GpuPowConfig
{
  static void initOptions(boost::program_options::options_description& desc);

  void init(const boost::program_options::variables_map& vm);

  /** -1 = disabled; >=0 selects global device index from ranked GPU list */
  int deviceIndex = -1;
  bool listDevices = false;
  /** Max hashes per OpenCL dispatch (hard cap). */
  uint32_t batchSize = 128;
  /** Min jobs before dispatch unless oldest queued job age exceeds maxWaitUs (0 = no floor). */
  uint32_t minBatchSize = 64;
  uint32_t maxWaitUs = 8000;
  /** Max blocks ahead of validated tip to enqueue for GPU (0 = use default 1024). */
  uint32_t prefetchWindow = 1024;
  /** Min catch-up gap (peer − local) or batch size before GPU prefetch/consume is active. */
  uint32_t backlogThreshold = 16;
  /** Max GPU jobs queued/in-flight ahead of verify. */
  uint32_t prefetchQueueDepth = 256;
  bool debugCrossCheck = false;
  /** Log SSE inner_hash_3 state after 1 iter (also CN_GPU_INNER_TRACE=1) */
  bool debugInnerTrace = false;
  /** Use prefetched GPU PoW on cache hit without re-running CPU longhash (default on). */
  bool trustGpuCache = true;
};

} // namespace cn
