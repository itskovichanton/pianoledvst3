#include "PluginEditor.h"

#include <algorithm>

namespace
{
juce::String noteNameFromNumber (int noteNumber)
{
    if (noteNumber < 0 || noteNumber > 127)
        return "--";

    static constexpr const char* names[] = { "C", "C#", "D", "D#", "E", "F",
                                             "F#", "G", "G#", "A", "A#", "B" };
    const auto name = names[noteNumber % 12];
    const auto octave = (noteNumber / 12) - 1;
    return juce::String (name) + juce::String (octave);
}

bool isBlackKey (int noteNumber)
{
    switch (noteNumber % 12)
    {
        case 1: case 3: case 6: case 8: case 10: return true;
        default: return false;
    }
}

juce::String chordTextFromSnapshot (const piano_led::NoteBitmask::Snapshot& snap)
{
    juce::StringArray names;
    names.ensureStorageAllocated (snap.count());

    for (int note = 0; note < 128; ++note)
        if (snap.isOn (note))
            names.add (noteNameFromNumber (note));

    return names.joinIntoString ("  ");
}
} // namespace

PianoLEDAudioProcessorEditor::PianoLEDAudioProcessorEditor (PianoLEDAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    setSize (500, 400);
    setResizable (false, false);

    titleLabel.setText ("PianoLED", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (28.0f, juce::Font::bold));
    titleLabel.setJustificationType (juce::Justification::centred);
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (titleLabel);

    statusLabel.setText ("Waiting for MIDI...", juce::dontSendNotification);
    statusLabel.setFont (juce::FontOptions (16.0f));
    statusLabel.setJustificationType (juce::Justification::centredTop);
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff7ee0a8));
    addAndMakeVisible (statusLabel);

    connectionLabel.setText ("LED strip: looking...", juce::dontSendNotification);
    connectionLabel.setFont (juce::FontOptions (12.0f));
    connectionLabel.setJustificationType (juce::Justification::centredTop);
    connectionLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa3b2));
    addAndMakeVisible (connectionLabel);

    reconnectButton.setButtonText (juce::String::fromUTF8 (u8"\u041f\u043e\u0434\u043a\u043b\u044e\u0447\u0438\u0442\u044c \u043b\u0435\u043d\u0442\u0443"));
    reconnectButton.onClick = [this] {
        processorRef.reconnectLeds();
    };
    addAndMakeVisible (reconnectButton);

    hintLabel.setText ("3 LEDs per key  |  C2-B5  |  1% brightness",
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

    drawStrip (g);
    drawKeyboard (g);
}

void PianoLEDAudioProcessorEditor::drawStrip (juce::Graphics& g)
{
    const auto& frame = processorRef.lastLedFrame();
    const int leds = static_cast<int> (frame.size() / 3);
    if (leds <= 0 || stripBounds.isEmpty())
        return;

    g.setColour (juce::Colour (0xff9aa3b2));
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("strip", stripBounds.withHeight (14).translated (0, -16),
                juce::Justification::centredLeft, false);

    const float ledWidth = static_cast<float> (stripBounds.getWidth()) / static_cast<float> (leds);
    for (int led = 0; led < leds; ++led)
    {
        const auto r = frame[static_cast<std::size_t> (led) * 3u];
        const auto gg = frame[static_cast<std::size_t> (led) * 3u + 1u];
        const auto b = frame[static_cast<std::size_t> (led) * 3u + 2u];
        const bool on = r != 0 || gg != 0 || b != 0;

        g.setColour (on ? juce::Colour (0xffe05050) : juce::Colour (0xff1a1e28));
        g.fillRect (static_cast<float> (stripBounds.getX()) + static_cast<float> (led) * ledWidth,
                    static_cast<float> (stripBounds.getY()),
                    std::max (1.0f, ledWidth - 0.4f),
                    static_cast<float> (stripBounds.getHeight()));
    }
}

void PianoLEDAudioProcessorEditor::drawKeyboard (juce::Graphics& g)
{
    const auto layout = processorRef.ledLayout();
    const auto notes = processorRef.getActiveNotes();
    if (! layout.isValid() || keyboardBounds.isEmpty())
        return;

    const int lowest = layout.lowestNote;
    const int highest = layout.highestNote();
    const int keys = highest - lowest + 1;
    if (keys <= 0)
        return;

    const float keyWidth = static_cast<float> (keyboardBounds.getWidth()) / static_cast<float> (keys);

    for (int i = 0; i < keys; ++i)
    {
        const int note = lowest + i;
        const bool on = notes.isOn (note);
        const bool black = isBlackKey (note);
        const auto x = static_cast<float> (keyboardBounds.getX()) + static_cast<float> (i) * keyWidth;

        if (on)
            g.setColour (juce::Colour (0xffe05050));
        else if (black)
            g.setColour (juce::Colour (0xff161820));
        else
            g.setColour (juce::Colour (0xff2a3140));

        g.fillRect (x, static_cast<float> (keyboardBounds.getY()),
                    std::max (1.0f, keyWidth - 0.5f),
                    static_cast<float> (keyboardBounds.getHeight()));
    }

    g.setColour (juce::Colour (0xff3a4150));
    g.drawRect (keyboardBounds.toFloat(), 1.0f);
}

void PianoLEDAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (24);
    titleLabel.setBounds (bounds.removeFromTop (36));
    bounds.removeFromTop (6);
    statusLabel.setBounds (bounds.removeFromTop (52));
    bounds.removeFromTop (4);
    connectionLabel.setBounds (bounds.removeFromTop (36));
    bounds.removeFromTop (8);
    reconnectButton.setBounds (bounds.removeFromTop (28).reduced (90, 0));
    bounds.removeFromTop (22);
    stripBounds = bounds.removeFromTop (18);
    bounds.removeFromTop (10);
    keyboardBounds = bounds.removeFromTop (36);
    bounds.removeFromTop (8);
    hintLabel.setBounds (bounds);
}

void PianoLEDAudioProcessorEditor::timerCallback()
{
    const auto snap = processorRef.getActiveNotes();
    const int held = snap.count();

    if (held == 0)
    {
        statusLabel.setText ("Waiting for MIDI...", juce::dontSendNotification);
    }
    else
    {
        const auto chord = chordTextFromSnapshot (snap);
        const auto suffix = held == 1 ? " note" : " notes";
        statusLabel.setText (juce::String (held) + suffix + "  |  " + chord,
                             juce::dontSendNotification);
    }

    if (processorRef.isLedConnected())
    {
        connectionLabel.setColour (juce::Label::textColourId, juce::Colour (0xff7ee0a8));
        connectionLabel.setText ("LED strip: " + processorRef.ledDevicePath() + "  |  1%",
                                 juce::dontSendNotification);
    }
    else
    {
        connectionLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe08a7e));
        auto error = processorRef.ledLastError().trim();
        if (error.isEmpty())
            connectionLabel.setText ("LED strip: not connected", juce::dontSendNotification);
        else
            connectionLabel.setText ("LED strip: " + error.substring (0, 280),
                                     juce::dontSendNotification);
    }

    repaint (stripBounds.expanded (2, 18));
    repaint (keyboardBounds.expanded (2, 2));
}
