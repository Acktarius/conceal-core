here’s a compact pseudo‑C++ design that keeps GPU off the critical path by treating it as a speculative producer and the sync loop as a consumer of cached PoW results. OpenCL commands are naturally asynchronous from the host side, and a producer/consumer queue with condition variables is a standard fit for this pattern.
Core idea

The rule is simple:

    enqueue_pow(job) when you have a header far enough ahead of the current validation point.

    try_get_pow(job_id) when the CPU reaches that block.

    If no GPU result is ready, do CPU PoW immediately and move on; do not wait.

That means GPU can help sync only when it finishes early enough to populate a result cache before the CPU needs the answer.
Pseudo-C++ sketch

cpp
// pow_types.hpp
#pragma once
#include <array>
#include <cstdint>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

using BlockHeight = uint64_t;
using PowJobId = uint64_t;

struct PowInput {
    BlockHeight height{};
    PowJobId job_id{};
    std::array<uint8_t, 80> header_blob{};   // example; replace with exact PoW input
    std::array<uint8_t, 200> spad_seed{};    // example; replace with exact CN input state
};

struct PowResult {
    PowJobId job_id{};
    BlockHeight height{};
    bool valid{false};
    std::array<uint8_t, 32> hash{};
    std::chrono::steady_clock::time_point ready_at{};
};

cpp
// pow_result_cache.hpp
#pragma once
#include <unordered_map>
#include <shared_mutex>
#include <optional>

class PowResultCache {
public:
    void put(PowResult result) {
        std::unique_lock lock(mutex_);
        results_[result.job_id] = std::move(result);
    }

    std::optional<PowResult> try_get(PowJobId id) const {
        std::shared_lock lock(mutex_);
        auto it = results_.find(id);
        if (it == results_.end()) return std::nullopt;
        return it->second;
    }

    void erase(PowJobId id) {
        std::unique_lock lock(mutex_);
        results_.erase(id);
    }

    void prune_below(BlockHeight min_height_kept) {
        std::unique_lock lock(mutex_);
        for (auto it = results_.begin(); it != results_.end(); ) {
            if (it->second.height < min_height_kept) it = results_.erase(it);
            else ++it;
        }
    }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<PowJobId, PowResult> results_;
};

cpp
// pow_job_queue.hpp
#pragma once
#include <deque>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <vector>

class PowJobQueue {
public:
    explicit PowJobQueue(size_t max_depth) : max_depth_(max_depth) {}

    bool enqueue(PowInput job) {
        std::unique_lock lock(mutex_);

        if (shutdown_) return false;
        if (queue_.size() >= max_depth_) return false;
        if (inflight_or_queued_.contains(job.job_id)) return false;

        inflight_or_queued_.insert(job.job_id);
        queue_.push_back(std::move(job));
        cv_not_empty_.notify_one();
        return true;
    }

    std::vector<PowInput> pop_batch(size_t max_batch, std::chrono::milliseconds max_wait) {
        std::unique_lock lock(mutex_);

        cv_not_empty_.wait_for(lock, max_wait, [&] {
            return shutdown_ || !queue_.empty();
        });

        std::vector<PowInput> batch;
        if (shutdown_ || queue_.empty()) return batch;

        const size_t n = std::min(max_batch, queue_.size());
        batch.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            batch.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return batch;
    }

    void mark_done(PowJobId id) {
        std::unique_lock lock(mutex_);
        inflight_or_queued_.erase(id);
    }

    void shutdown() {
        std::unique_lock lock(mutex_);
        shutdown_ = true;
        cv_not_empty_.notify_all();
    }

private:
    size_t max_depth_;
    bool shutdown_{false};

    std::deque<PowInput> queue_;
    std::unordered_set<PowJobId> inflight_or_queued_;

    std::mutex mutex_;
    std::condition_variable cv_not_empty_;
};

cpp
// opencl_pow_worker.hpp
#pragma once
#include <thread>
#include <atomic>

class OpenClPowWorker {
public:
    OpenClPowWorker(PowJobQueue& queue,
                    PowResultCache& cache,
                    size_t batch_size,
                    std::chrono::milliseconds batch_wait)
        : queue_(queue), cache_(cache), batch_size_(batch_size), batch_wait_(batch_wait) {}

    void start() {
        running_ = true;
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        running_ = false;
        queue_.shutdown();
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        init_opencl_once(); // device, context, queue, kernel, reusable buffers

        while (running_) {
            auto batch = queue_.pop_batch(batch_size_, batch_wait_);
            if (batch.empty()) continue;

            // Host → device, enqueue kernel, device → host.
            // Use one or more command queues/events; host side should not block the daemon thread.
            auto gpu_outputs = run_opencl_batch(batch);

            for (size_t i = 0; i < batch.size(); ++i) {
                PowResult result;
                result.job_id = batch[i].job_id;
                result.height = batch[i].height;
                result.valid = gpu_outputs[i].valid;
                result.hash = gpu_outputs[i].hash;
                result.ready_at = std::chrono::steady_clock::now();

                cache_.put(std::move(result));
                queue_.mark_done(batch[i].job_id);
            }
        }
    }

