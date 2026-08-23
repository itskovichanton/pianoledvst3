#include "PluginEditor.h"

namespace
{
juce::String noteNameFromNumber (int noteNumber)
{
    if (noteNumber < 0)
        return "--";

    static constexpr const char* names[] = { "C", "C#", "D", "D#", "E", "F",
                                             "F#", "G", "G#", "A", "A#", "B" };
    const auto name = names[noteNumber % 12];
    const auto octave = (noteNumber / 12) - 1;
    return juce::String (name) + juce::String (octave);
}
} // namespace

PianoLEDAudioProcessorEditor::PianoLEDAudioProcessorEditor (PianoLEDAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    setSize (420, 240);
    setResizable (false, false);

    titleLabel.setText ("PianoLED", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (28.0f, juce::Font::bold));
    titleLabel.setJustificationType (juce::Justification::centred);
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (titleLabel);

    statusLabel.setText ("Waiting for MIDI...", juce::dontSendNotification);
    statusLabel.setFont (juce::FontOptions (18.0f));
    statusLabel.setJustificationType (juce::Justification::centred);
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff7ee0a8));
    addAndMakeVisible (statusLabel);

    hintLabel.setText ("AU instrument · VST3 + Standalone\nPlay a note to see pitch and velocity.",
                       juce::dontSendNotification);
    hintLabel.setFont (juce::FontOptions (13.0f));
    hintLabel.setJustificationType (juce::Justification::centred);
    hintLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa3b2));
    addAndMakeVisible (hintLabel);

    startTimerHz (24);
}

void PianoLEDAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff12141a));
    g.setColour (juce::Colour (0xff2a3140));
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (10.0f), 12.0f, 1.5f);
}

void PianoLEDAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (24);
    titleLabel.setBounds (bounds.removeFromTop (48));
    bounds.removeFromTop (12);
    statusLabel.setBounds (bounds.removeFromTop (48));
    bounds.removeFromTop (8);
    hintLabel.setBounds (bounds);
}

void PianoLEDAudioProcessorEditor::timerCallback()
{
    const auto note = processorRef.getLastMidiNote();
    const auto velocity = processorRef.getLastMidiVelocity();

    if (note < 0)
        statusLabel.setText ("Waiting for MIDI...", juce::dontSendNotification);
    else if (velocity > 0)
        statusLabel.setText ("Note " + noteNameFromNumber (note)
                                 + "  ·  vel " + juce::String (velocity),
                             juce::dontSendNotification);
    else
        statusLabel.setText ("Note " + noteNameFromNumber (note) + "  ·  off",
                             juce::dontSendNotification);
}
