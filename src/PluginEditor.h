#pragma once

#include "PluginProcessor.h"

class PianoLEDAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                           private juce::Timer
{
public:
    explicit PianoLEDAudioProcessorEditor (PianoLEDAudioProcessor&);
    ~PianoLEDAudioProcessorEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    PianoLEDAudioProcessor& processorRef;
    juce::Label titleLabel;
    juce::Label statusLabel;
    juce::Label connectionLabel;
    juce::TextButton reconnectButton;
    juce::Label hintLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoLEDAudioProcessorEditor)
};
