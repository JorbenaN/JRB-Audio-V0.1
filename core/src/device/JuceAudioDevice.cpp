// JUCE-backed AudioDevice (Core Audio on macOS). The only file in the engine
// path that sees JUCE types (ADR-003).

#include "jrb/device/AudioDevice.hpp"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

#if JUCE_MAC
#include <CoreAudio/CoreAudio.h>
#endif

namespace jrb::device {
namespace {

class JuceWorkgroupMembership final : public WorkgroupMembership {
public:
    explicit JuceWorkgroupMembership(const juce::AudioWorkgroup& workgroup) {
        if (workgroup)
            workgroup.join(token_);
    }
    [[nodiscard]] bool joined() const noexcept override { return static_cast<bool>(token_); }

private:
    juce::WorkgroupToken token_;
};

class CallbackAdapter final : public juce::AudioIODeviceCallback {
public:
    explicit CallbackAdapter(AudioCallback& target) : target_(target) {}

    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override {
        target_.render(outputs, numOutputs, numSamples);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        target_.prepare(device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(),
                        device->getActiveOutputChannels().countNumberOfSetBits());
    }

    void audioDeviceStopped() override { target_.stopped(); }

private:
    AudioCallback& target_;
};

std::string toStd(const juce::String& s) { return s.toStdString(); }

#if JUCE_MAC
/// Counts Core Audio overload notifications (missed I/O deadlines).
///
/// Workaround for JUCE 9.0.5: AudioIODevice::getXRunCount() on macOS always
/// returns -1, because AudioDeviceProcessor::start() never sets its `running`
/// flag. JUCE also runs every device through a private aggregate device it
/// creates, so we listen on the physical output device and on any aggregate in
/// this process that contains it, and report the highest count.
class OverloadCounter {
public:
    ~OverloadCounter() { detach(); }

    void attach(const std::string& outputDeviceName) {
        detach();
        const auto devices = allDevices();
        AudioObjectID physical = kAudioObjectUnknown;
        for (auto id : devices)
            if (classOf(id) != kAudioAggregateDeviceClassID && nameOf(id) == outputDeviceName)
                physical = id;
        if (physical == kAudioObjectUnknown)
            return;
        watch(physical);
        for (auto id : devices)
            if (classOf(id) == kAudioAggregateDeviceClassID && aggregateContains(id, physical))
                watch(id);
    }

    void detach() {
        for (auto& [id, count] : counts_)
            AudioObjectRemovePropertyListener(id, &kOverload, &OverloadCounter::onOverload, count.get());
        counts_.clear();
    }

    [[nodiscard]] bool attached() const noexcept { return !counts_.empty(); }

    [[nodiscard]] int count() const noexcept {
        int highest = 0;
        for (const auto& entry : counts_)
            highest = std::max(highest, entry.second->load());
        return highest;
    }

private:
    static constexpr AudioObjectPropertyAddress kOverload{kAudioDeviceProcessorOverload, kAudioObjectPropertyScopeGlobal,
                                                          kAudioObjectPropertyElementMain};

    static OSStatus onOverload(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* client) {
        static_cast<std::atomic<int>*>(client)->fetch_add(1, std::memory_order_relaxed);
        return noErr;
    }

    void watch(AudioObjectID id) {
        auto counter = std::make_unique<std::atomic<int>>(0);
        if (AudioObjectAddPropertyListener(id, &kOverload, &OverloadCounter::onOverload, counter.get()) == noErr)
            counts_[id] = std::move(counter);
    }

    static std::vector<AudioObjectID> allDevices() {
        const AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain};
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr)
            return {};
        std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, ids.data()) != noErr)
            return {};
        ids.resize(size / sizeof(AudioObjectID));
        return ids;
    }

    static AudioClassID classOf(AudioObjectID id) {
        const AudioObjectPropertyAddress address{kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain};
        AudioClassID cls = 0;
        UInt32 size = sizeof(cls);
        return AudioObjectGetPropertyData(id, &address, 0, nullptr, &size, &cls) == noErr ? cls : 0;
    }

    static std::string nameOf(AudioObjectID id) {
        const AudioObjectPropertyAddress address{kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain};
        CFStringRef name = nullptr;
        UInt32 size = sizeof(name);
        if (AudioObjectGetPropertyData(id, &address, 0, nullptr, &size, &name) != noErr || name == nullptr)
            return {};
        const auto result = juce::String::fromCFString(name).toStdString();
        CFRelease(name);
        return result;
    }

    static bool aggregateContains(AudioObjectID aggregate, AudioObjectID device) {
        const AudioObjectPropertyAddress address{kAudioAggregateDevicePropertyActiveSubDeviceList,
                                                 kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(aggregate, &address, 0, nullptr, &size) != noErr)
            return false;
        std::vector<AudioObjectID> subs(size / sizeof(AudioObjectID));
        if (AudioObjectGetPropertyData(aggregate, &address, 0, nullptr, &size, subs.data()) != noErr)
            return false;
        return std::find(subs.begin(), subs.end(), device) != subs.end();
    }

    std::map<AudioObjectID, std::unique_ptr<std::atomic<int>>> counts_;
};
#endif