    struct GpuOutput {
        bool valid{};
        std::array<uint8_t, 32> hash{};
    };

    void init_opencl_once();
    std::vector<GpuOutput> run_opencl_batch(const std::vector<PowInput>& batch);

    PowJobQueue& queue_;
    PowResultCache& cache_;
    size_t batch_size_;
    std::chrono::milliseconds batch_wait_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

cpp
// pow_prefetcher.hpp
#pragma once

class PowPrefetcher {
public:
    PowPrefetcher(size_t queue_depth,
                  size_t batch_size,
                  std::chrono::milliseconds batch_wait)
        : queue_(queue_depth), worker_(queue_, cache_, batch_size, batch_wait) {}

    void start() { worker_.start(); }
    void stop() { worker_.stop(); }

    bool enqueue_pow(const PowInput& job,
                     BlockHeight validated_tip,
                     size_t prefetch_window) {
        if (job.height <= validated_tip) return false;
        if (job.height > validated_tip + prefetch_window) return false;
        return queue_.enqueue(job);
    }

    std::optional<PowResult> try_get_pow(PowJobId id) const {
        return cache_.try_get(id);
    }

    void consume(PowJobId id) {
        cache_.erase(id);
    }

    void prune(BlockHeight validated_tip, size_t keep_back = 32) {
        const BlockHeight floor = (validated_tip > keep_back) ? (validated_tip - keep_back) : 0;
        cache_.prune_below(floor);
    }

private:
    PowJobQueue queue_;
    PowResultCache cache_;
    OpenClPowWorker worker_;
};

Sync loop usage

Use the prefetcher in two places: once when headers/blocks arrive, and once when validation reaches that block. The cache should be checked first, but the CPU must never block waiting for the GPU.

cpp
// When a block/header is parsed and becomes a plausible candidate:
void on_block_announced(const ParsedBlock& blk,
                        PowPrefetcher& prefetcher,
                        BlockHeight validated_tip)
{
    PowInput job;
    job.height = blk.height;
    job.job_id = blk.stable_job_id;   // e.g. derived from hash or internal id
    job.header_blob = blk.header_blob;
    job.spad_seed = blk.spad_seed;    // whatever exact input your OpenCL path needs

    constexpr size_t kPrefetchWindow = 512;
    prefetcher.enqueue_pow(job, validated_tip, kPrefetchWindow);
}

cpp
// When the core validation loop reaches a block whose PoW must be checked now:
bool verify_block_pow(const ParsedBlock& blk, PowPrefetcher& prefetcher)
{
    if (auto cached = prefetcher.try_get_pow(blk.stable_job_id)) {
        const bool ok = validate_finish_hash_and_target(blk, cached->hash);
        prefetcher.consume(blk.stable_job_id);
        if (ok) {
            log_info("PoW=GPU cache hit height=", blk.height);
            return true;
        }
        log_warn("GPU cached PoW mismatch or weak PoW; falling back to CPU height=", blk.height);
    }

    // IMPORTANT: never wait on GPU here.
    auto cpu_hash = compute_pow_on_cpu(blk);
    const bool ok = validate_finish_hash_and_target(blk, cpu_hash);
    log_info("PoW=CPU fallback height=", blk.height);
    return ok;
}

Important behavior

This design avoids the slowdown you saw because:

    the daemon never blocks on clFinish() or a “wait until GPU result is ready” call in the validation path,

    the GPU worker uses a bounded queue and batches jobs independently of the sync thread,

    results are stored in a synchronized cache; a shared mutex is appropriate when reads are common and writes are occasional, but all access still needs locking.

Operational rules

A few rules make this practical:

    Only enqueue jobs for blocks that are ahead of the validated tip but still within a bounded prefetch window.

    Deduplicate by job_id so a block is not queued repeatedly.

    If the GPU result is late, the CPU wins and the GPU result becomes discardable.

    Add metrics:

        queue depth,

        average batch size,

        cache hit ratio,

        CPU fallback ratio,

        GPU batch latency,

        result age when consumed.

If cache hit ratio stays low during sync, the GPU is not staying far enough ahead to help, even if the kernel itself is correct.
One key adjustment

In your case I would make the very first production version even stricter:

    Only use GPU results during initial sync when backlog depth is above a threshold, for example 128+ queued future blocks.

    Disable GPU use automatically when backlog collapses near the tip, because at that point the CPU path will usually be lower latency than OpenCL round-trips.

That keeps the GPU in the regime where batching can actually work