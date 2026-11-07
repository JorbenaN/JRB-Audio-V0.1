#pragma once

// Third-party plugin hosting (docs/04-plugin-hosting.md). Format-agnostic and
// JUCE-free: the JUCE adapter lives in src/plugins/JucePluginHost.cpp.
//
// Threading:
//  - PluginHost and everything except process() is main-thread only.
//  - process() is called on the audio thread. It is *not* JRB_NONBLOCKING:
//    third-party code cannot be verified, so the engine calls it through an
//    audited escape hatch and measures it instead.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jrb::plugins {

enum class Format : std::uint8_t { audioUnit, vst3 };

[[nodiscard]] const char* formatName(Format format) noexcept;
[[nodiscard]] bool parseFormat(const std::string& name, Format& out) noexcept;

/// Everything known about a plugin without loading it again. Produced by the
/// out-of-process scanner and cached.
struct PluginInfo {
    Format format = Format::vst3;
    std::string name;
    std::string vendor;
    std::string version;
    std::string category;
    std::string fileOrIdentifier;  // VST3 bundle path, or AU component identifier
    std::string uniqueId;          // stable across rescans: used to match saved projects to plugins
    bool isInstrument = false;
    bool acceptsMidi = false;
    bool producesMidi = false;
    int inputChannels = 0;
    int outputChannels = 0;
    std::string loadToken;         // opaque, backend-specific; needed to instantiate
};

enum class ScanStatus : std::uint8_t { ok, failed, crashed, timedOut };
[[nodiscard]] const char* scanStatusName(ScanStatus status) noexcept;

struct ScanResult {
    Format format = Format::vst3;
    std::string fileOrIdentifier;
    ScanStatus status = ScanStatus::failed;
    std::string message;           // human-readable reason when not ok
    std::vector<PluginInfo> plugins;
    double seconds = 0;
};

struct ParameterInfo {
    std::string name;
    bool automatable = true;
    bool readOnly = false;     // meters and other outputs: never automated, never part of user state
    bool discrete = false;
    int steps = 0;             // for discrete parameters
};

struct MidiEvent {
    int sampleOffset = 0;
    std::uint8_t bytes[3] = {};
    std::uint8_t size = 0;
};

/// A plugin's GUI, attached to a host-provided native view. Main thread only.
class PluginEditor {
public:
    virtual ~PluginEditor() = default;
    [[nodiscard]] virtual int width() const = 0;
    [[nodiscard]] virtual int height() const = 0;
};

class PluginInstance {
public:
    virtual ~PluginInstance() = default;

    [[nodiscard]] virtual const PluginInfo& info() const = 0;

    /// Non-real-time. Must be called before process() and after any format change.
    virtual bool prepare(double sampleRate, int maxBlockSize, std::string& error) = 0;
    virtual void release() = 0;
    virtual void setOffline(bool offline) = 0;

    /// Audio thread. `channels` holds max(inputs, outputs) buffers of `frames`
    /// samples, processed in place.
    virtual void process(float* const* channels, int numChannels, int frames, const MidiEvent* events,
                         int numEvents) noexcept = 0;

    [[nodiscard]] virtual int inputChannels() const = 0;
    [[nodiscard]] virtual int outputChannels() const = 0;
    [[nodiscard]] virtual int latencySamples() const = 0;
    [[nodiscard]] virtual double tailSeconds() const = 0;

    /// Opaque state for saving into a project. Never interpreted by the host.
    [[nodiscard]] virtual std::vector<std::uint8_t> state() = 0;
    virtual bool setState(const std::vector<std::uint8_t>& state) = 0;

    [[nodiscard]] virtual int parameterCount() const = 0;
    [[nodiscard]] virtual ParameterInfo parameterInfo(int index) const = 0;
    [[nodiscard]] virtual float parameterValue(int index) const = 0;   // normalised 0...1
    virtual void setParameterValue(int index, float value) = 0;

    [[nodiscard]] virtual bool hasEditor() const = 0;
    /// Embeds the plugin's GUI in `parentView` (an NSView* on macOS). Null on failure.
    [[nodiscard]] virtual std::unique_ptr<PluginEditor> openEditor(void* parentView) = 0;
};

/// Discovery and instantiation. Main thread only.
class PluginHost {
public:
    virtual ~PluginHost() = default;

    /// Candidate files/identifiers for a format, found without loading any code.
    [[nodiscard]] virtual std::vector<std::string> discover(Format format,
                                                            const std::vector<std::string>& extraFolders) = 0;

    /// Loads the candidate to describe it. Dangerous: only the scanner process calls this.
    [[nodiscard]] virtual ScanResult describe(Format format, const std::string& fileOrIdentifier) = 0;

    [[nodiscard]] virtual std::unique_ptr<PluginInstance> instantiate(const PluginInfo& info, double sampleRate,
                                                                      int maxBlockSize, std::string& error) = 0;
};

/// JUCE-backed host. Which flavour you get depends on the library linked:
/// jrb_juce (with plugin GUIs) or jrb_juce_headless (scanner; openEditor returns null).
std::unique_ptr<PluginHost> createPluginHost();

} // namespace jrb::plugins
