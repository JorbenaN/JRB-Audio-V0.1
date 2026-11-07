// Spike S2 engine tests that need no audio hardware: lock-free primitives under
// thread stress, the worker pool's exactly-once guarantee, and engine output
// checked against an independent reference, with plan swaps at random block sizes.

#include "Check.hpp"

#include "jrb/engine/Engine.hpp"
#include "jrb/engine/SpikeScene.hpp"
#include "jrb/rt/PlanExchange.hpp"
#include "jrb/rt/SpscQueue.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <numbers>
#include <random>
#include <thread>
#include <vector>

using namespace jrb;

namespace {

struct Counted {
    static inline std::atomic<long> live{0};
    std::uint64_t id;
    explicit Counted(std::uint64_t i) : id(i) { live.fetch_add(1); }
    ~Counted() { live.fetch_sub(1); }
};

/// Drives an engine the way a device would: random block sizes, with a script
/// of control actions applied between blocks.
struct OfflineDriver {
    engine::Engine& engine;
    std::vector<float> left, right;

    void renderBlock(int frames) {
        left.assign(static_cast<std::size_t>(frames), 0.0f);
        right.assign(static_cast<std::size_t>(frames), 0.0f);
        float* outs[2] = {left.data(), right.data()};
        engine.render(outs, 2, frames);
    }
};

} // namespace

TEST("SpscQueue: one million items cross threads in order, none lost") {
    rt::SpscQueue<std::uint32_t> queue(1024);
    constexpr std::uint32_t kCount = 1'000'000;
    std::thread producer([&] {
        for (std::uint32_t i = 0; i < kCount;)
            if (queue.push(i))
                ++i;
    });
    std::uint32_t expected = 0;
    bool inOrder = true;
    while (expected < kCount) {
        std::uint32_t v = 0;
        if (queue.pop(v)) {
            inOrder &= (v == expected);
            ++expected;
        }
    }
    producer.join();
    CHECK(inOrder);
    std::uint32_t extra = 0;
    CHECK(!queue.pop(extra));
}

TEST("PlanExchange: 50,000 publishes against a spinning consumer; nothing leaks, ids never go back") {
    {
        rt::PlanExchange<Counted> exchange(64);
        std::atomic<bool> running{true};
        bool monotonic = true;
        std::thread audio([&] {
            std::uint64_t last = 0;
            while (running.load()) {
                if (Counted* c = exchange.acquire()) {
                    monotonic &= c->id >= last;
                    last = c->id;
                }
            }
        });
        for (std::uint64_t i = 1; i <= 50'000; ++i) {
            exchange.publish(std::make_unique<Counted>(i));
            if (i % 16 == 0)
                exchange.collectGarbage();
        }
        running.store(false);
        audio.join();
        CHECK(monotonic);
        CHECK(exchange.swapCount() > 0);
    }
    CHECK(Counted::live.load() == 0);
}

TEST("WorkerPool: every job of 20,000 batches runs exactly once") {
    engine::WorkerPool pool;
    pool.start(4, 1'333'333);  // 64 frames at 48 kHz
    struct Ctx {
        std::array<std::atomic<std::uint32_t>, 64> marks{};
        std::atomic<int> executed{0};
        std::uint32_t batch = 0;
    } ctx;
    auto job = [](void* c, int i) noexcept JRB_NONBLOCKING {
        auto* self = static_cast<Ctx*>(c);
        self->marks[static_cast<std::size_t>(i)].store(self->batch, std::memory_order_relaxed);
        self->executed.fetch_add(1, std::memory_order_relaxed);
    };
    std::mt19937 rng(7);
    bool exact = true;
    long expectedTotal = 0;
    for (std::uint32_t b = 1; b <= 20'000; ++b) {
        const int count = std::uniform_int_distribution<int>(1, 64)(rng);
        ctx.batch = b;
        pool.run(job, &ctx, count);
        expectedTotal += count;
        for (int i = 0; i < count; ++i)
            exact &= ctx.marks[static_cast<std::size_t>(i)].load() == b;
    }
    CHECK(exact);
    CHECK(ctx.executed.load() == expectedTotal);
    CHECK(pool.jobsRunByWorkers() > 0);  // the workers really participated
    pool.stop();
}

