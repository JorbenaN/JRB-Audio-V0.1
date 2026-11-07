// Test effect: gain plus a fixed 64-sample delay that it reports as latency,
// so hosts can verify latency reporting and compensation.
#include <juce_audio_utils/juce_audio_utils.h>

class TestGain final : public juce::AudioProcessor {
public:
    static constexpr int kLatency = 64;

    TestGain()
        : AudioProcessor(BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo())
                             .withOutput("Output", juce::AudioChannelSet::stereo())),
          state(*this, nullptr, "params",
                {std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{"gain", 1}, "Gain", -24.0f, 12.0f, 0.0f),
                 std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"invert", 1}, "Invert", false)}) {
        setLatencySamples(kLatency);
    }

    void prepareToPlay(double, int) override {
        delay.setSize(2, kLatency);
        delay.clear();
        position = 0;
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        const float gain = juce::Decibels::decibelsToGain(state.getRawParameterValue("gain")->load()) *
                           (state.getRawParameterValue("invert")->load() > 0.5f ? -1.0f : 1.0f);
        const int channels = std::min(buffer.getNumChannels(), 2);
        for (int i = 0; i < buffer.getNumSamples(); ++i) {
            for (int ch = 0; ch < channels; ++ch) {
                float* data = buffer.getWritePointer(ch);
                const float delayed = delay.getSample(ch, position);
                delay.setSample(ch, position, data[i]);
                data[i] = delayed * gain;
            }
            position = (position + 1) % kLatency;
        }
    }

    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor(*this); }
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "JRB Test Gain"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Default"; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& dest) override {
        if (auto xml = state.copyState().createXml()) copyXmlToBinary(*xml, dest);
    }
    void setStateInformation(const void* data, int size) override {
        if (auto xml = getXmlFromBinary(data, size)) state.replaceState(juce::ValueTree::fromXml(*xml));
    }

private:
    juce::AudioProcessorValueTreeState state;
    juce::AudioBuffer<float> delay;
    int position = 0;
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TestGain(); }
