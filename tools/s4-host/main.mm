// Spike S4: plugin hosting proof.
//
//   jrb_s4_host --scanner <path> [--vst3-folder <dir>]... [--only <text>]... [--skip <text>]...
//               [--show-gui] [--snapshots <dir>]
//
// 1. Scans every AU and VST3 plugin out of process (jrb-plugin-scanner).
// 2. For each plugin that scanned OK (after --only/--skip filters), in process:
//    load → prepare → process 400 blocks on a separate audio thread (effects get
//    a sine, instruments get a chord) → check output → save state → load a second
//    instance → restore state → compare → open the GUI inside our own NSView 3×.

#import <AppKit/AppKit.h>

#include "jrb/plugins/Plugin.hpp"
#include "jrb/plugins/ScanClient.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace jrb::plugins;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    std::string scanner;
    std::vector<std::string> vst3Folders;
    std::vector<std::string> only;
    std::vector<std::string> skip;
    bool showGui = false;
    std::string snapshots;
};

double ms(Clock::time_point since) { return std::chrono::duration<double, std::milli>(Clock::now() - since).count(); }

bool matches(const PluginInfo& p, const std::vector<std::string>& needles) {
    for (const auto& n : needles)
        if (p.name.find(n) != std::string::npos || p.vendor.find(n) != std::string::npos ||
            p.fileOrIdentifier.find(n) != std::string::npos)
            return true;
    return false;
}

void pump(double seconds) {
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
}

struct AudioCheck {
    bool ok = false;
    double rms = 0;
    double maxBlockMs = 0;
    double avgBlockMs = 0;
    bool finite = true;
    std::string note;
};

/// Runs `blocks` blocks of 512 frames at 48 kHz on a separate thread, as the
/// engine's audio thread would.
AudioCheck runAudio(PluginInstance& plugin, int blocks) {
    AudioCheck check;
    constexpr int kFrames = 512;
    const int channels = std::max({plugin.inputChannels(), plugin.outputChannels(), 2});
    std::vector<std::vector<float>> storage(static_cast<std::size_t>(channels), std::vector<float>(kFrames));
    std::vector<float*> ptrs;
    for (auto& c : storage) ptrs.push_back(c.data());
    const bool instrument = plugin.info().isInstrument || plugin.info().acceptsMidi;

    std::thread audio([&] {
        double phase = 0, sumSquares = 0, total = 0;
        long samples = 0;
        for (int b = 0; b < blocks; ++b) {
            for (int ch = 0; ch < channels; ++ch) {
                double ph = phase;
                for (int i = 0; i < kFrames; ++i) {
                    storage[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)] =
                        instrument || ch >= plugin.inputChannels() ? 0.0f : static_cast<float>(0.25 * std::sin(ph));
                    ph += 2 * M_PI * 440.0 / 48000.0;
                }
            }
            phase += kFrames * 2 * M_PI * 440.0 / 48000.0;
            std::vector<MidiEvent> events;
            if (instrument && b == 0)
                for (std::uint8_t note : {60, 64, 67}) events.push_back({0, {0x90, note, 100}, 3});
            if (instrument && b == blocks / 2)
                for (std::uint8_t note : {60, 64, 67}) events.push_back({0, {0x80, note, 0}, 3});
            const auto t0 = Clock::now();
            plugin.process(ptrs.data(), channels, kFrames, events.data(), static_cast<int>(events.size()));
            const double t = ms(t0);
            total += t;
            check.maxBlockMs = std::max(check.maxBlockMs, t);
            if (b < blocks / 2) {  // measure while the chord / sine is sounding
                for (int ch = 0; ch < std::min(channels, std::max(1, plugin.outputChannels())); ++ch)
                    for (float v : storage[static_cast<std::size_t>(ch)]) {
                        if (!std::isfinite(v)) check.finite = false;
                        sumSquares += static_cast<double>(v) * v;
                        ++samples;
                    }
            }
        }
        check.rms = samples ? std::sqrt(sumSquares / static_cast<double>(samples)) : 0;
        check.avgBlockMs = total / blocks;
    });
    audio.join();
    // Generators and mixers produce nothing without host-specific setup (a file,
    // a network stream, bus levels), so silence is expected and not a failure.
    const std::string& category = plugin.info().category;
    const bool silenceExpected = category.find("Generator") != std::string::npos || category.find("Mixer") != std::string::npos;
    check.ok = check.finite && (check.rms > 1e-5 || silenceExpected);
    if (!check.finite) check.note = "non-finite output";
    else if (check.rms <= 1e-5) check.note = silenceExpected ? "silent (" + category + ": expected)" : "silent output";
    return check;
}

