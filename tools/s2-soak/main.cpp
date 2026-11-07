// Spike S2 soak test: renders through the real audio device while plans are
// swapped and parameters changed continuously, then checks the result.
//
//   jrb_s2_soak [--seconds 30] [--buffer 64] [--rate 48000] [--load 48] [--partials 16]
//               [--swap-ms 50] [--workers N] [--device "Name"] [--audible]
//
// By default the device plays silence while the engine's real mix is captured
// and analysed. --audible plays a quiet test tone (about -26 dBFS) that switches
// between A4 and E5 on every plan swap.

#include "jrb/device/AudioDevice.hpp"
#include "jrb/engine/Engine.hpp"
#include "jrb/engine/SpikeScene.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

using namespace jrb;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    double seconds = 30;
    int buffer = 64;
    double rate = 48000;
    int load = 48;
    int partials = 16;
    int swapMs = 50;
    int workers = -1;  // auto
    std::string device;
    bool audible = false;
    int injectEveryMs = 0;  // > 0: deliberately overrun one callback this often (verifies xrun detection)
};

/// Wraps the engine; can deliberately miss a deadline to prove overloads are detected.
class InjectingCallback final : public device::AudioCallback {
public:
    InjectingCallback(engine::Engine& engine, int everyMs) : engine_(engine), everyMs_(everyMs) {}
    void prepare(double rate, int maxFrames, int channels) override {
        rate_ = rate;
        engine_.prepare(rate, maxFrames, channels);
    }
    void render(float* const* out, int channels, int frames) noexcept JRB_NONBLOCKING override {
        engine_.render(out, channels, frames);
        if (everyMs_ <= 0)
            return;
        elapsedFrames_ += frames;
        if (elapsedFrames_ >= rate_ * everyMs_ / 1000.0) {
            elapsedFrames_ = 0;
            injected_.fetch_add(1, std::memory_order_relaxed);
            const std::uint64_t until = rt::nowNanos() + static_cast<std::uint64_t>(3e9 * frames / rate_);
            while (rt::nowNanos() < until)
                rt::cpuPause();  // three block durations: guaranteed miss
        }
    }
    void stopped() override { engine_.stopped(); }
    [[nodiscard]] int injected() const noexcept { return injected_.load(); }

private:
    engine::Engine& engine_;
    int everyMs_;
    double rate_ = 48000.0;
    double elapsedFrames_ = 0;
    std::atomic<int> injected_{0};
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        const std::string a = argv[i];
        if (a == "--seconds") o.seconds = std::stod(next());
        else if (a == "--buffer") o.buffer = std::stoi(next());
        else if (a == "--rate") o.rate = std::stod(next());
        else if (a == "--load") o.load = std::stoi(next());
        else if (a == "--partials") o.partials = std::stoi(next());
        else if (a == "--swap-ms") o.swapMs = std::stoi(next());
        else if (a == "--workers") o.workers = std::stoi(next());
        else if (a == "--device") o.device = next();
        else if (a == "--audible") o.audible = true;
        else if (a == "--inject-overload-every-ms") o.injectEveryMs = std::stoi(next());
        else std::fprintf(stderr, "Unknown option %s\n", a.c_str());
    }
    return o;
}

} // namespace

