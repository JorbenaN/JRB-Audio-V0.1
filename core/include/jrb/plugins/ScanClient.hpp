#pragma once

// Runs the out-of-process plugin scanner (docs/04 §3). Every candidate is
// loaded in its own short-lived jrb-plugin-scanner process, so a plugin that
// crashes, hangs or calls exit() can never take the app down; it is recorded
// as crashed / timed out instead.

#include "jrb/plugins/Plugin.hpp"

#include <string>
#include <vector>

namespace jrb::plugins {

struct ScanOptions {
    std::string scannerPath;            // path to the jrb-plugin-scanner executable
    double timeoutSeconds = 15.0;
    int parallelScans = 4;
    std::vector<std::string> extraFolders;
};

/// Lists candidates for a format (runs the scanner's cheap `discover` command).
std::vector<std::string> discoverPlugins(const ScanOptions& options, Format format, std::string& error);

/// Scans one candidate in a fresh process.
ScanResult scanPlugin(const ScanOptions& options, Format format, const std::string& fileOrIdentifier);

/// Scans many candidates, `parallelScans` processes at a time. Results keep input order.
std::vector<ScanResult> scanPlugins(const ScanOptions& options, Format format, const std::vector<std::string>& candidates);

// Wire format between scanner and client: one "key<TAB>value" pair per line,
// values escaped (\\, \t, \n). Plain text so a corrupted result can be read by hand.
std::string encodeScanResult(const ScanResult& result);
bool decodeScanResult(const std::string& text, ScanResult& out);

} // namespace jrb::plugins
