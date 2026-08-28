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
    setSize (440, 320);
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

    connectionLabel.setText ("LED strip: looking...", juce::dontSendNotification);
    connectionLabel.setFont (juce::FontOptions (12.0f));
    connectionLabel.setJustificationType (juce::Justification::centredTop);
    connectionLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa3b2));
    addAndMakeVisible (connectionLabel);

    reconnectButton.setButtonText ("Подключить ленту");
    reconnectButton.onClick = [this] {
        processorRef.reconnectLeds();
    };
    addAndMakeVisible (reconnectButton);

    hintLabel.setText ("3 LEDs per key  ·  C2–B5  ·  AU / VST3 / Standalone",
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
    titleLabel.setBounds (bounds.removeFromTop (40));
    bounds.removeFromTop (8);
    statusLabel.setBounds (bounds.removeFromTop (36));
    bounds.removeFromTop (4);
    connectionLabel.setBounds (bounds.removeFromTop (72));
    bounds.removeFromTop (8);
    reconnectButton.setBounds (bounds.removeFromTop (28).reduced (80, 0));
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

    if (processorRef.isLedConnected())
    {
        connectionLabel.setColour (juce::Label::textColourId, juce::Colour (0xff7ee0a8));
        connectionLabel.setText ("LED strip: " + processorRef.ledDevicePath(),
                                 juce::dontSendNotification);
        return;
    }

    connectionLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe08a7e));
    auto error = processorRef.ledLastError().trim();
    if (error.isEmpty())
        connectionLabel.setText ("LED strip: not connected", juce::dontSendNotification);
    else
        connectionLabel.setText ("LED strip: " + error.substring (0, 280),
                                 juce::dontSendNotification);
}