int main(int argc, char** argv) {
    const Options opt = parse(argc, argv);

    auto device = device::createSystemAudioDevice();
    std::string error;
    device::OpenRequest request;
    request.outputDeviceName = opt.device;
    request.sampleRate = opt.rate;
    request.bufferSize = opt.buffer;
    if (!device->open(request, error)) {
        std::fprintf(stderr, "Could not open audio device: %s\n", error.c_str());
        return 2;
    }
    const auto info = device->info();
    std::printf("Device:   %s\n", info.name.c_str());
    std::printf("Format:   %.0f Hz, %d-frame buffer (requested %.0f Hz, %d), %d outputs, output latency %d frames\n",
                info.sampleRate, info.bufferSize, opt.rate, opt.buffer, info.outputChannels, info.outputLatencyFrames);

    engine::Engine engine;
    engine.setOutputMuted(!opt.audible);
    rt::SpscQueue<float> capture(1u << 18);
    engine.setCapture(&capture);

    InjectingCallback callback(engine, opt.injectEveryMs);
    if (!device->start(callback, error)) {
        std::fprintf(stderr, "Could not start audio device: %s\n", error.c_str());
        return 2;
    }

    const double sampleRate = engine.sampleRate();
    const int maxFrames = std::max(engine.maxFrames(), info.bufferSize);
    const auto periodNanos = static_cast<std::uint64_t>(1e9 * info.bufferSize / sampleRate);
    const std::size_t recommended = device->maxParallelThreads();
    const int workers = opt.workers >= 0 ? opt.workers : static_cast<int>(std::min<std::size_t>(8, recommended > 1 ? recommended - 1 : 0));
    engine.workers().start(workers, periodNanos, [&] { return device->joinWorkgroup(); });

    engine::SpikeCompiler compiler;
    engine::SpikeScene scene;
    scene.toneAmplitude = 0.05f;  // about -26 dBFS
    scene.loadVoices = opt.load;
    scene.loadPartials = opt.partials;
    engine.publish(compiler.compile(scene, maxFrames));

    std::printf("Engine:   %d workers (device recommends %zu threads), %d load voices x %d partials, plan swap every %d ms, %s\n",
                workers, recommended, opt.load, opt.partials, opt.swapMs, opt.audible ? "AUDIBLE" : "muted output");
    std::printf("Running for %.0f s...\n", opt.seconds);
    std::fflush(stdout);

    std::vector<float> captured;
    captured.reserve(static_cast<std::size_t>(sampleRate * (opt.seconds + 2)));
    auto drain = [&] {
        float v = 0;
        while (capture.pop(v))
            captured.push_back(v);
    };

    const auto start = Clock::now();
    auto nextSwap = start;
    auto nextGain = start;
    bool high = false;
    bool loud = true;
    std::size_t published = 1;
    while (Clock::now() - start < std::chrono::duration<double>(opt.seconds)) {
        const auto now = Clock::now();
        if (now >= nextSwap) {
            high = !high;
            scene.toneFrequency = high ? 659.25 : 440.0;
            scene.loadVoices = opt.load / 2 + static_cast<int>(published % 2) * (opt.load - opt.load / 2);
            engine.publish(compiler.compile(scene, maxFrames));
            ++published;
            nextSwap += std::chrono::milliseconds(opt.swapMs);
        }
        if (now >= nextGain) {
            loud = !loud;
            (void)engine.setMasterGain(loud ? 1.0f : 0.8f);
            nextGain += std::chrono::milliseconds(200);
        }
        drain();
        engine.collectGarbage();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const int xruns = device->xrunCount();  // must be read while running; JUCE reports -1 once stopped
    device->stop();
    engine.workers().stop();
    drain();
    const auto stats = engine.stats();
    const int realtimeWorkers = engine.workers().workersRealtime();
    const int joinedWorkers = engine.workers().workersInWorkgroup();
    const auto workerJobs = engine.workers().jobsRunByWorkers();
    device->close();

    // Continuity: the largest sample-to-sample step the mix can legitimately make.
    double harmonic = 0, loadFreqSum = 0;
    for (int k = 1; k <= opt.partials; ++k)
        harmonic += 1.0 / k;
    for (int v = 0; v < opt.load; ++v)
        loadFreqSum += engine::SpikeCompiler::loadFrequency(v);
    const double twoPi = 2.0 * std::numbers::pi;
    const double peak = scene.toneAmplitude + opt.load * scene.loadAmplitude * harmonic;
    const double bound = 1.05 * (scene.toneAmplitude * twoPi * 659.25 / sampleRate +
                                 scene.loadAmplitude * opt.partials * twoPi * loadFreqSum / sampleRate +
                                 0.2 * peak / std::max(1, info.bufferSize < 16 ? 1 : 16));
    double maxStep = 0;
    std::size_t discontinuities = 0;
    for (std::size_t i = 1; i < captured.size(); ++i) {
        const double step = std::abs(static_cast<double>(captured[i]) - captured[i - 1]);
        maxStep = std::max(maxStep, step);
        discontinuities += step > bound;
    }

    const double budgetUs = 1e6 * info.bufferSize / sampleRate;
    const double avgUs = stats.callbacks ? stats.totalCallbackNanos / 1e3 / static_cast<double>(stats.callbacks) : 0;
    std::printf("\nResults\n");
    std::printf("  callbacks            %llu (%.1f s of audio)\n", static_cast<unsigned long long>(stats.callbacks),
                static_cast<double>(stats.frames) / sampleRate);
    std::printf("  callback time        avg %.1f us, max %.1f us, budget %.1f us (avg load %.0f%%)\n", avgUs,
                static_cast<double>(stats.maxCallbackNanos) / 1e3, budgetUs, 100.0 * avgUs / budgetUs);
    std::printf("  engine overruns      %llu\n", static_cast<unsigned long long>(stats.overruns));
    std::printf("  device xruns         %d%s\n", xruns, xruns < 0 ? " (not measurable on this device)" : "");
    if (opt.injectEveryMs > 0)
        std::printf("  injected overloads   %d (device must report at least this many)\n", callback.injected());
    std::printf("  plan swaps           %llu of %zu published (rest superseded before pickup)\n",
                static_cast<unsigned long long>(stats.planSwaps), published);
    std::printf("  gain changes         %llu\n", static_cast<unsigned long long>(stats.paramChanges));
    std::printf("  workers              %d real-time, %d in the device workgroup, %llu jobs run by workers\n",
                realtimeWorkers, joinedWorkers, static_cast<unsigned long long>(workerJobs));
    std::printf("  captured             %zu samples, %llu dropped\n", captured.size(),
                static_cast<unsigned long long>(stats.captureDropped));
    std::printf("  continuity           max step %.6f, bound %.6f, %zu discontinuities\n", maxStep, bound, discontinuities);

    engine.publish(nullptr);
    engine.collectGarbage();

    const bool injecting = opt.injectEveryMs > 0;
    const bool xrunsOk = injecting ? xruns >= callback.injected() && callback.injected() > 0 : xruns == 0;
    const bool pass = xrunsOk && (injecting || stats.overruns == 0) && (injecting || discontinuities == 0) &&
                      stats.captureDropped == 0 && joinedWorkers == workers && stats.planSwaps > 0 &&
                      stats.paramChanges > 0;
    std::printf("\nS2 soak%s: %s\n", injecting ? " (overload-detection check)" : "", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
