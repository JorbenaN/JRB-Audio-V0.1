// JUCE-backed plugin host (ADR-003: JUCE stays behind jrb::plugins).
// Compiled twice: JRB_PLUGIN_EDITORS=1 (app, with GUIs) and 0 (scanner, headless).

#include "jrb/plugins/Plugin.hpp"

#if JRB_PLUGIN_EDITORS
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#else
#include <juce_audio_processors_headless/juce_audio_processors_headless.h>
#endif

#include <algorithm>

#if JUCE_MAC
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace jrb::plugins {
namespace {

const char* juceFormatName(Format format) { return format == Format::audioUnit ? "AudioUnit" : "VST3"; }

PluginInfo toInfo(const juce::PluginDescription& d, Format format) {
    PluginInfo info;
    info.format = format;
    info.name = d.name.toStdString();
    info.vendor = d.manufacturerName.toStdString();
    info.version = d.version.toStdString();
    info.category = d.category.toStdString();
    info.fileOrIdentifier = d.fileOrIdentifier.toStdString();
    info.uniqueId = d.createIdentifierString().toStdString();
    info.isInstrument = d.isInstrument;
    info.acceptsMidi = d.isInstrument;
    info.inputChannels = d.numInputChannels;
    info.outputChannels = d.numOutputChannels;
    if (auto xml = d.createXml())
        info.loadToken = xml->toString(juce::XmlElement::TextFormat().singleLine()).toStdString();
    return info;
}

#if JRB_PLUGIN_EDITORS
/// Shared between an instance and its open editor so that destroying the
/// instance first closes the editor instead of leaving it dangling.
struct EditorSlot {
    std::unique_ptr<juce::AudioProcessorEditor> editor;
};

class JuceEditor final : public PluginEditor {
public:
    explicit JuceEditor(std::shared_ptr<EditorSlot> slot) : slot_(std::move(slot)) {}
    ~JuceEditor() override { slot_->editor.reset(); }
    [[nodiscard]] int width() const override { return slot_->editor ? slot_->editor->getWidth() : 0; }
    [[nodiscard]] int height() const override { return slot_->editor ? slot_->editor->getHeight() : 0; }

private:
    std::shared_ptr<EditorSlot> slot_;
};
#endif

class JuceInstance final : public PluginInstance {
public:
    JuceInstance(std::unique_ptr<juce::AudioPluginInstance> plugin, PluginInfo info)
        : plugin_(std::move(plugin)), info_(std::move(info)) {
        info_.acceptsMidi = plugin_->acceptsMidi();
        info_.producesMidi = plugin_->producesMidi();
    }

    ~JuceInstance() override {
#if JRB_PLUGIN_EDITORS
        if (editorSlot_)
            editorSlot_->editor.reset();  // close any open GUI before the plugin goes away
#endif
        plugin_->releaseResources();
    }

    [[nodiscard]] const PluginInfo& info() const override { return info_; }

    bool prepare(double sampleRate, int maxBlockSize, std::string& error) override {
        if (sampleRate <= 0 || maxBlockSize <= 0) {
            error = "Invalid sample rate or block size.";
            return false;
        }
        plugin_->enableAllBuses();
        plugin_->setRateAndBufferSizeDetails(sampleRate, maxBlockSize);
        plugin_->prepareToPlay(sampleRate, maxBlockSize);
        midi_.ensureSize(4096);  // so adding events on the audio thread never allocates
        return true;
    }

    void release() override { plugin_->releaseResources(); }
    void setOffline(bool offline) override { plugin_->setNonRealtime(offline); }

    void process(float* const* channels, int numChannels, int frames, const MidiEvent* events,
                 int numEvents) noexcept override {
        midi_.clear();
        for (int i = 0; i < numEvents; ++i)
            midi_.addEvent(events[i].bytes, events[i].size, events[i].sampleOffset);
        juce::AudioBuffer<float> buffer(channels, numChannels, frames);  // refers to the host's memory
        plugin_->processBlock(buffer, midi_);
    }

    [[nodiscard]] int inputChannels() const override { return plugin_->getTotalNumInputChannels(); }
    [[nodiscard]] int outputChannels() const override { return plugin_->getTotalNumOutputChannels(); }
    [[nodiscard]] int latencySamples() const override { return plugin_->getLatencySamples(); }
    [[nodiscard]] double tailSeconds() const override { return plugin_->getTailLengthSeconds(); }

    [[nodiscard]] std::vector<std::uint8_t> state() override {
        juce::MemoryBlock block;
        plugin_->getStateInformation(block);
        const auto* bytes = static_cast<const std::uint8_t*>(block.getData());
        return {bytes, bytes + block.getSize()};
    }

    bool setState(const std::vector<std::uint8_t>& state) override {
        plugin_->setStateInformation(state.data(), static_cast<int>(state.size()));
        return true;
    }

    [[nodiscard]] int parameterCount() const override { return plugin_->getParameters().size(); }
    [[nodiscard]] ParameterInfo parameterInfo(int i) const override {
        ParameterInfo info;
        auto* p = parameter(i);
        if (!p)
            return info;
        info.name = p->getName(128).toStdString();
        info.automatable = p->isAutomatable();
        info.discrete = p->isDiscrete();
        info.steps = info.discrete ? p->getNumSteps() : 0;
        info.readOnly = (p->getCategory() >> 16) == 2;  // JUCE's meter categories
#if JUCE_MAC
        // JUCE 9 does not carry Audio Unit read-only/meter flags into its
        // parameter objects; ask the Audio Unit directly.
        if (info_.format == Format::audioUnit)
            if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(p))
                if (auto* client = plugin_->getAudioUnitClient(); AudioUnit au = client ? client->getAudioUnitHandle() : nullptr) {
                    AudioUnitParameterInfo auInfo{};
                    UInt32 size = sizeof(auInfo);
                    const auto id = static_cast<AudioUnitParameterID>(hosted->getParameterID().getLargeIntValue());
                    if (AudioUnitGetProperty(au, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &auInfo, &size) == noErr) {
                        if (auInfo.flags & (kAudioUnitParameterFlag_MeterReadOnly))
                            info.readOnly = true;
                        if ((auInfo.flags & kAudioUnitParameterFlag_IsWritable) == 0)
                            info.readOnly = true;
                        if (auInfo.cfNameString && (auInfo.flags & kAudioUnitParameterFlag_CFNameRelease))
                            CFRelease(auInfo.cfNameString);
                    }
                }
#endif
        if (info.readOnly)
            info.automatable = false;
        return info;
    }
    [[nodiscard]] float parameterValue(int i) const override {
        auto* p = parameter(i);
        if (!p)
            return 0.0f;
#if JUCE_MAC
        // JUCE caches Audio Unit parameter values and can report stale values
        // (S4 finding). Read the live value from the Audio Unit instead.
        float live = 0.0f;
        if (readAudioUnitParameter(p, live))
            return live;
#endif
        return p->getValue();
    }
    void setParameterValue(int i, float value) override {
        if (auto* p = parameter(i))
            p->setValueNotifyingHost(std::clamp(value, 0.0f, 1.0f));
    }

    [[nodiscard]] bool hasEditor() const override { return plugin_->hasEditor(); }

    [[nodiscard]] std::unique_ptr<PluginEditor> openEditor(void* parentView) override {
#if JRB_PLUGIN_EDITORS
        if (!plugin_->hasEditor() || parentView == nullptr)
            return nullptr;
        if (!editorSlot_)
            editorSlot_ = std::make_shared<EditorSlot>();
        editorSlot_->editor.reset();
        auto* editor = plugin_->createEditorAndMakeActive();
        if (editor == nullptr)
            return nullptr;
        editorSlot_->editor.reset(editor);
        editor->setVisible(true);
        editor->addToDesktop(0, parentView);  // embeds the editor's native view inside parentView
        return std::make_unique<JuceEditor>(editorSlot_);
#else
        (void)parentView;
        return nullptr;
#endif
    }

private:
#if JUCE_MAC
    AudioUnit audioUnit() const {
        if (info_.format != Format::audioUnit)
            return nullptr;
        auto* client = plugin_->getAudioUnitClient();
        return client ? client->getAudioUnitHandle() : nullptr;
    }

    /// Live, normalised value straight from the Audio Unit.
    bool readAudioUnitParameter(juce::AudioProcessorParameter* p, float& normalised) const {
        auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(p);
        AudioUnit au = audioUnit();
        if (!hosted || !au)
            return false;
        const auto id = static_cast<AudioUnitParameterID>(hosted->getParameterID().getLargeIntValue());
        AudioUnitParameterInfo auInfo{};
        UInt32 size = sizeof(auInfo);
        if (AudioUnitGetProperty(au, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &auInfo, &size) != noErr)
            return false;
        if (auInfo.cfNameString && (auInfo.flags & kAudioUnitParameterFlag_CFNameRelease))
            CFRelease(auInfo.cfNameString);
        AudioUnitParameterValue value = 0;
        if (AudioUnitGetParameter(au, id, kAudioUnitScope_Global, 0, &value) != noErr)
            return false;
        const float range = auInfo.maxValue - auInfo.minValue;
        normalised = range > 0 ? std::clamp((value - auInfo.minValue) / range, 0.0f, 1.0f) : 0.0f;
        return true;
    }
#endif

    juce::AudioProcessorParameter* parameter(int i) const {
        const auto& params = plugin_->getParameters();
        return (i >= 0 && i < params.size()) ? params[i] : nullptr;
    }

    std::unique_ptr<juce::AudioPluginInstance> plugin_;
    PluginInfo info_;
    juce::MidiBuffer midi_;
#if JRB_PLUGIN_EDITORS
    std::shared_ptr<EditorSlot> editorSlot_;
#endif
};

class JuceHost final : public PluginHost {
public:
    JuceHost() {
#if JRB_PLUGIN_EDITORS
        juce::initialiseJuce_GUI();
        juce::addDefaultFormatsToManager(manager_);
#else
        juce::MessageManager::getInstance();
        juce::addHeadlessDefaultFormatsToManager(manager_);
#endif
    }

    [[nodiscard]] std::vector<std::string> discover(Format format, const std::vector<std::string>& extraFolders) override {
        std::vector<std::string> out;
        auto* f = find(format);
        if (!f)
            return out;
        auto path = f->getDefaultLocationsToSearch();
        for (const auto& folder : extraFolders)
            path.add(juce::File(folder));
        for (const auto& id : f->searchPathsForPlugins(path, true, false))
            out.push_back(id.toStdString());
        return out;
    }

    [[nodiscard]] ScanResult describe(Format format, const std::string& fileOrIdentifier) override {
        ScanResult result;
        result.format = format;
        result.fileOrIdentifier = fileOrIdentifier;
        auto* f = find(format);
        if (!f) {
            result.message = std::string("Plugin format ") + juceFormatName(format) + " is not available.";
            return result;
        }
        juce::OwnedArray<juce::PluginDescription> found;
        f->findAllTypesForFile(found, juce::String(fileOrIdentifier));
        for (auto* d : found)
            result.plugins.push_back(toInfo(*d, format));
        result.status = result.plugins.empty() ? ScanStatus::failed : ScanStatus::ok;
        if (result.plugins.empty())
            result.message = "No loadable plugin was found in this file.";
        return result;
    }

    [[nodiscard]] std::unique_ptr<PluginInstance> instantiate(const PluginInfo& info, double sampleRate, int maxBlockSize,
                                                              std::string& error) override {
        auto xml = juce::parseXML(juce::String(info.loadToken));
        juce::PluginDescription description;
        if (!xml || !description.loadFromXml(*xml)) {
            error = "The saved plugin description is unreadable.";
            return nullptr;
        }
        juce::String message;
        auto plugin = manager_.createPluginInstance(description, sampleRate, maxBlockSize, message);
        if (!plugin) {
            error = message.isEmpty() ? "The plugin could not be loaded." : message.toStdString();
            return nullptr;
        }
        return std::make_unique<JuceInstance>(std::move(plugin), info);
    }

private:
    juce::AudioPluginFormat* find(Format format) {
        for (auto* f : manager_.getFormats())
            if (f->getName() == juceFormatName(format))
                return f;
        return nullptr;
    }

    juce::AudioPluginFormatManager manager_;
};

} // namespace

std::unique_ptr<PluginHost> createPluginHost() { return std::make_unique<JuceHost>(); }

} // namespace jrb::plugins
