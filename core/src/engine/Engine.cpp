#include "jrb/engine/Engine.hpp"

#include <algorithm>
#include <numbers>

namespace jrb::engine {

// MARK: OscillatorNode

void OscillatorNode::render(float* out, int frames, double frequency, float amplitude, int partials,
                            double sampleRate) noexcept JRB_NONBLOCKING {
    if (frames <= 0)
        return;
    const int count = std::clamp(partials, 1, kMaxPartials);
    for (int i = 0; i < frames; ++i)
        out[i] = 0.0f;

    constexpr double twoPi = 2.0 * std::numbers::pi;
    for (int k = 1; k <= count; ++k) {
        const double increment = frequency * k / sampleRate;
        double phase = phase_[k - 1];
        if (increment < 0.5) {  // partials at or above Nyquist are skipped but keep their phase
            const auto weight = static_cast<float>(1.0 / k);
            for (int i = 0; i < frames; ++i) {
                out[i] += weight * static_cast<float>(rt::sin(twoPi * phase));
                phase += increment;
                if (phase >= 1.0)
                    phase -= 1.0;
            }
        } else {
            phase += increment * frames;
            phase -= static_cast<double>(static_cast<long long>(phase));
        }
        phase_[k - 1] = phase;
    }

    const float start = lastAmplitude_ < 0.0f ? amplitude : lastAmplitude_;
    const float step = (amplitude - start) / static_cast<float>(frames);
    for (int i = 0; i < frames; ++i)
        out[i] *= start + step * static_cast<float>(i + 1);
    lastAmplitude_ = amplitude;
}

// MARK: Engine

Engine::Engine() = default;

Engine::~Engine() {
    pool_.stop();
}

void Engine::prepare(double sampleRate, int maxFrames, int /*outputChannels*/) {
    sampleRate_ = sampleRate;
    maxFrames_ = maxFrames;
}

bool Engine::setMasterGain(float gain) noexcept {
    return params_.push(ParamChange{std::clamp(gain, 0.0f, 4.0f)});
}

EngineStats Engine::stats() const noexcept {
    EngineStats s;
    s.callbacks = callbacks_.load(std::memory_order_relaxed);
    s.frames = frames_.load(std::memory_order_relaxed);
    s.overruns = overruns_.load(std::memory_order_relaxed);
    s.maxCallbackNanos = maxNanos_.load(std::memory_order_relaxed);
    s.totalCallbackNanos = totalNanos_.load(std::memory_order_relaxed);
    s.planSwaps = plans_.swapCount();
    s.paramChanges = paramChanges_.load(std::memory_order_relaxed);
    s.captureDropped = captureDropped_.load(std::memory_order_relaxed);
    s.lastPlanId = lastPlanId_.load(std::memory_order_relaxed);
    return s;
}

void Engine::renderVoice(void* context, int index) noexcept JRB_NONBLOCKING {
    auto* self = static_cast<Engine*>(context);
    RenderPlan* plan = self->slicePlan_;
    const VoiceJob& v = plan->voices[static_cast<std::size_t>(index)];
    v.node->render(plan->voiceBuffer(static_cast<std::size_t>(index)), self->sliceFrames_, v.frequency, v.amplitude,
                   v.partials, self->sampleRate_);
}

void Engine::renderSlice(RenderPlan* plan, float* const* outputs, int channels, int offset, int frames) noexcept JRB_NONBLOCKING {
    slicePlan_ = plan;
    sliceFrames_ = frames;
    pool_.run(&Engine::renderVoice, this, static_cast<int>(plan->voices.size()));

    // Fixed summation order: the result is identical however jobs were distributed.
    const float start = masterGain_;
    const float step = (masterTarget_ - start) / static_cast<float>(frames);
    const std::size_t voices = plan->voices.size();
    for (int i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (std::size_t v = 0; v < voices; ++v)
            sum += plan->voiceBuffer(v)[i];
        const float sample = sum * (start + step * static_cast<float>(i + 1));
        for (int ch = 0; ch < channels; ++ch)
            outputs[ch][offset + i] = sample;
    }
    masterGain_ = masterTarget_;
}

void Engine::render(float* const* outputs, int channels, int frames) noexcept JRB_NONBLOCKING {
    const std::uint64_t t0 = rt::nowNanos();

    ParamChange change{};
    while (params_.pop(change)) {
        masterTarget_ = change.masterGain;
        paramChanges_.fetch_add(1, std::memory_order_relaxed);
    }

    RenderPlan* plan = plans_.acquire();
    if (plan == nullptr || plan->voices.empty() || plan->maxFrames <= 0) {
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                outputs[ch][i] = 0.0f;
        masterGain_ = masterTarget_;
    } else {
        lastPlanId_.store(plan->id, std::memory_order_relaxed);
        // The device may deliver more frames than the plan was sized for.
        for (int offset = 0; offset < frames; offset += plan->maxFrames)
            renderSlice(plan, outputs, channels, offset, std::min(plan->maxFrames, frames - offset));
    }

    if (capture_ != nullptr && channels > 0) {
        std::uint64_t dropped = 0;
        for (int i = 0; i < frames; ++i)
            dropped += capture_->push(outputs[0][i]) ? 0 : 1;
        if (dropped)
            captureDropped_.fetch_add(dropped, std::memory_order_relaxed);
    }
    if (outputMuted_.load(std::memory_order_relaxed))
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                outputs[ch][i] = 0.0f;

    const std::uint64_t elapsed = rt::nowNanos() - t0;
    const auto budget = static_cast<std::uint64_t>(1e9 * frames / sampleRate_);
    callbacks_.fetch_add(1, std::memory_order_relaxed);
    frames_.fetch_add(static_cast<std::uint64_t>(frames), std::memory_order_relaxed);
    totalNanos_.fetch_add(elapsed, std::memory_order_relaxed);
    if (elapsed > budget)
        overruns_.fetch_add(1, std::memory_order_relaxed);
    if (elapsed > maxNanos_.load(std::memory_order_relaxed))
        maxNanos_.store(elapsed, std::memory_order_relaxed);
}

} // namespace jrb::engine
