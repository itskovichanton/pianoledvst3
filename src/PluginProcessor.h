#pragma once

#include "LedBridgeGlue.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

class PianoLEDAudioProcessor final : public juce::AudioProcessor
{
public:
    PianoLEDAudioProcessor();
    ~PianoLEDAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }

    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    piano_led::NoteBitmask::Snapshot getActiveNotes() const { return ledBridge.activeNotes(); }
    const piano_led::StripLayout& ledLayout() const { return ledBridge.layout(); }
    const std::vector<std::uint8_t>& lastLedFrame() const { return ledBridge.lastFrame(); }

    bool isLedConnected() const { return ledBridge.isConnected(); }
    juce::String ledDevicePath() const { return ledBridge.devicePath(); }
    juce::String ledLastError() const { return ledBridge.lastError(); }
    bool reconnectLeds() { return ledBridge.reconnect(); }

private:
    piano_led::PluginLedBridge ledBridge;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoLEDAudioProcessor)
};
