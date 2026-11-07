#pragma once

// The real-time engine (spike S2 scope). Control-side methods are called from
// the main thread; render() is called by the audio device.

#include "jrb/device/AudioDevice.hpp"
#include "jrb/engine/RenderPlan.hpp"
#include "jrb/engine/WorkerPool.hpp"
#include "jrb/rt/PlanExchange.hpp"
#include "jrb/rt/SpscQueue.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace jrb::engine {

struct EngineStats {
    std::uint64_t callbacks = 0;
    std::uint64_t frames = 0;
    std::uint64_t overruns = 0;         // callbacks whose processing took longer than their duration
    std::uint64_t maxCallbackNanos = 0;
    std::uint64_t totalCallbackNanos = 0;
    std::uint64_t planSwaps = 0;
    std::uint64_t paramChanges = 0;
    std::uint64_t captureDropped = 0;   // samples the capture queue had no room for
    std::uint64_t lastPlanId = 0;
};

class Engine final : public device::AudioCallback {
public:
    Engine();
    ~Engine() override;

    // MARK: Control side (main thread)

    /// Hands a plan to the audio thread; it takes effect at the next block.
    void publish(std::unique_ptr<RenderPlan> plan) { plans_.publish(std::move(plan)); }
    /// Frees plans the audio thread has retired. Call regularly (e.g. 10–50 Hz).
    std::size_t collectGarbage() { return plans_.collectGarbage(); }
    /// Continuous control: ramps to the new gain over the next block. False if the queue is full.
    bool setMasterGain(float gain) noexcept;
    /// Optional: receives the master bus (left channel) for analysis. Set before start.
    void setCapture(rt::SpscQueue<float>* capture) noexcept { capture_ = capture; }
    /// When true, the device receives silence while the capture still gets the real mix.
    void setOutputMuted(bool muted) noexcept { outputMuted_.store(muted); }

    WorkerPool& workers() noexcept { return pool_; }
    [[nodiscard]] EngineStats stats() const noexcept;
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] int maxFrames() const noexcept { return maxFrames_; }

    // MARK: Device side

    void prepare(double sampleRate, int maxFrames, int outputChannels) override;
    void render(float* const* outputs, int outputChannels, int frames) noexcept JRB_NONBLOCKING override;
    void stopped() override {}

private:
    struct ParamChange {
        float masterGain;
    };

    void renderSlice(RenderPlan* plan, float* const* outputs, int channels, int offset, int frames) noexcept JRB_NONBLOCKING;
    static void renderVoice(void* engine, int index) noexcept JRB_NONBLOCKING;

    rt::PlanExchange<RenderPlan> plans_;
    rt::SpscQueue<ParamChange> params_{256};
    WorkerPool pool_;
    rt::SpscQueue<float>* capture_ = nullptr;
    std::atomic<bool> outputMuted_{false};

    double sampleRate_ = 48000.0;
    int maxFrames_ = 0;

    // Audio-thread state.
    float masterGain_ = 1.0f;
    float masterTarget_ = 1.0f;
    RenderPlan* slicePlan_ = nullptr;
    int sliceFrames_ = 0;

    // Telemetry, written by the audio thread (relaxed), read by anyone.
    std::atomic<std::uint64_t> callbacks_{0}, frames_{0}, overruns_{0}, maxNanos_{0}, totalNanos_{0},
        paramChanges_{0}, captureDropped_{0}, lastPlanId_{0};
};

} // namespace jrb::engine
