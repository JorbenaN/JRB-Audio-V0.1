// Plugin scanning: wire format and process isolation (no real plugins needed).

#include "Check.hpp"

#include "jrb/plugins/ScanClient.hpp"

#include <chrono>

using namespace jrb::plugins;

namespace {
ScanOptions fakeOptions(double timeout = 3.0) {
    ScanOptions o;
    o.scannerPath = JRB_FAKE_SCANNER;
    o.timeoutSeconds = timeout;
    return o;
}
} // namespace

TEST("plugin scan wire format: round-trips every field, including awkward characters") {
    ScanResult r;
    r.format = Format::audioUnit;
    r.fileOrIdentifier = "AudioUnit:Effects/aufx,dely,appl";
    r.status = ScanStatus::ok;
    r.message = "line one\nline\ttwo \\ end";
    PluginInfo p;
    p.format = Format::audioUnit;
    p.name = "AUDelay";
    p.vendor = "Apple";
    p.version = "1.7";
    p.category = "Effect";
    p.fileOrIdentifier = r.fileOrIdentifier;
    p.uniqueId = "AudioUnit-AUDelay-abcd";
    p.isInstrument = false;
    p.acceptsMidi = false;
    p.producesMidi = true;
    p.inputChannels = 2;
    p.outputChannels = 2;
    p.loadToken = "<PLUGIN name=\"AUDelay\"\n file=\"x\"/>";
    r.plugins = {p, p};
    ScanResult back;
    CHECK(decodeScanResult(encodeScanResult(r), back));
    CHECK(back.format == r.format && back.fileOrIdentifier == r.fileOrIdentifier && back.message == r.message);
    CHECK(back.plugins.size() == 2);
    const auto& q = back.plugins[1];
    CHECK(q.name == p.name && q.vendor == p.vendor && q.version == p.version && q.category == p.category);
    CHECK(q.uniqueId == p.uniqueId && q.loadToken == p.loadToken && q.fileOrIdentifier == p.fileOrIdentifier);
    CHECK(!q.isInstrument && !q.acceptsMidi && q.producesMidi && q.inputChannels == 2 && q.outputChannels == 2);
}

TEST("plugin scan: a healthy scanner result is decoded") {
    const auto r = scanPlugin(fakeOptions(), Format::vst3, "ok");
    CHECK(r.status == ScanStatus::ok);
    CHECK(r.plugins.size() == 1 && r.plugins[0].name == "Fake\tSynth\nwith \\ odd chars");
}

TEST("plugin scan: crashes, aborts, exits, garbage and truncation are contained and explained") {
    const auto crash = scanPlugin(fakeOptions(), Format::vst3, "crash");
    CHECK(crash.status == ScanStatus::crashed && !crash.message.empty());
    const auto abortRun = scanPlugin(fakeOptions(), Format::vst3, "abort");
    CHECK(abortRun.status == ScanStatus::crashed);
    const auto exitRun = scanPlugin(fakeOptions(), Format::vst3, "exit");
    CHECK(exitRun.status == ScanStatus::failed && exitRun.message.find("exit(1)") != std::string::npos);
    const auto garbage = scanPlugin(fakeOptions(), Format::vst3, "garbage");
    CHECK(garbage.status == ScanStatus::failed && garbage.plugins.empty());
    const auto truncated = scanPlugin(fakeOptions(), Format::vst3, "truncated");
    CHECK(truncated.status == ScanStatus::failed);
}

TEST("plugin scan: a hanging plugin is killed at the timeout") {
    const auto start = std::chrono::steady_clock::now();
    const auto r = scanPlugin(fakeOptions(0.5), Format::vst3, "hang");
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(r.status == ScanStatus::timedOut);
    CHECK(seconds < 2.0);
}

TEST("plugin scan: parallel scanning keeps results in input order") {
    auto options = fakeOptions(1.0);
    options.parallelScans = 4;
    const std::vector<std::string> candidates{"ok", "crash", "garbage", "ok", "hang", "ok", "exit", "ok"};
    const auto results = scanPlugins(options, Format::vst3, candidates);
    CHECK(results.size() == candidates.size());
    const ScanStatus expected[] = {ScanStatus::ok, ScanStatus::crashed, ScanStatus::failed, ScanStatus::ok,
                                   ScanStatus::timedOut, ScanStatus::ok, ScanStatus::failed, ScanStatus::ok};
    bool ordered = true;
    for (std::size_t i = 0; i < results.size(); ++i)
        ordered &= results[i].status == expected[i] && results[i].fileOrIdentifier == candidates[i];
    CHECK(ordered);
}
