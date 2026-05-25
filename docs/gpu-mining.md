# GPU mining (OpenCL)

Mine blocks on your own node using OpenCL GPUs. This is separate from [GPU PoW offload](gpu-offload.md), which only accelerates sync verification.

See also: [GPU PoW offload](gpu-offload.md) for `--gpu-device` sync tuning.

## Build

```bash
cmake -B build -DWITH_OPENCL=ON
cmake --build build
```

Install vendor OpenCL (ROCm/NVIDIA). On Linux, add your user to `render` and `video` groups for GPU access.

## List devices

```bash
./build/src/conceald --gpu-list
```

Use the **global index** in `--start-gpu-mining`.

## Start mining (CLI)

```text
--start-gpu-mining <addr,deviceId:intensity[,deviceId:intensity...]>
```

Example (one GPU, total intensity 1920 → 3 host threads × 640 each):

```bash
./build/src/conceald --start-gpu-mining ccx1YourAddress...,0:1920 --data-dir /path/to/data
```

**Intensity is the total for the GPU**, split evenly across **3 host threads** (like three xmr-stak `gpu_threads_conf` entries with the same `"index"`). OpenCL **worksize is fixed at 8**; it is not a CLI parameter.

Each host thread owns an **independent OpenCL pipeline**: its own command **queue**, buffer set, and kernel objects. All three queues run on the same GPU in parallel (no shared queue mutex), which keeps the device fed more continuously.

### OpenCL kernel cache

Compiled device binaries are cached under:

```text
/tmp/conceal-opencl/<deviceIndex>-<hash>.bin
```

On startup, the miner tries to load the cached binary before compiling from source. The cache key includes the device index, program kind (`mine` vs verify `inner`), build flags, and kernel source. A hit is logged as `[pow/opencl-cache] hit …`; a fresh compile ends with `saved …`. Cache survives daemon restarts until `/tmp` is cleared or kernels/build options change.

### Intensity alignment

Total intensity is rounded **down** to a multiple of `3 × 8 = 24`, then divided by 3:

| Input | Aligned total | Per thread (×3) |
|-------|---------------|-----------------|
| 1920  | 1920          | 640             |
| 1922  | 1920          | 640             |
| 100   | 96            | 32              |

Minimum aligned total is 24 (3 × 8).

### xmr-stak mapping

Three entries like:

```json
{ "index": 0, "intensity": 640, "worksize": 8, ... }
{ "index": 0, "intensity": 640, "worksize": 8, ... }
{ "index": 0, "intensity": 640, "worksize": 8, ... }
```

→ `--start-gpu-mining addr,0:1920` (640 × 3 = 1920 total).

## CPU vs GPU mining

**Mutually exclusive.** If both `--start-mining` and `--start-gpu-mining` are set, GPU mining is **discarded** (warning logged; CPU mining wins).

At runtime, `start_mining` and `start_gpu_mining` reject starting if the other mode is already active.

## Console commands

```text
start_gpu_mining <addr,deviceId:intensity[,deviceId:intensity...]>
stop_gpu_mining
show_hr          # shows CPU and/or GPU hashrate when mining
hide_hr
```

Console `start_gpu_mining` uses the **same comma-separated value** as `--start-gpu-mining` (one argument, no spaces between address and device spec):

```text
start_gpu_mining ccx1YourAddress...,0:1920
```

## Conflicts with verify offload

Do not use the same OpenCL device index for `--gpu-device` (sync offload) and GPU mining. The daemon refuses to start GPU mining on a device already assigned to verify offload.

## Smoke test

```bash
./build/src/conceald --gpu-list
./build/src/conceald --start-gpu-mining ccx1...,0:1920 --data-dir /path/to/data
# after sync, watch for hashrate logs; stop_gpu_mining in console
```

Optional staged CPU vs GPU mining probe (OpenCL + GPU required):

```bash
cmake -B build -DWITH_OPENCL=ON -DBUILD_TESTS=ON
cmake --build build --target GpuVsCpuMiningHashTests
./build/tests/gpu_vs_cpu_mining_hash_tests --device 0 --nonce 0
```

Prints `MATCH` / `MISMATCH` after prepare, 1 inner iter, full inner, and finish.

Optional config parse test (no GPU required):

```bash
./build/tests/PowGpuMineEquivalenceTests
```
