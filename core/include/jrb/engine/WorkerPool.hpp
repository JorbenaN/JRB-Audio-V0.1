#pragma once

// Parallel execution of independent DSP jobs within one audio block
// (docs/03-audio-engine.md §5). The audio thread publishes a batch, wakes up
// to N real-time workers, works on the batch itself, and spins until every
// claimed job has finished.
//
// Claims go through one 64-bit atomic: [generation:32 | count:16 | next:16].
// A worker can only claim a job of the generation it read, so a worker that
// wakes up late can never run a job twice or touch a newer batch's data.

#include "jrb/device/AudioDevice.hpp"
#include "jrb/platform/Realtime.hpp"
#include "jrb/rt/Realtime.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace jrb::engine {

using JobFn = void (*)(void* context, int index) noexcept JRB_NONBLOCKING;

class WorkerPool {
public:
    using JoinWorkgroup = std::function<std::unique_ptr<device::WorkgroupMembership>()>;
    static constexpr int kMaxJobsPerBatch = 0xFFFF;

    WorkerPool() = default;
    ~WorkerPool() { stop(); }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    /// Non-real-time. Starts `count` workers with real-time scheduling sized for
    /// blocks of `periodNanos`. Each worker calls `join` (if set) on its own thread.
    void start(int count, std::uint64_t periodNanos, JoinWorkgroup join = {});
    /// Non-real-time. Must not be called while run() may be executing.
    void stop();

    [[nodiscard]] int workerCount() const noexcept { return static_cast<int>(threads_.size()); }
    [[nodiscard]] int workersInWorkgroup() const noexcept { return joined_.load(); }
    [[nodiscard]] int workersRealtime() const noexcept { return realtime_.load(); }
    /// Jobs executed by workers rather than the calling (audio) thread.
    [[nodiscard]] std::uint64_t jobsRunByWorkers() const noexcept { return workerJobs_.load(std::memory_order_relaxed); }

    /// Audio thread. Runs fn(context, i) for every i in [0, count).
    void run(JobFn fn, void* context, int count) noexcept JRB_NONBLOCKING;

private:
    struct Batch {
        JobFn fn = nullptr;
        void* context = nullptr;
    };
    static constexpr int kSlots = 4;

    bool claim(std::uint32_t generation, int& index) noexcept JRB_NONBLOCKING;
    void workOn(std::uint32_t generation, bool countAsWorker) noexcept JRB_NONBLOCKING;
    void workerMain(std::uint64_t periodNanos, const JoinWorkgroup& join);

    Batch slots_[kSlots];
    alignas(128) std::atomic<std::uint64_t> state_{0};
    alignas(128) std::atomic<int> done_{0};
    std::atomic<bool> quit_{false};
    std::atomic<int> joined_{0};
    std::atomic<int> realtime_{0};
    std::atomic<std::uint64_t> workerJobs_{0};
    platform::WakeSemaphore wake_;
    std::vector<std::thread> threads_;
    std::uint32_t generation_ = 0;  // audio thread only
};

} // namespace jrb::engine
