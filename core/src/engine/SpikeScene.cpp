#include "jrb/engine/SpikeScene.hpp"

#include <algorithm>

namespace jrb::engine {

std::unique_ptr<RenderPlan> SpikeCompiler::compile(const SpikeScene& scene, int maxFrames) {
    const std::size_t voiceCount = 1 + static_cast<std::size_t>(std::max(0, scene.loadVoices));

    // Keep nodes for surviving slots; create nodes for new slots; drop the rest
    // (they stay alive until every plan that references them has been freed).
    registry_.resize(voiceCount);
    for (auto& node : registry_)
        if (!node)
            node = std::make_shared<OscillatorNode>();

    auto plan = std::make_unique<RenderPlan>();
    plan->id = nextPlanId_++;
    plan->maxFrames = maxFrames;
    plan->nodes = registry_;
    plan->voices.reserve(voiceCount);
    plan->voices.push_back({registry_[0].get(), scene.toneFrequency, scene.toneAmplitude, 1});
    for (std::size_t i = 1; i < voiceCount; ++i)
        plan->voices.push_back({registry_[i].get(), loadFrequency(static_cast<int>(i - 1)), scene.loadAmplitude,
                                scene.loadPartials});
    plan->scratch.assign(voiceCount * static_cast<std::size_t>(maxFrames), 0.0f);
    return plan;
}

} // namespace jrb::engine
