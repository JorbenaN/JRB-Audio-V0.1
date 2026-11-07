#include "jrb/plugins/ScanClient.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace jrb::plugins {

const char* formatName(Format format) noexcept {
    switch (format) {
    case Format::audioUnit: return "au";
    case Format::vst3: return "vst3";
    }
    return "?";
}

bool parseFormat(const std::string& name, Format& out) noexcept {
    if (name == "au") { out = Format::audioUnit; return true; }
    if (name == "vst3") { out = Format::vst3; return true; }
    return false;
}

const char* scanStatusName(ScanStatus status) noexcept {
    switch (status) {
    case ScanStatus::ok: return "ok";
    case ScanStatus::failed: return "failed";
    case ScanStatus::crashed: return "crashed";
    case ScanStatus::timedOut: return "timed out";
    }
    return "?";
}

// MARK: Wire format

namespace {

std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

std::string unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char n = s[++i];
            out += n == 't' ? '\t' : n == 'n' ? '\n' : n;
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace

std::string encodeScanResult(const ScanResult& r) {
    std::ostringstream o;
    o << "format\t" << formatName(r.format) << "\n";
    o << "file\t" << escape(r.fileOrIdentifier) << "\n";
    o << "status\t" << static_cast<int>(r.status) << "\n";
    o << "message\t" << escape(r.message) << "\n";
    for (const auto& p : r.plugins) {
        o << "plugin\t" << escape(p.name) << "\n";
        o << "vendor\t" << escape(p.vendor) << "\n";
        o << "version\t" << escape(p.version) << "\n";
        o << "category\t" << escape(p.category) << "\n";
        o << "pluginFile\t" << escape(p.fileOrIdentifier) << "\n";
        o << "uid\t" << escape(p.uniqueId) << "\n";
        o << "flags\t" << p.isInstrument << p.acceptsMidi << p.producesMidi << "\n";
        o << "io\t" << p.inputChannels << " " << p.outputChannels << "\n";
        o << "token\t" << escape(p.loadToken) << "\n";
    }
    o << "end\t\n";
    return o.str();
}

bool decodeScanResult(const std::string& text, ScanResult& out) {
    std::istringstream in(text);
    std::string line;
    bool sawEnd = false;
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos)
            continue;
        const std::string key = line.substr(0, tab);
        const std::string value = unescape(line.substr(tab + 1));
        PluginInfo* p = out.plugins.empty() ? nullptr : &out.plugins.back();
        if (key == "format") { if (!parseFormat(value, out.format)) return false; }
        else if (key == "file") out.fileOrIdentifier = value;
        else if (key == "status") out.status = static_cast<ScanStatus>(std::clamp(std::atoi(value.c_str()), 0, 3));
        else if (key == "message") out.message = value;
        else if (key == "plugin") { out.plugins.emplace_back(); out.plugins.back().name = value; out.plugins.back().format = out.format; }
        else if (p && key == "vendor") p->vendor = value;
        else if (p && key == "version") p->version = value;
        else if (p && key == "category") p->category = value;
        else if (p && key == "pluginFile") p->fileOrIdentifier = value;
        else if (p && key == "uid") p->uniqueId = value;
        else if (p && key == "flags" && value.size() == 3) {
            p->isInstrument = value[0] == '1';
            p->acceptsMidi = value[1] == '1';
            p->producesMidi = value[2] == '1';
        }
        else if (p && key == "io") std::istringstream(value) >> p->inputChannels >> p->outputChannels;
        else if (p && key == "token") p->loadToken = value;
        else if (key == "end") sawEnd = true;
    }
    return sawEnd;  // a truncated result (process died mid-write) is rejected
}

// MARK: Process handling

namespace {

struct ChildResult {
    std::string out, err;
    bool timedOut = false;
    bool exited = false;
    int exitCode = -1;
    int signal = 0;
    double seconds = 0;
};

ChildResult runScanner(const std::vector<std::string>& args, double timeoutSeconds) {
    ChildResult result;
    const auto start = std::chrono::steady_clock::now();
    int outPipe[2], errPipe[2];
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        result.err = "Could not create pipes.";
        return result;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, outPipe[0]);
    posix_spawn_file_actions_addclose(&actions, errPipe[0]);

