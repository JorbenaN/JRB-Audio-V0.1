#pragma once

// Real-time annotations and the small set of trusted primitives the audio
// thread may call (docs/03-audio-engine.md §6).
//
// JRB_NONBLOCKING marks a function that runs on the audio thread or a DSP
// worker. Clang checks it at compile time (-Wfunction-effects, an error in
// this project): such a function may not allocate, free, lock, throw, or call
// anything not itself proven non-blocking.
//
// The functions below are the audited exceptions: system calls that are
// real-time safe but carry no annotation in the SDK. Every addition here
// needs a justification comment.

#include <cstdint>

#if defined(__clang__) && __has_cpp_attribute(clang::nonblocking)
#define JRB_NONBLOCKING [[clang::nonblocking]]
#else
#define JRB_NONBLOCKING
#endif

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif
#include <chrono>

namespace jrb::rt {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfunction-effects"

/// Monotonic time in nanoseconds. mach_absolute_time reads the commpage: no
/// system call, no lock.
inline std::uint64_t nowNanos() noexcept JRB_NONBLOCKING {
#if defined(__APPLE__)
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        return tb;
    }();
    return mach_absolute_time() * timebase.numer / timebase.denom;
#else
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

/// libm sine: pure computation (no allocation, locks or errno-dependent
/// control flow on Apple platforms), merely unannotated in the SDK.
inline double sin(double radians) noexcept JRB_NONBLOCKING { return __builtin_sin(radians); }

#pragma clang diagnostic pop

/// Hint to the CPU that we are spin-waiting.
inline void cpuPause() noexcept JRB_NONBLOCKING {
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}

} // namespace jrb::rt
