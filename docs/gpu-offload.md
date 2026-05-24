# CN-GPU PoW verification offload (OpenCL)

Optional GPU acceleration for **CN-GPU (block major version >= 8)** proof-of-work **verification** during daemon sync and normal operation. Consensus rules are unchanged: the CPU reference (`cn_gpu_hash_v0`) remains authoritative; GPU results are validated by equivalence tests and optional debug cross-checks.

## Build

```bash
cmake -B build -DWITH_OPENCL=ON
cmake --build build
```

If OpenCL is not found, the build continues with CPU-only PoW verification.

Device discovery tries `CL_DEVICE_TYPE_GPU`, then `GPU|ACCELERATOR`, then `ALL` excluding CPU. When OpenCL is empty, `--gpu-list` also prints **PCI display adapters** from sysfs (kernel view only).

**AMD discrete GPUs (e.g. PCI `1002:15bf`, driver `amdgpu`)** usually need **ROCm OpenCL** and membership in groups **`render`** and **`video`** to access `/dev/dri/renderD*`. Without those groups, `clGetDeviceIDs` may return **0 devices** on the AMD platform even though `sudo conceald --gpu-list` shows the GPU (e.g. `gfx1103`). Miners run as **root** or with `render` access often work while a normal-user daemon does not.

```bash
sudo usermod -aG render,video $USER
# log out and back in, then:
./conceald --gpu-list
./conceald --gpu-device 0 --data-dir /path/to/data
```

Avoid running `conceald` as root in production; fix group membership instead.

**Note:** `0000:00:00.2` is typically the CPU/IOMMU bridge (`1022:14e9`), not the discrete GPU. Check `0000:65:00.0` (or your slot from `lspci`).

## CLI

| Flag | Description |
|------|-------------|
| `--gpu-device N` | Enable offload on global OpenCL GPU index `N` (e.g. `0`) |
| `--gpu-list` | List OpenCL compute devices (GPU, accelerator, non-CPU) and exit |
| `--gpu-batch-size` | Max hashes per OpenCL dispatch, hard cap (default `128`) |
| `--gpu-min-batch-size` | Min jobs before dispatch unless max-wait exceeded; `0` = no floor (default `64`) |
| `--gpu-max-wait-us` | Max microseconds to wait for batch fill before flushing a partial batch (default `9000`) |
| `--gpu-prefetch-window` | Max blocks ahead of validated tip to enqueue GPU jobs (default `1024`) |
| `--gpu-backlog-threshold` | Min catch-up gap or batch size before GPU prefetch/consume (default `16`) |
| `--gpu-prefetch-depth` | Max GPU jobs queued/in-flight ahead of verify (default `256`) |
| `--gpu-cpu-verify` | Always recompute PoW on CPU at verify (disables GPU cache fast path) |
| `--gpu-debug-crosscheck` | Sample CPU re-verify of GPU longhashes |
| `--gpu-debug-inner` | Trace SSE inner loop after 1 iter (CPU stderr + scratchpad dump on self-test failure) |

Environment: `CN_GPU_INNER_TRACE=1` enables the same CPU trace without the flag.

Example:

```bash
./build/src/conceald --gpu-device 0 --data-dir /path/to/data
```

## Architecture

