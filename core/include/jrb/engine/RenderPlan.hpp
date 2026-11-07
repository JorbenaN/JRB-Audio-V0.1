#pragma once

// Spike S2 render plan: N independent oscillator voices rendered in parallel,
// then summed into the master bus. The general DAG compiler (03 §4) replaces
// this in Phase 3; the publish / swap / reclaim mechanics stay the same.
//
// Split of responsibilities:
//  - the plan is immutable and carries *parameters* (frequency, amplitude);
//  - nodes carry *state* (oscillator phase) and are shared between
//    consecutive plans, so swapping plans never resets a sound.

#include "jrb/rt/Realtime.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace jrb::engine {

/// Additive oscillator: partials k = 1...P at frequency f*k and amplitude a/k.
class OscillatorNode {
public:
    static constexpr int kMaxPartials = 64;

    OscillatorNode() noexcept { live_.fetch_add(1, std::memory_order_relaxed); }
    ~OscillatorNode() { live_.fetch_sub(1, std::memory_order_relaxed); }
    OscillatorNode(const OscillatorNode&) = delete;
    OscillatorNode& operator=(const OscillatorNode&) = delete;

    /// Writes `frames` samples (replacing the buffer contents). Amplitude
    /// changes are ramped linearly across the block to avoid clicks.
    void render(float* out, int frames, double frequency, float amplitude, int partials, double sampleRate) noexcept
        JRB_NONBLOCKING;

    /// Number of nodes currently alive (for leak tests).
    static long liveCount() noexcept { return live_.load(); }

private:
    double phase_[kMaxPartials] = {};  // cycles, in [0, 1)
    float lastAmplitude_ = -1.0f;      // < 0: no previous block, start at target
    static inline std::atomic<long> live_{0};
};

struct VoiceJob {
    OscillatorNode* node = nullptr;
    double frequency = 440.0;
    float amplitude = 0.0f;
    int partials = 1;
};

struct RenderPlan {
    std::uint64_t id = 0;
    int maxFrames = 0;
    std::vector<std::shared_ptr<OscillatorNode>> nodes;  // keeps every node alive while the plan exists
    std::vector<VoiceJob> voices;
    std::vector<float> scratch;  // voices.size() * maxFrames, preallocated

    [[nodiscard]] float* voiceBuffer(std::size_t voice) noexcept JRB_NONBLOCKING {
        return scratch.data() + voice * static_cast<std::size_t>(maxFrames);
    }
};

} // namespace jrb::engine