    std::vector<char*> argv;
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(outPipe[1]);
    close(errPipe[1]);
    if (spawned != 0) {
        close(outPipe[0]);
        close(errPipe[0]);
        result.err = std::string("Could not start the plugin scanner: ") + std::strerror(spawned);
        return result;
    }

    pollfd fds[2] = {{outPipe[0], POLLIN, 0}, {errPipe[0], POLLIN, 0}};
    bool open[2] = {true, true};
    char buffer[4096];
    while (open[0] || open[1]) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeoutSeconds) {
            kill(pid, SIGKILL);
            result.timedOut = true;
            break;
        }
        for (int i = 0; i < 2; ++i)
            fds[i].fd = open[i] ? (i == 0 ? outPipe[0] : errPipe[0]) : -1;
        if (poll(fds, 2, 50) <= 0)
            continue;
        for (int i = 0; i < 2; ++i) {
            if (!open[i] || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            const ssize_t n = read(fds[i].fd, buffer, sizeof buffer);
            if (n > 0) (i == 0 ? result.out : result.err).append(buffer, static_cast<std::size_t>(n));
            else open[i] = false;
        }
    }
    close(outPipe[0]);
    close(errPipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        result.exited = true;
        result.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.signal = WTERMSIG(status);
    }
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}

std::string lastLines(const std::string& text, std::size_t maxChars = 300) {
    if (text.size() <= maxChars) return text;
    return "…" + text.substr(text.size() - maxChars);
}

} // namespace

std::vector<std::string> discoverPlugins(const ScanOptions& options, Format format, std::string& error) {
    std::vector<std::string> args{options.scannerPath, "discover", formatName(format)};
    args.insert(args.end(), options.extraFolders.begin(), options.extraFolders.end());
    const auto child = runScanner(args, 60.0);
    std::vector<std::string> out;
    if (!child.exited || child.exitCode != 0) {
        error = "Plugin discovery failed: " + lastLines(child.err);
        return out;
    }
    std::istringstream in(child.out);
    std::string line;
    while (std::getline(in, line))
        if (!line.empty())
            out.push_back(line);
    return out;
}

ScanResult scanPlugin(const ScanOptions& options, Format format, const std::string& fileOrIdentifier) {
    const auto child = runScanner({options.scannerPath, "scan", formatName(format), fileOrIdentifier}, options.timeoutSeconds);
    ScanResult result;
    result.format = format;
    result.fileOrIdentifier = fileOrIdentifier;
    result.seconds = child.seconds;
    if (child.timedOut) {
        result.status = ScanStatus::timedOut;
        result.message = "The plugin did not finish loading within " + std::to_string(static_cast<int>(options.timeoutSeconds)) +
                         " seconds.";
    } else if (child.signal != 0) {
        result.status = ScanStatus::crashed;
        result.message = std::string("The plugin crashed while loading (") + strsignal(child.signal) + ").";
    } else if (!decodeScanResult(child.out, result)) {
        result.status = ScanStatus::failed;
        result.message = "The scanner returned no usable result (exit code " + std::to_string(child.exitCode) + "). " +
                         lastLines(child.err);
    }
    result.fileOrIdentifier = fileOrIdentifier;
    result.seconds = child.seconds;
    return result;
}

std::vector<ScanResult> scanPlugins(const ScanOptions& options, Format format, const std::vector<std::string>& candidates) {
    std::vector<ScanResult> results(candidates.size());
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> threads;
    const int workers = std::max(1, std::min<int>(options.parallelScans, static_cast<int>(candidates.size())));
    for (int w = 0; w < workers; ++w) {
        threads.emplace_back([&] {
            for (std::size_t i = next++; i < candidates.size(); i = next++)
                results[i] = scanPlugin(options, format, candidates[i]);
        });
    }
    for (auto& t : threads)
        t.join();
    return results;
}

} // namespace jrb::plugins
