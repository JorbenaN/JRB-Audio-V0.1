#include "jrb/engine/WorkerPool.hpp"

#include <algorithm>

namespace jrb::engine {
namespace {

constexpr std::uint64_t kIndexMask = 0xFFFF;

constexpr std::uint32_t generationOf(std::uint64_t s) noexcept { return static_cast<std::uint32_t>(s >> 32); }
constexpr int countOf(std::uint64_t s) noexcept { return static_cast<int>((s >> 16) & kIndexMask); }
constexpr int nextOf(std::uint64_t s) noexcept { return static_cast<int>(s & kIndexMask); }

} // namespace

void WorkerPool::start(int count, std::uint64_t periodNanos, JoinWorkgroup join) {
    stop();
    quit_.store(false);
    joined_.store(0);
    realtime_.store(0);
    for (int i = 0; i < count; ++i)
        threads_.emplace_back([this, periodNanos, join] { workerMain(periodNanos, join); });
}

void WorkerPool::stop() {
    if (threads_.empty())
        return;
    quit_.store(true);
    for (std::size_t i = 0; i < threads_.size(); ++i)
        wake_.signal();
    for (auto& t : threads_)
        t.join();
    threads_.clear();
}

void WorkerPool::workerMain(std::uint64_t periodNanos, const JoinWorkgroup& join) {
    // Expect to use up to half of each period; the constraint is the full period.
    if (platform::makeCurrentThreadRealtime(periodNanos, periodNanos / 2))
        realtime_.fetch_add(1);
    std::unique_ptr<device::WorkgroupMembership> membership;
    if (join) {
        membership = join();
        if (membership && membership->joined())
            joined_.fetch_add(1);
    }
    for (;;) {
        wake_.wait();
        if (quit_.load(std::memory_order_acquire))
            break;
        workOn(generationOf(state_.load(std::memory_order_acquire)), true);
    }
    // `membership` is released here, on the thread that joined.
}

bool WorkerPool::claim(std::uint32_t generation, int& index) noexcept JRB_NONBLOCKING {
    std::uint64_t s = state_.load(std::memory_order_acquire);
    for (;;) {
        if (generationOf(s) != generation || nextOf(s) >= countOf(s))
            return false;
        if (state_.compare_exchange_weak(s, s + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
            index = nextOf(s);
            return true;
        }
    }
}

void WorkerPool::workOn(std::uint32_t generation, bool countAsWorker) noexcept JRB_NONBLOCKING {
    int index = 0;
    while (claim(generation, index)) {
        // Safe: the batch slot cannot be reused until this job is counted as done.
        const Batch& batch = slots_[generation % kSlots];
        batch.fn(batch.context, index);
        if (countAsWorker)
            workerJobs_.fetch_add(1, std::memory_order_relaxed);
        done_.fetch_add(1, std::memory_order_release);
    }
}

void WorkerPool::run(JobFn fn, void* context, int count) noexcept JRB_NONBLOCKING {
    if (count <= 0)
        return;
    const int workers = static_cast<int>(threads_.size());
    if (workers == 0 || count == 1 || count > kMaxJobsPerBatch) {
        for (int i = 0; i < count; ++i)
            fn(context, i);
        return;
    }

    const std::uint32_t generation = ++generation_;
    slots_[generation % kSlots] = Batch{fn, context};
    done_.store(0, std::memory_order_relaxed);
    state_.store((static_cast<std::uint64_t>(generation) << 32) | (static_cast<std::uint64_t>(count) << 16),
                 std::memory_order_release);

    const int toWake = std::min(workers, count - 1);
    for (int i = 0; i < toWake; ++i)
        wake_.signal();

    workOn(generation, false);
    while (done_.load(std::memory_order_acquire) < count)
        rt::cpuPause();
}

} // namespace jrb::engine
