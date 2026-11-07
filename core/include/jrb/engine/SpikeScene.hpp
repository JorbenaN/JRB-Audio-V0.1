#pragma once

// Spike S2 "graph compiler": turns a scene description into a RenderPlan,
// reusing oscillator nodes by voice slot so state survives every recompile.
// Runs on a control thread and allocates freely.

#include "jrb/engine/RenderPlan.hpp"

#include <memory>
#include <vector>

namespace jrb::engine {

struct SpikeScene {
    double toneFrequency = 440.0;  // voice 0: the audible test tone
    float toneAmplitude = 0.05f;
    int loadVoices = 0;            // extra inaudible voices that create real DSP load
    int loadPartials = 16;
    float loadAmplitude = 0.0005f;
};

class SpikeCompiler {
public:
    std::unique_ptr<RenderPlan> compile(const SpikeScene& scene, int maxFrames);

    /// Frequency of load voice `index` (stable per slot).
    static double loadFrequency(int index) noexcept { return 110.0 + 13.0 * index; }

private:
    std::vector<std::shared_ptr<OscillatorNode>> registry_;  // slot → node
    std::uint64_t nextPlanId_ = 1;
};

} // namespace jrb::engine
