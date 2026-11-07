// jrb-plugin-scanner: loads untrusted plugin binaries on behalf of the app, in
// a separate process (docs/04 §3). One process per plugin; the app treats a
// crash, a hang or garbage output as a failed scan, never as its own failure.
//
//   jrb-plugin-scanner discover <au|vst3> [extra folders...]   candidates, one per line (loads nothing)
//   jrb-plugin-scanner scan <au|vst3> <fileOrIdentifier>        loads one candidate, prints its description

#include "jrb/plugins/Plugin.hpp"
#include "jrb/plugins/ScanClient.hpp"

#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

using namespace jrb::plugins;

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s discover <au|vst3> [folders...] | scan <au|vst3> <fileOrIdentifier>\n", argv[0]);
        return 64;
    }
    const std::string command = argv[1];
    Format format{};
    if (!parseFormat(argv[2], format)) {
        std::fprintf(stderr, "Unknown plugin format: %s\n", argv[2]);
        return 64;
    }

    auto host = createPluginHost();
    if (command == "discover") {
        std::vector<std::string> folders(argv + 3, argv + argc);
        for (const auto& candidate : host->discover(format, folders))
            std::printf("%s\n", candidate.c_str());
        return 0;
    }
    if (command == "scan" && argc >= 4) {
        const ScanResult result = host->describe(format, argv[3]);
        const std::string encoded = encodeScanResult(result);
        std::fwrite(encoded.data(), 1, encoded.size(), stdout);
        std::fflush(stdout);
        // Exit without running static destructors: some plugins crash during
        // teardown, and the result has already been delivered.
        std::_Exit(result.status == ScanStatus::ok ? 0 : 3);
    }
    std::fprintf(stderr, "Unknown command: %s\n", command.c_str());
    return 64;
}