void savePNG(NSView* view, const std::string& path) {
    NSBitmapImageRep* rep = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
    if (!rep) return;
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:rep];
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    [png writeToFile:[NSString stringWithUTF8String:path.c_str()] atomically:YES];
}

} // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        Options opt;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
            if (a == "--scanner") opt.scanner = next();
            else if (a == "--vst3-folder") opt.vst3Folders.push_back(next());
            else if (a == "--only") opt.only.push_back(next());
            else if (a == "--skip") opt.skip.push_back(next());
            else if (a == "--show-gui") opt.showGui = true;
            else if (a == "--snapshots") opt.snapshots = next();
        }
        if (opt.scanner.empty()) {
            std::fprintf(stderr, "--scanner <path to jrb-plugin-scanner> is required\n");
            return 64;
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:opt.showGui ? NSApplicationActivationPolicyRegular : NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];

        // 1. Out-of-process scan.
        std::vector<PluginInfo> found;
        int scanned = 0, failures = 0;
        for (Format format : {Format::audioUnit, Format::vst3}) {
            ScanOptions options;
            options.scannerPath = opt.scanner;
            options.extraFolders = format == Format::vst3 ? opt.vst3Folders : std::vector<std::string>{};
            std::string error;
            const auto t0 = Clock::now();
            const auto candidates = discoverPlugins(options, format, error);
            if (!error.empty()) std::printf("discover %s: %s\n", formatName(format), error.c_str());
            const auto results = scanPlugins(options, format, candidates);
            std::printf("Scanned %zu %s candidates out of process in %.1f s\n", candidates.size(), formatName(format),
                        ms(t0) / 1000);
            for (const auto& r : results) {
                ++scanned;
                if (r.status != ScanStatus::ok) {
                    ++failures;
                    std::printf("  %-9s %-60s %s\n", scanStatusName(r.status), r.fileOrIdentifier.c_str(), r.message.c_str());
                }
                for (const auto& p : r.plugins) found.push_back(p);
            }
        }
        std::printf("Scan: %d candidates, %zu plugins found, %d not loadable\n\n", scanned, found.size(), failures);
        for (const auto& p : found)
            std::printf("  %-5s %-28s %-22s %-10s %s in/out %d/%d\n", formatName(p.format), p.name.c_str(), p.vendor.c_str(),
                        p.version.c_str(), p.isInstrument ? "instrument" : "effect    ", p.inputChannels, p.outputChannels);

        // 2. In-process hosting tests.
        auto host = createPluginHost();
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 900, 640)
                                                       styleMask:(opt.showGui ? NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                                              : NSWindowStyleMaskBorderless)
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.releasedWhenClosed = NO;
        if (opt.showGui) [window center];
        else [window setFrameOrigin:NSMakePoint(-30000, -30000)];   // real window, off every screen
        [window orderFront:nil];

        std::printf("\n%-30s %-5s %7s | %-6s %8s %8s | %-6s %-12s | %-5s %-10s | %s\n", "Plugin", "fmt", "load ms", "audio",
                    "rms", "max ms", "state", "params", "gui", "size", "result");
        int tested = 0, passed = 0;
        for (const auto& info : found) {
            if (!opt.only.empty() && !matches(info, opt.only)) continue;
            if (matches(info, opt.skip)) continue;
            ++tested;
            std::string error;
            const auto t0 = Clock::now();
            auto plugin = host->instantiate(info, 48000, 512, error);
            const double loadMs = ms(t0);
            if (!plugin) {
                std::printf("%-30s %-5s %7.0f | load failed: %s\n", info.name.c_str(), formatName(info.format), loadMs, error.c_str());
                continue;
            }
            plugin->prepare(48000, 512, error);
            AudioCheck audio = runAudio(*plugin, 400);

            // State round trip: change a parameter, save, restore into a fresh instance.
            bool stateOk = true;
            std::string paramsNote = std::to_string(plugin->parameterCount());
            int firstWritable = -1, meters = 0;
            for (int i = 0; i < plugin->parameterCount(); ++i) {
                if (plugin->parameterInfo(i).readOnly) ++meters;
                else if (firstWritable < 0) firstWritable = i;
            }
            const double settle = std::getenv("JRB_SETTLE") ? std::atof(std::getenv("JRB_SETTLE")) : 0.05;
            if (firstWritable >= 0) {
                plugin->setParameterValue(firstWritable, 0.3f);
                pump(settle);
            }
            const auto saved = plugin->state();
            auto second = host->instantiate(info, 48000, 512, error);
            if (second) {
                second->prepare(48000, 512, error);
                second->setState(saved);
                pump(settle);
                const bool sameBytes = second->state() == saved;
                paramsNote += sameBytes ? "=" : "≠";
                float worst = 0;
                std::string worstName;
                const int n = std::min(plugin->parameterCount(), second->parameterCount());
                for (int i = 0; i < n; ++i) {
                    if (plugin->parameterInfo(i).readOnly) continue;   // meters are outputs, not state
                    const float d = std::abs(plugin->parameterValue(i) - second->parameterValue(i));
                    if (d > worst) {
                        worst = d;
                        worstName = plugin->parameterInfo(i).name + " " + std::to_string(plugin->parameterValue(i)) + "→" +
                                    std::to_string(second->parameterValue(i));
                    }
                }
                stateOk = worst < 1e-3f && plugin->parameterCount() == second->parameterCount();
                if (meters) paramsNote += "(" + std::to_string(meters) + "m)";
                paramsNote += stateOk ? " match" : " DIFFER";
                if (!stateOk) audio.note += " [" + worstName + "]";
            } else {
                stateOk = false;
                paramsNote += " no 2nd";
            }
            second.reset();

            // GUI: open inside our view three times.
            std::string gui = "none", size = "-";
            bool guiOk = true;
            if (plugin->hasEditor()) {
                gui = "ok";
                for (int round = 0; round < 3 && guiOk; ++round) {
                    NSView* container = [[NSView alloc] initWithFrame:window.contentView.bounds];
                    window.contentView = container;
                    auto editor = plugin->openEditor((__bridge void*)container);
                    pump(0.25);
                    if (!editor || editor->width() <= 0 || editor->height() <= 0 || container.subviews.count == 0) {
                        guiOk = false;
                        gui = "FAIL";
                        break;
                    }
                    size = std::to_string(editor->width()) + "x" + std::to_string(editor->height());
                    if (round == 0 && !opt.snapshots.empty()) {
                        [window setContentSize:NSMakeSize(editor->width(), editor->height())];
                        pump(0.3);
                        std::string file = info.name;
                        std::replace_if(file.begin(), file.end(), [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
                        savePNG(container, opt.snapshots + "/" + file + ".png");
                    }
                    editor.reset();
                    pump(0.05);
                }
            }

            const bool ok = audio.ok && stateOk && guiOk;
            passed += ok;
            std::printf("%-30s %-5s %7.0f | %-6s %8.4f %8.3f | %-6s %-12s | %-5s %-10s | %s%s%s\n", info.name.substr(0, 30).c_str(),
                        formatName(info.format), loadMs, audio.ok ? "ok" : "FAIL", audio.rms, audio.maxBlockMs,
                        stateOk ? "ok" : "FAIL", paramsNote.c_str(), gui.c_str(), size.c_str(), ok ? "PASS" : "FAIL ",
                        audio.note.c_str(), plugin->latencySamples() ? (" latency " + std::to_string(plugin->latencySamples())).c_str() : "");
            std::fflush(stdout);
            plugin.reset();
            pump(0.05);
        }
        [window close];
        std::printf("\nS4 hosting: %d of %d plugins passed (audio, state round trip, GUI)\n", passed, tested);
        std::fflush(stdout);
        std::_Exit(passed == tested && tested > 0 ? 0 : 1);   // skip static teardown of third-party code
    }
}