GPU PoW is a **speculative accelerator**, not a mandatory step. Design rationale and component mapping are in [Design rationale](#design-rationale) below.

- `PowVerifyBackend`: CPU or OpenCL (inner-loop on GPU; Keccak expand/finish on CPU).
- **Batched async OpenCL**: `submitLonghash` queues jobs on a worker thread (tagged with chain height); the worker dispatches up to `--gpu-batch-size` jobs per enqueue once the queue holds at least `--gpu-min-batch-size` jobs or the oldest job has waited `--gpu-max-wait-us`. Completed hashes are stored in `GpuPowResultCache` keyed by height.
- **Default verify path**: on GPU cache hit, use the prefetched hash for `check_hash` (no CPU longhash). On miss, CPU `cn_gpu_hash_v0`.
- **Paranoid mode**: `--gpu-cpu-verify` always runs CPU longhash and compares against any GPU cache entry (`gpuCacheMismatch` if they differ).
- **Stale GPU work**: when the CPU validates height *H*, queued/finished GPU jobs for heights ≤ *H* are dropped so the queue refills with work ahead of the tip.
- **Prefetch policy**:
  - Only enqueue blocks strictly **ahead** of the validated tip and within `--gpu-prefetch-window`.
  - Only when the current P2P batch has ≥ `--gpu-backlog-threshold` future blocks (bulk initial sync).
  - Tail prefetch after a block is committed, plus **batch-start prefetch** when a P2P block batch arrives (fills the GPU queue before the CPU begins verifying block 1).
  - Late GPU results are discarded via cache prune; CPU wins if it verified first.
- `PowPrefetchCache`: only when **`--gpu-device N`** and OpenCL init succeeds.
- **Checkpoint zone**: mainnet heights ≤ **2 070 000** skip PoW entirely (checkpoints only). GPU offload applies only to the CN-GPU tail during sync.
- `PowService`: daemon-wide facade used from `Currency::checkProofOfWork` for v8+ blocks.

## Profiling sync (`pushBlock`)

When validating blocks, the daemon accumulates nanoseconds in `PowVerifyMetrics`:

- `powVerifyNs` — PoW gate (`checkProofOfWork`)
- `txValidateNs` — transaction and miner validation before DB write
- `dbCommitNs` — `pushBlock` / index update
- `gpuHits` — verify used a prefetched GPU result (cache hit)
- `gpuCacheMismatch` — GPU cache present but hash differed from CPU (only with `--gpu-cpu-verify`)
- `cpuPowUsed` — CPU longhash at verify (cache miss or `--gpu-cpu-verify`)
- `jobsSubmitted` — PoW jobs completed on the OpenCL worker (per batch job)

Every 256 CN-GPU blocks, an INFO line logs these counters during sync.

Per-block sync trace (category `pow`, heights above checkpoint **2 070 000**):

- `CN-GPU prefetch block height=…` — queued on GPU worker (**DEBUG**)
- `CN-GPU prefetch tail N block(s) heights …` — tail of incoming batch after commit (**DEBUG**)
- `CN-GPU dispatch K job(s) to GPU heights …` — worker **started** a batch (**DEBUG**)
- `CN-GPU ready height=…` — hash stored in the height cache (**DEBUG**)
- `CN-GPU verify height=…` — per-block verify path (**DEBUG**)

Near the chain tip (catch-up gap and batch remainder both &lt; `--gpu-backlog-threshold`), verification uses CPU even with `--gpu-device`. During large catch-up (gap ≫ threshold), GPU prefetch and cache hits stay enabled for the whole sync, not only the first blocks of each P2P batch.
Inspect via debugger or extend logging around `PowService::metrics()` during long sync runs. Suggested host profiling:

```bash
perf record -g ./build/src/conceald --testnet --gpu-device 0 ...
```

If `powVerifyNs` is a small fraction of `txValidateNs` + `dbCommitNs` on your hardware, GPU investment mainly helps on CN-GPU-heavy heights with large sync batches.

### Batch tuning

The worker uses a **hard cap** (`--gpu-batch-size`) and a **soft floor** (`--gpu-min-batch-size`). It waits until the queue holds at least `min_batch` jobs, hits `batch_size`, or the oldest queued job age exceeds `--gpu-max-wait-us` — then dispatches at most `batch_size` jobs. Small partial flushes (low `avgBatch` in metrics) usually mean `min_batch` is too low or `max_wait_us` is too short for your prefetch rate.

Example for large catch-up sync (defaults are already tuned for this; override only if needed):

```bash
./conceald --gpu-device 0 --data-dir /path/to/data
```

To disable the min-batch floor (flush partial batches sooner):

```bash
./conceald --gpu-device 0 --gpu-min-batch-size 0
```

## Clean-room OpenCL kernel

`src/pow/kernels/cn_gpu_inner.cl` is an **independent** implementation of the CN-GPU inner loop (MIT). It is **not** copied from GPLv3 miner code. Float types and bit-mask math follow the same structure as in-tree `inner_hash_3` (`float4 rnd_c`, `mm_and_ps` / `mm_or_ps` on `as_int4` lanes) and the public CN-GPU OpenCL layout described in [xmr-stak `cryptonight_gpu.cl`](https://github.com/fireice-uk/xmr-stak/blob/master/xmrstak/backend/amd/amd_gpu/opencl/cryptonight_gpu.cl) (bibliography only).

Merge gate: `PowGpuEquivalenceTests` must match `cn_gpu_hash_staged_reference` for all sample inputs. That reference uses the same inner path as production `cn_gpu_hash_v0` on this CPU (`inner_hash_3_avx` when AVX2 is available, else `inner_hash_3` / ARM `inner_hash_3`). The OpenCL kernel mirrors **`inner_hash_3_avx`**: two 32-byte scratch loads (`idx0`/`idx2`), `permute2f128` lane grouping, `double_compute_wrap` with paired `lcnt`/`hcnt`, 256-bit `out2` mixing (`permute2x128` imm `0x41`), and AVX `sub_round` (no in-place `n1`/`n3` mutation).

### OpenCL build requirements (bit-exact inner)

The program is built **without** `-cl-fast-relaxed-math`. Kernel source sets `#pragma OPENCL FP_CONTRACT OFF`. Host build options:

- `-cl-single-precision-constant`
- `-cl-fp32-correctly-rounded-divide-sqrt`

Inside `cn_gpu_inner.cl`, float→int uses `convert_*_rtz` (truncate toward zero, matching `_mm_cvttps_epi32`). `round_compute` uses per-lane `(float)((double)n/(double)d)` so AMDGPU `divps` matches x86 for consensus-sensitive steps.

## Failure modes

- OpenCL init, build, or self-test failure → **logged**, CPU fallback (never silent).
- GPU/CPU mismatch (debug cross-check) → logged, request fails verification path fallback.
- Checkpoint zones → PoW skipped (unchanged); GPU not used there.

## Test

```bash
./build/tests/PowGpuEquivalenceTests
```

## Design rationale

This section summarizes the speculative GPU PoW model and how it maps to the current codebase.

### Core rule

GPU PoW is a **speculative producer**; block validation is the **consumer**. The daemon must never block on OpenCL in the validation path.

1. **Enqueue** when a CN-GPU block is far enough ahead of the validated tip (within `--gpu-prefetch-window`).
2. **Try cache** when the CPU reaches that block at verify time.
3. **If no GPU result is ready**, run CPU longhash immediately and move on — do not wait on `clFinish()` or `awaitLonghash()` in the hot path.

GPU helps sync only when it finishes **before** the CPU needs the hash. A high `gpuHits` count means the pipeline is working; low hits with high `jobsSubmitted` means the GPU is computing but arriving too late.

### Design → implementation map

| Design sketch | Production code |
|---------------|-----------------|
| `PowPrefetcher::enqueue_pow` | `PowPrefetchCache::enqueueAtHeight` / `enqueueUpcoming` |
| `PowJobQueue` | `OpenclPowBackend` worker queue (`m_queue`, `m_pending`) |
| `OpenClPowWorker` | `OpenclPowBackend::workerLoop` + `executeBatch` |
| `PowResultCache` | `GpuPowResultCache` (keyed by block id / height) |
| `try_get_pow` / `consume` | `PowService::verifyCnGpu` → `tryGetLonghash` / cache hit path |
| `prune_below` | `dropWorkAtOrBelow`, `pruneResultsBelow` on validated tip advance |
| Backlog-gated prefetch | `gpuSpeculativeActive()` + `--gpu-backlog-threshold` |

OpenCL commands are asynchronous from the host; a dedicated worker thread with a condition variable batches jobs independently of the P2P/sync thread.

### Sync loop usage (two injection points)

**When blocks arrive** (`CryptoNoteProtocolHandler::processObjects`):

- **Batch-start prefetch** — enqueue CN-GPU blocks from the incoming P2P batch when backlog ≥ threshold.
- **Tail prefetch** — after each block is committed, enqueue remaining blocks in the current batch.

Both respect `shouldPrefetchHeight`: strictly **ahead** of validated tip and within `--gpu-prefetch-window`. Enqueue stops when `pendingJobCount()` reaches `--gpu-prefetch-depth`.

**When validation reaches a block** (`PowService::verifyCnGpu` via `Currency::checkProofOfWork`):

```text
if GPU cache hit and trust_gpu_cache:
    check_hash(gpu_hash)          # no CPU longhash
else if GPU cache hit and --gpu-cpu-verify:
    CPU longhash + compare        # gpuCacheMismatch if differ
else:
    CPU longhash                  # cache miss; GPU result late or absent
```

The CPU **never waits** for the GPU at verify. Late GPU results are discarded when the tip advances past their height.

### Why this avoids sync slowdown

- Validation never blocks on GPU completion.
- The worker uses a **bounded queue** and **batched dispatches** off the critical path.
- Results live in a mutex-protected cache; reads at verify are cheap lookups.
- Stale work (heights ≤ validated tip) is dropped from queue and cache so the pipeline refills with ahead-of-tip jobs.
- Near the chain tip, GPU prefetch is **disabled** when catch-up gap and batch remainder both fall below `--gpu-backlog-threshold` — at that point CPU-only verify is lower latency than OpenCL round-trips.

### Operational rules

| Rule | How it is enforced |
|------|-------------------|
| Prefetch only ahead of tip, within window | `PowPrefetchCache::shouldPrefetchHeight` |
| Deduplicate by block id | `OpenclPowBackend::jobKnownLocked` / `m_pending` |
| Bounded queue depth | `--gpu-prefetch-depth`, `pendingJobCount()` check |
| CPU wins if GPU is late | Cache miss → CPU longhash; no blocking wait |
| GPU wins on stale race | CPU verify first; late GPU result pruned |
| Silent fallback forbidden | OpenCL init/build/self-test failures log and fall back to CPU |
| Near-tip CPU-only | `backlogThreshold` gates speculative prefetch and cache use |

### Metrics to watch during sync

Every 256 CN-GPU blocks the daemon logs `PowVerifyMetrics`. Use these to judge pipeline health:

| Metric | Healthy sync | Problem signal |
|--------|--------------|----------------|
| `gpuHits / (gpuHits + cpuPowUsed)` | Rising during catch-up | Stays low → GPU not staying ahead |
| `avgBatch` | Near `min_batch`–`batch_size` | Very low → tune batch/wait knobs |
| `jobsSubmitted` vs `gpuHits` | Submitted ≥ hits | Large gap → GPU computes but results arrive late |
| `cpuFallback` | 0 | Non-zero → OpenCL runtime failures |
| `gpuCacheMismatch` | 0 (or N/A in trust mode) | Non-zero with `--gpu-cpu-verify` → correctness bug |

If cache hit ratio stays low during bulk sync, increasing `--gpu-batch-size` and `--gpu-prefetch-depth` (keeping `min_batch ≈ batch_size / 2`) usually helps more than narrowing `--gpu-prefetch-window`.

Host-side profiling (`perf record -g`) on `pushBlock` helps confirm whether PoW or tx/DB commit dominates after GPU tuning.

### Batch worker logic (conceptual)

The OpenCL worker implements the queue `pop_batch` pattern with two extra controls:

```text
max_batch = --gpu-batch-size          # hard cap per dispatch
min_batch = --gpu-min-batch-size      # soft floor (0 = disabled)
max_wait  = --gpu-max-wait-us

wait until: queue >= max_batch
         OR queue >= min_batch (and predicate wake)
         OR oldest_job_age >= max_wait
         OR shutdown

dispatch min(queue, max_batch) jobs
if batch < min_batch AND age < max_wait AND not shutdown:
    requeue and wait again
```

Partial flushes (small batches at batch tails or near tip) are normal in a dynamic P2P environment. What matters is that most GPU work runs in decently sized groups and validation never stalls on GPU fill.

### Trust mode vs paranoid verify

**Default (`trust_gpu_cache=yes`)**: cache hit skips CPU longhash; `gpuMismatch` is not incremented because no compare runs. Use for production sync performance.

**`--gpu-cpu-verify`**: always runs CPU longhash and compares; increments `gpuCacheMismatch` on divergence. Use when validating GPU correctness after kernel or driver changes.

### Reference pseudo-code

The full pseudo-C++ sketch (`PowJobQueue`, `OpenClPowWorker`, `PowPrefetcher`, sync-loop examples) lives in `docs/gpu-offload-considerations.md`. The production code follows the same producer/consumer pattern with Conceal-specific types (`crypto::cn_context`, block blobs, checkpoint gating, P2P batch integration).