TEST("Engine: tone matches an independent reference across plan swaps and odd block sizes") {
    constexpr double kRate = 48000.0;
    constexpr int kMaxFrames = 256;
    engine::Engine eng;
    eng.prepare(kRate, kMaxFrames, 2);
    engine::SpikeCompiler compiler;
    OfflineDriver driver{eng, {}, {}};

    std::mt19937 rng(42);
    double refPhase = 0.0;
    double refFreq = 440.0;
    const float amp = 0.25f;
    double maxError = 0.0;
    std::size_t rendered = 0;

    engine::SpikeScene scene;
    scene.toneAmplitude = amp;
    eng.publish(compiler.compile(scene, kMaxFrames));

    for (int block = 0; block < 2000; ++block) {
        if (block % 37 == 0) {  // swap plans: alternate the tone frequency
            refFreq = refFreq == 440.0 ? 660.0 : 440.0;
            scene.toneFrequency = refFreq;
            eng.publish(compiler.compile(scene, kMaxFrames));
            eng.collectGarbage();
        }
        const int frames = std::uniform_int_distribution<int>(1, 700)(rng);  // includes > maxFrames
        driver.renderBlock(frames);
        const double increment = refFreq / kRate;
        for (int i = 0; i < frames; ++i) {
            const double expected = amp * std::sin(2.0 * std::numbers::pi * refPhase);
            refPhase += increment;
            if (refPhase >= 1.0)
                refPhase -= 1.0;
            maxError = std::max(maxError, std::abs(expected - driver.left[static_cast<std::size_t>(i)]));
            maxError = std::max(maxError, std::abs(expected - driver.right[static_cast<std::size_t>(i)]));
        }
        rendered += static_cast<std::size_t>(frames);
    }
    CHECK(maxError < 1e-6);
    CHECK(eng.stats().frames == rendered);
    CHECK(eng.stats().planSwaps >= 50);
}

TEST("Engine: parallel rendering is bit-identical to serial, with swaps and gain ramps") {
    constexpr int kMaxFrames = 128;
    auto runScript = [&](int workers) {
        engine::Engine eng;
        eng.prepare(48000.0, kMaxFrames, 2);
        if (workers > 0)
            eng.workers().start(workers, 2'666'666);
        engine::SpikeCompiler compiler;
        OfflineDriver driver{eng, {}, {}};
        std::vector<float> all;
        std::mt19937 rng(99);
        engine::SpikeScene scene;
        scene.loadVoices = 24;
        scene.loadPartials = 8;
        scene.loadAmplitude = 0.01f;
        eng.publish(compiler.compile(scene, kMaxFrames));
        for (int block = 0; block < 600; ++block) {
            if (block % 20 == 0) {
                scene.loadVoices = 8 + (block / 20) % 3 * 12;  // nodes come and go
                scene.toneFrequency = 220.0 * (1 + (block / 20) % 4);
                eng.publish(compiler.compile(scene, kMaxFrames));
                eng.collectGarbage();
            }
            if (block % 45 == 0)
                (void)eng.setMasterGain(0.5f + 0.1f * static_cast<float>(block % 5));
            driver.renderBlock(std::uniform_int_distribution<int>(16, 300)(rng));
            all.insert(all.end(), driver.left.begin(), driver.left.end());
        }
        eng.workers().stop();
        return all;
    };
    const auto serial = runScript(0);
    const auto parallel = runScript(3);
    CHECK(serial.size() == parallel.size());
    CHECK(serial == parallel);
    CHECK(engine::OscillatorNode::liveCount() == 0);  // every node freed with its last plan
}
