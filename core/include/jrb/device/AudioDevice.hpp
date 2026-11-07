#pragma once

// Audio device abstraction (docs/02-architecture.md §3, "device/"). The engine
// depends only on this interface; JUCE stays inside the backend.

#include "jrb/rt/Realtime.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace jrb::device {

/// Implemented by the engine. Called by the device.
class AudioCallback {
public:
    virtual ~AudioCallback() = default;
    /// Before the first render, on a non-real-time thread. May allocate.
    virtual void prepare(double sampleRate, int maxFrames, int outputChannels) = 0;
    /// The real-time callback.
    virtual void render(float* const* outputs, int outputChannels, int frames) noexcept JRB_NONBLOCKING = 0;
    /// After the last render.
    virtual void stopped() = 0;
};

/// Holds a thread's membership in the device's real-time workgroup. Must be
/// created and destroyed on that same thread.
class WorkgroupMembership {
public:
    virtual ~WorkgroupMembership() = default;
    [[nodiscard]] virtual bool joined() const noexcept = 0;
};

struct OpenRequest {
    std::string outputDeviceName;  // empty = system default output
    double sampleRate = 48000.0;
    int bufferSize = 128;
    int outputChannels = 2;
};

struct DeviceInfo {
    std::string name;
    double sampleRate = 0;
    int bufferSize = 0;
    int outputChannels = 0;
    int outputLatencyFrames = 0;
    std::vector<double> availableSampleRates;
    std::vector<int> availableBufferSizes;
};

class AudioDevice {
public:
    virtual ~AudioDevice() = default;

    virtual bool open(const OpenRequest& request, std::string& error) = 0;
    virtual bool start(AudioCallback& callback, std::string& error) = 0;
    virtual void stop() = 0;
    virtual void close() = 0;

    [[nodiscard]] virtual DeviceInfo info() const = 0;
    /// Overloads reported by the OS since start (missed deadlines anywhere in the
    /// chain). -1 if this device cannot report them. Read while running.
    [[nodiscard]] virtual int xrunCount() const = 0;
    /// Recommended number of threads (including the I/O thread) for parallel DSP.
    [[nodiscard]] virtual std::size_t maxParallelThreads() const = 0;
    /// Joins the calling thread to the running device's workgroup (macOS 11+).
    [[nodiscard]] virtual std::unique_ptr<WorkgroupMembership> joinWorkgroup() = 0;

    [[nodiscard]] virtual std::vector<std::string> outputDeviceNames() const = 0;
};

/// The platform's primary backend (Core Audio on macOS), implemented with JUCE.
std::unique_ptr<AudioDevice> createSystemAudioDevice();

} // namespace jrb::device
