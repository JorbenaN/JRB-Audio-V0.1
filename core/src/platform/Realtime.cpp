#include "jrb/platform/Realtime.hpp"

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/task.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#endif

namespace jrb::platform {

#if defined(__APPLE__)

namespace {
std::uint32_t toAbsolute(std::uint64_t nanos) noexcept {
    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    return static_cast<std::uint32_t>(nanos * tb.denom / tb.numer);
}
} // namespace

bool makeCurrentThreadRealtime(std::uint64_t periodNanos, std::uint64_t computationNanos) noexcept {
    thread_time_constraint_policy_data_t policy{};
    policy.period = toAbsolute(periodNanos);
    policy.computation = toAbsolute(computationNanos);
    policy.constraint = toAbsolute(periodNanos);
    policy.preemptible = 1;
    return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                             reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
}

WakeSemaphore::WakeSemaphore() { semaphore_create(mach_task_self(), &sem_, SYNC_POLICY_FIFO, 0); }
WakeSemaphore::~WakeSemaphore() { semaphore_destroy(mach_task_self(), sem_); }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfunction-effects"
// semaphore_signal is a single Mach trap: no allocation, no user-space lock.
// It is the standard way for audio threads to wake workers on macOS.
void WakeSemaphore::signal() noexcept JRB_NONBLOCKING { semaphore_signal(sem_); }
#pragma clang diagnostic pop

void WakeSemaphore::wait() noexcept {
    while (semaphore_wait(sem_) == KERN_ABORTED) {
    }
}

#else

bool makeCurrentThreadRealtime(std::uint64_t, std::uint64_t) noexcept { return false; }  // TODO(windows): MMCSS
WakeSemaphore::WakeSemaphore() = default;
WakeSemaphore::~WakeSemaphore() = default;
void WakeSemaphore::signal() noexcept JRB_NONBLOCKING { sem_.release(); }
void WakeSemaphore::wait() noexcept { sem_.acquire(); }

#endif

} // namespace jrb::platform
