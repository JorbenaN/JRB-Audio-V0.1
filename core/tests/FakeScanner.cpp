// Test double for jrb-plugin-scanner: `scan <format> <behaviour>`.
#include "jrb/plugins/ScanClient.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace jrb::plugins;

int main(int argc, char** argv) {
    if (argc < 4) return 64;
    const std::string what = argv[3];
    if (what == "crash") std::raise(SIGSEGV);
    if (what == "abort") std::abort();
    if (what == "hang") std::this_thread::sleep_for(std::chrono::hours(1));
    if (what == "garbage") { std::printf("this is not a scan result\n"); return 0; }
    if (what == "truncated") {
        ScanResult r;
        r.status = ScanStatus::ok;
        r.plugins.push_back({});
        std::string text = encodeScanResult(r);
        text.resize(text.size() / 2);  // dies mid-write
        std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }
    if (what == "exit") { std::fprintf(stderr, "plugin called exit(1)\n"); std::_Exit(1); }
    ScanResult r;
    r.format = Format::vst3;
    r.fileOrIdentifier = what;
    r.status = ScanStatus::ok;
    PluginInfo p;
    p.format = Format::vst3;
    p.name = "Fake\tSynth\nwith \\ odd chars";
    p.vendor = "JRB";
    p.uniqueId = "VST3-Fake-1234";
    p.isInstrument = true;
    p.acceptsMidi = true;
    p.inputChannels = 0;
    p.outputChannels = 2;
    p.loadToken = "<PLUGIN name=\"Fake\"/>";
    r.plugins.push_back(p);
    const std::string text = encodeScanResult(r);
    std::fwrite(text.data(), 1, text.size(), stdout);
    return 0;
}
