// Test instrument: polyphonic sine voices driven by MIDI.
#include <juce_audio_utils/juce_audio_utils.h>

namespace {
struct Sound final : juce::SynthesiserSound {
    bool appliesToNote(int) override { return true; }
    bool appliesToChannel(int) override { return true; }
};

struct Voice final : juce::SynthesiserVoice {
    bool canPlaySound(juce::SynthesiserSound*) override { return true; }
    void startNote(int note, float velocity, juce::SynthesiserSound*, int) override {
        increment = juce::MathConstants<double>::twoPi * juce::MidiMessage::getMidiNoteInHertz(note) / getSampleRate();
        level = velocity * 0.15;
        envelope.setSampleRate(getSampleRate());
        envelope.setParameters({0.005f, 0.1f, 0.8f, 0.2f});
        envelope.noteOn();
    }
    void stopNote(float, bool allowTail) override {
        if (allowTail) envelope.noteOff();
        else { envelope.reset(); clearCurrentNote(); }
    }
    void pitchWheelMoved(int) override {}
    void controllerMoved(int, int) override {}
    void renderNextBlock(juce::AudioBuffer<float>& out, int start, int count) override {
        if (!isVoiceActive()) return;
        for (int i = start; i < start + count; ++i) {
            const auto sample = static_cast<float>(std::sin(phase) * level) * envelope.getNextSample();
            phase += increment;
            for (int ch = 0; ch < out.getNumChannels(); ++ch) out.addSample(ch, i, sample);
        }
        if (!envelope.isActive()) clearCurrentNote();
    }
    double phase = 0, increment = 0, level = 0;
    juce::ADSR envelope;
};
} // namespace

class TestSynth final : public juce::AudioProcessor {
public:
    TestSynth()
        : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo())),
          state(*this, nullptr, "params",
                {std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{"level", 1}, "Level", 0.0f, 1.0f, 0.8f)}) {
        for (int i = 0; i < 8; ++i) synth.addVoice(new Voice());
        synth.addSound(new Sound());
    }
    void prepareToPlay(double rate, int) override { synth.setCurrentPlaybackSampleRate(rate); }
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override {
        buffer.clear();
        synth.renderNextBlock(buffer, midi, 0, buffer.getNumSamples());
        buffer.applyGain(state.getRawParameterValue("level")->load());
    }
    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor(*this); }
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "JRB Test Synth"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.2; }
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
    juce::Synthesiser synth;
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TestSynth(); }
