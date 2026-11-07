#pragma once

// OS services for real-time threads: scheduling policy and a wake-up
// semaphore whose signal side is safe to call from the audio thread.

#include "jrb/rt/Realtime.hpp"

#include <cstdint>

#if defined(__APPLE__)
#include <mach/semaphore.h>
#else
#include <semaphore>
#endif

namespace jrb::platform {

/// Gives the calling thread real-time (time-constraint) scheduling, the same
/// class Core Audio uses for its I/O thread. `periodNanos` is the audio block
/// duration; `computationNanos` how much of it this thread expects to use.
/// Returns false if the OS refused.
bool makeCurrentThreadRealtime(std::uint64_t periodNanos, std::uint64_t computationNanos) noexcept;

/// Counting semaphore. signal() may be called from the audio thread;
/// wait() blocks and is for worker threads only.
class WakeSemaphore {
public:
    WakeSemaphore();
    ~WakeSemaphore();
    WakeSemaphore(const WakeSemaphore&) = delete;
    WakeSemaphore& operator=(const WakeSemaphore&) = delete;

    void signal() noexcept JRB_NONBLOCKING;
    void wait() noexcept;

private:
#if defined(__APPLE__)
    semaphore_t sem_{};
#else
    std::counting_semaphore<> sem_{0};
#endif
};

} // namespace jrb::platform