class JuceAudioDevice final : public AudioDevice {
public:
    JuceAudioDevice() {
        // JUCE's Core Audio backend posts device notifications to a message
        // manager; make sure one exists (on the thread that creates the device).
        static std::once_flag once;
        std::call_once(once, [] { juce::MessageManager::getInstance(); });
#if JUCE_MAC
        type_.reset(juce::AudioIODeviceType::createAudioIODeviceType_CoreAudio());
#endif
        if (type_)
            type_->scanForDevices();
    }

    ~JuceAudioDevice() override { close(); }

    bool open(const OpenRequest& request, std::string& error) override {
        close();
        if (!type_) {
            error = "No audio backend is available on this platform.";
            return false;
        }
        const auto names = type_->getDeviceNames(false);
        if (names.isEmpty()) {
            error = "No audio output devices were found.";
            return false;
        }
        juce::String name = request.outputDeviceName.empty() ? names[std::max(0, type_->getDefaultDeviceIndex(false))]
                                                             : juce::String(request.outputDeviceName);
        if (!names.contains(name)) {
            error = "Audio output \"" + toStd(name) + "\" is not connected.";
            return false;
        }
        device_.reset(type_->createDevice(name, {}));
        if (!device_) {
            error = "Could not create audio device \"" + toStd(name) + "\".";
            return false;
        }
        juce::BigInteger outputs;
        outputs.setRange(0, request.outputChannels, true);
        const auto message = device_->open({}, outputs, request.sampleRate, request.bufferSize);
        if (message.isNotEmpty()) {
            error = toStd(message);
            device_.reset();
            return false;
        }
        return true;
    }

    bool start(AudioCallback& callback, std::string& error) override {
        if (!device_ || !device_->isOpen()) {
            error = "The audio device is not open.";
            return false;
        }
        adapter_ = std::make_unique<CallbackAdapter>(callback);
        device_->start(adapter_.get());
        if (!device_->isPlaying()) {
            error = toStd(device_->getLastError());
            if (error.empty())
                error = "The audio device did not start.";
            return false;
        }
#if JUCE_MAC
        overloads_.attach(toStd(device_->getName()));
#endif
        return true;
    }

    void stop() override {
#if JUCE_MAC
        overloads_.detach();
#endif
        if (device_)
            device_->stop();
    }

    void close() override {
        if (device_) {
            device_->stop();
            device_->close();
            device_.reset();
        }
        adapter_.reset();
    }

    [[nodiscard]] DeviceInfo info() const override {
        DeviceInfo info;
        if (!device_)
            return info;
        info.name = toStd(device_->getName());
        info.sampleRate = device_->getCurrentSampleRate();
        info.bufferSize = device_->getCurrentBufferSizeSamples();
        info.outputChannels = device_->getActiveOutputChannels().countNumberOfSetBits();
        info.outputLatencyFrames = device_->getOutputLatencyInSamples();
        for (auto rate : device_->getAvailableSampleRates())
            info.availableSampleRates.push_back(rate);
        for (auto size : device_->getAvailableBufferSizes())
            info.availableBufferSizes.push_back(size);
        return info;
    }

    [[nodiscard]] int xrunCount() const override {
#if JUCE_MAC
        if (overloads_.attached())
            return overloads_.count();
#endif
        return device_ ? device_->getXRunCount() : -1;  // -1: not measurable
    }

    [[nodiscard]] std::size_t maxParallelThreads() const override {
        if (!device_)
            return 1;
        const auto workgroup = device_->getWorkgroup();
        return workgroup ? workgroup.getMaxParallelThreadCount() : 1;
    }

    [[nodiscard]] std::unique_ptr<WorkgroupMembership> joinWorkgroup() override {
        return std::make_unique<JuceWorkgroupMembership>(device_ ? device_->getWorkgroup() : juce::AudioWorkgroup{});
    }

    [[nodiscard]] std::vector<std::string> outputDeviceNames() const override {
        std::vector<std::string> out;
        if (type_)
            for (const auto& n : type_->getDeviceNames(false))
                out.push_back(toStd(n));
        return out;
    }

private:
    std::unique_ptr<juce::AudioIODeviceType> type_;
    std::unique_ptr<juce::AudioIODevice> device_;
    std::unique_ptr<CallbackAdapter> adapter_;
#if JUCE_MAC
    OverloadCounter overloads_;
#endif
};

} // namespace

std::unique_ptr<AudioDevice> createSystemAudioDevice() { return std::make_unique<JuceAudioDevice>(); }

} // namespace jrb::device
