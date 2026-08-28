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

juce::String utf8 (const char* text)
{
    return juce::String::fromUTF8 (text);
}
} // namespace

class LayoutSizeCell final : public juce::Component
{
public:
    explicit LayoutSizeCell (PianoLEDAudioProcessorEditor& owner)
        : ownerRef (owner)
    {
        slider.setSliderStyle (juce::Slider::IncDecButtons);
        slider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 36, 18);
        slider.setRange (1.0, 16.0, 1.0);
        slider.onValueChange = [this] {
            ownerRef.setKeySizeFromCell (row, static_cast<int> (slider.getValue()));
        };
        addAndMakeVisible (slider);
    }

    void setRow (int newRow, int size)
    {
        row = newRow;
        slider.setValue (size, juce::dontSendNotification);
    }

    void resized() override { slider.setBounds (getLocalBounds().reduced (2, 1)); }

private:
    PianoLEDAudioProcessorEditor& ownerRef;
    juce::Slider slider;
    int row = 0;
};

PianoLEDAudioProcessorEditor::PianoLEDAudioProcessorEditor (PianoLEDAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    setSize (520, 400);
    setResizable (false, false);

    titleLabel.setText ("PianoLED", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (titleLabel);

    testButton.setButtonText (utf8 (u8"\u0422\u0435\u0441\u0442"));
    testButton.onClick = [this] { processorRef.startStripTest(); };
    addAndMakeVisible (testButton);

    layoutButton.setButtonText (utf8 (u8"\u0420\u0430\u0441\u043a\u043b\u0430\u0434\u043a\u0430"));
    layoutButton.onClick = [this] { showLayout (true); };
    addAndMakeVisible (layoutButton);

    backButton.setButtonText (utf8 (u8"\u041d\u0430\u0437\u0430\u0434"));
    backButton.onClick = [this] { showLayout (false); };
    backButton.setVisible (false);
    addAndMakeVisible (backButton);

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

    reconnectButton.setButtonText (utf8 (u8"\u041f\u043e\u0434\u043a\u043b\u044e\u0447\u0438\u0442\u044c \u043b\u0435\u043d\u0442\u0443"));
    reconnectButton.onClick = [this] { processorRef.reconnectLeds(); };
    addAndMakeVisible (reconnectButton);

    firstNoteLabel.setText (utf8 (u8"\u041f\u0435\u0440\u0432\u0430\u044f \u043d\u043e\u0442\u0430"), juce::dontSendNotification);
    firstNoteLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa3b2));
    firstNoteLabel.setVisible (false);
    addAndMakeVisible (firstNoteLabel);

    for (int note = 21; note <= 108; ++note)
        firstNoteBox.addItem (noteNameFromNumber (note), note + 1);
    firstNoteBox.onChange = [this] { applyFirstNote(); };
    firstNoteBox.setVisible (false);
    addAndMakeVisible (firstNoteBox);

    startLedLabel.setText (utf8 (u8"\u0421\u0442\u0430\u0440\u0442\u043e\u0432\u044b\u0439 \u0434\u0438\u043e\u0434"), juce::dontSendNotification);
    startLedLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa3b2));
    startLedLabel.setVisible (false);
    addAndMakeVisible (startLedLabel);

    startLedSlider.setSliderStyle (juce::Slider::IncDecButtons);
    startLedSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, 18);
    startLedSlider.setRange (0.0, 143.0, 1.0);
    startLedSlider.onValueChange = [this] { applyStartLed(); };
    startLedSlider.setVisible (false);
    addAndMakeVisible (startLedSlider);

    keyCountLabel.setText (utf8 (u8"\u041a\u043b\u0430\u0432\u0438\u0448"), juce::dontSendNotification);
    keyCountLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa3b2));
    keyCountLabel.setVisible (false);
    addAndMakeVisible (keyCountLabel);

    keyCountSlider.setSliderStyle (juce::Slider::IncDecButtons);
    keyCountSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, 18);
    keyCountSlider.setRange (1.0, 88.0, 1.0);
    keyCountSlider.onValueChange = [this] { applyKeyCount(); };
    keyCountSlider.setVisible (false);
    addAndMakeVisible (keyCountSlider);

    presetBox.setTextWhenNothingSelected ("Default");
    presetBox.onChange = [this] { loadSelectedPreset(); };
    presetBox.setVisible (false);
    addAndMakeVisible (presetBox);

    presetNameEditor.setText ("Default");
    presetNameEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff1a1e28));
    presetNameEditor.setColour (juce::TextEditor::textColourId, juce::Colours::white);
    presetNameEditor.setVisible (false);
    addAndMakeVisible (presetNameEditor);

    saveButton.setButtonText (utf8 (u8"\u0421\u043e\u0445\u0440\u0430\u043d\u0438\u0442\u044c"));
    saveButton.onClick = [this] { savePreset(); };
    saveButton.setVisible (false);
    addAndMakeVisible (saveButton);

    savedStatusLabel.setJustificationType (juce::Justification::centredLeft);
    savedStatusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff7ee0a8));
    savedStatusLabel.setVisible (false);
    addAndMakeVisible (savedStatusLabel);

    layoutTable.setModel (this);
    layoutTable.getHeader().addColumn ("Note", 1, 180);
    layoutTable.getHeader().addColumn ("Size", 2, 140);
    layoutTable.getHeader().setStretchToFitActive (true);
    layoutTable.setRowHeight (26);
    layoutTable.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff161820));
    layoutTable.setVisible (false);
    addAndMakeVisible (layoutTable);

    hintLabel.setText ("3 LEDs per key  |  48 keys  |  C2-B5", juce::dontSendNotification);
    hintLabel.setFont (juce::FontOptions (13.0f));
    hintLabel.setJustificationType (juce::Justification::centred);
    hintLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa3b2));
    addAndMakeVisible (hintLabel);

    startTimerHz (24);
}

PianoLEDAudioProcessorEditor::~PianoLEDAudioProcessorEditor()
{
    layoutTable.setModel (nullptr);
    processorRef.stopStripTest();
    processorRef.setLayoutPreviewHold (false);
    processorRef.setLayoutPreviewNote (-1);
}

void PianoLEDAudioProcessorEditor::showLayout (bool on)
{
    layoutMode = on;
    statusLabel.setVisible (! on);
    connectionLabel.setVisible (! on);
    reconnectButton.setVisible (! on);
    testButton.setVisible (! on);
    layoutButton.setVisible (! on);
    backButton.setVisible (on);
    firstNoteLabel.setVisible (on);
    firstNoteBox.setVisible (on);
    startLedLabel.setVisible (on);
    startLedSlider.setVisible (on);
    keyCountLabel.setVisible (on);
    keyCountSlider.setVisible (on);
    presetBox.setVisible (on);
    presetNameEditor.setVisible (on);
    saveButton.setVisible (on);
    savedStatusLabel.setVisible (on);
    layoutTable.setVisible (on);

    if (on)
    {
        processorRef.stopStripTest();
        auto layout = processorRef.ledLayout();
        layout.makeSizesExplicit();
        processorRef.commitLayout (layout);
        ignorePresetBox = true;
        processorRef.refreshPresetCombo (presetBox);
        ignorePresetBox = false;
        presetNameEditor.setText (processorRef.getProgramName (processorRef.getCurrentProgram()),
                                  juce::dontSendNotification);
        savedStatusLabel.setText ({}, juce::dontSendNotification);
        syncLayoutControls();
        layoutTable.updateContent();
        layoutTable.selectRow (0);
        setSize (520, 620);
    }
    else
    {
        verifying = false;
        processorRef.setLayoutPreviewHold (false);
        processorRef.setLayoutPreviewNote (-1);
        setSize (520, 400);
    }

    resized();
}

void PianoLEDAudioProcessorEditor::syncLayoutControls()
{
    const auto& layout = processorRef.ledLayout();
    firstNoteBox.setSelectedId (layout.lowestNote + 1, juce::dontSendNotification);
    startLedSlider.setRange (0.0, std::max (0.0, static_cast<double> (layout.ledCount - 1)), 1.0);
    startLedSlider.setValue (layout.startLed, juce::dontSendNotification);
    keyCountSlider.setValue (layout.keyCount(), juce::dontSendNotification);
}

void PianoLEDAudioProcessorEditor::applyFirstNote()
{
    const int note = firstNoteBox.getSelectedId() - 1;
    if (note < 0) return;
    processorRef.setFirstNote (note);
    layoutTable.updateContent();
    selectedRowsChanged (layoutTable.getSelectedRow());
}

void PianoLEDAudioProcessorEditor::applyStartLed()
{
    processorRef.setStartLed (static_cast<int> (startLedSlider.getValue()));
}

void PianoLEDAudioProcessorEditor::applyKeyCount()
{
    processorRef.setMappedKeyCount (static_cast<int> (keyCountSlider.getValue()));
    layoutTable.updateContent();
    selectedRowsChanged (layoutTable.getSelectedRow());
}

void PianoLEDAudioProcessorEditor::loadSelectedPreset()
{
    if (ignorePresetBox) return;
    const int id = presetBox.getSelectedId();
    if (id <= 0) return;
    processorRef.setCurrentProgram (id - 1);
    presetNameEditor.setText (processorRef.getProgramName (id - 1), juce::dontSendNotification);
    syncLayoutControls();
    layoutTable.updateContent();
    layoutTable.selectRow (0);
    selectedRowsChanged (0);
}

void PianoLEDAudioProcessorEditor::savePreset()
{
    const auto name = processorRef.saveLayoutPreset (presetNameEditor.getText());
    ignorePresetBox = true;
    processorRef.refreshPresetCombo (presetBox);
    ignorePresetBox = false;
    presetNameEditor.setText (name, juce::dontSendNotification);
    savedStatusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff7ee0a8));
    savedStatusLabel.setText (utf8 (u8"\u0421\u043e\u0445\u0440\u0430\u043d\u0435\u043d\u043e: ") + name
                                  + utf8 (u8" \u2014 \u0441\u043c\u043e\u0442\u0440\u0438 \u043b\u0435\u043d\u0442\u0443"),
                              juce::dontSendNotification);
    startVerifyPlayback();
}

void PianoLEDAudioProcessorEditor::startVerifyPlayback()
{
    verifying = true;
    verifyStartMs = juce::Time::currentTimeMillis();
    verifyKey = 0;
    processorRef.setLayoutPreviewHold (true);
    processorRef.setLayoutPreviewNote (processorRef.ledLayout().lowestNote);
    layoutTable.selectRow (0);
}

void PianoLEDAudioProcessorEditor::tickVerifyPlayback()
{
    if (! verifying) return;

    const auto& layout = processorRef.ledLayout();
    const int keys = std::max (1, layout.keyCount());
    const auto elapsed = juce::Time::currentTimeMillis() - verifyStartMs;
    const int key = static_cast<int> (elapsed / 160);
    if (key >= keys)
    {
        verifying = false;
        processorRef.setLayoutPreviewHold (false);
        selectedRowsChanged (layoutTable.getSelectedRow());
        return;
    }

    if (key != verifyKey)
    {
        verifyKey = key;
        layoutTable.selectRow (key);
        processorRef.setLayoutPreviewNote (layout.lowestNote + key);
    }
}

void PianoLEDAudioProcessorEditor::setKeySizeFromCell (int row, int size)
{
    processorRef.setKeySize (row, size);
    layoutTable.selectRow (row);
    selectedRowsChanged (row);
}

int PianoLEDAudioProcessorEditor::getNumRows()
{
    return processorRef.ledLayout().keyCount();
}

void PianoLEDAudioProcessorEditor::paintRowBackground (juce::Graphics& g, int, int width, int height,
                                                       bool rowIsSelected)
{
    g.fillAll (rowIsSelected ? juce::Colour (0xff2a5080) : juce::Colour (0xff161820));
    juce::ignoreUnused (width, height);
}

void PianoLEDAudioProcessorEditor::paintCell (juce::Graphics& g, int rowNumber, int columnId,
                                              int width, int height, bool)
{
    if (columnId != 1) return;

    const auto& layout = processorRef.ledLayout();
    const int note = layout.lowestNote + rowNumber;
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (14.0f));
    g.drawText (noteNameFromNumber (note), 8, 0, width - 12, height,
                juce::Justification::centredLeft, false);
}

juce::Component* PianoLEDAudioProcessorEditor::refreshComponentForCell (int rowNumber, int columnId,
                                                                        bool, juce::Component* existing)
{
    if (columnId != 2) return nullptr;

    auto* cell = dynamic_cast<LayoutSizeCell*> (existing);
    if (cell == nullptr)
        cell = new LayoutSizeCell (*this);

    cell->setRow (rowNumber, processorRef.ledLayout().sizeForKey (rowNumber));
    return cell;
}

void PianoLEDAudioProcessorEditor::selectedRowsChanged (int lastRowSelected)
{
    if (verifying) return;
    const auto& layout = processorRef.ledLayout();
    if (lastRowSelected >= 0 && lastRowSelected < layout.keyCount())
        processorRef.setLayoutPreviewNote (layout.lowestNote + lastRowSelected);
    else
        processorRef.setLayoutPreviewNote (-1);
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
    const int keys = layout.keyCount();
    if (keys <= 0)
        return;

    const int preview = verifying ? verifyKey : layoutTable.getSelectedRow();
    const bool blinkOn = verifying || ((juce::Time::currentTimeMillis() / 500) % 2) == 0;

    const float keyWidth = static_cast<float> (keyboardBounds.getWidth()) / static_cast<float> (keys);

    for (int i = 0; i < keys; ++i)
    {
        const int note = lowest + i;
        const bool midiOn = notes.isOn (note);
        const bool editBlink = layoutMode && i == preview && blinkOn;
        const bool on = midiOn || editBlink;
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
    auto bounds = getLocalBounds().reduced (20);
    auto titleRow = bounds.removeFromTop (32);
    if (layoutMode)
        backButton.setBounds (titleRow.removeFromLeft (80));
    else
    {
        layoutButton.setBounds (titleRow.removeFromRight (110));
        titleRow.removeFromRight (6);
        testButton.setBounds (titleRow.removeFromRight (72));
    }
    titleLabel.setBounds (titleRow);

    bounds.removeFromTop (8);

    if (! layoutMode)
    {
        statusLabel.setBounds (bounds.removeFromTop (48));
        bounds.removeFromTop (4);
        connectionLabel.setBounds (bounds.removeFromTop (32));
        bounds.removeFromTop (6);
        reconnectButton.setBounds (bounds.removeFromTop (28).reduced (90, 0));
        bounds.removeFromTop (20);
    }
    else
    {
        auto row = bounds.removeFromTop (24);
        firstNoteLabel.setBounds (row.removeFromLeft (120));
        firstNoteBox.setBounds (row.removeFromLeft (90));
        row.removeFromLeft (12);
        startLedLabel.setBounds (row.removeFromLeft (130));
        startLedSlider.setBounds (row);

        bounds.removeFromTop (6);
        auto keysRow = bounds.removeFromTop (24);
        keyCountLabel.setBounds (keysRow.removeFromLeft (120));
        keyCountSlider.setBounds (keysRow.removeFromLeft (120));

        bounds.removeFromTop (6);
        auto presetRow = bounds.removeFromTop (26);
        presetBox.setBounds (presetRow.removeFromLeft (140));
        presetRow.removeFromLeft (6);
        presetNameEditor.setBounds (presetRow.removeFromLeft (160));
        presetRow.removeFromLeft (6);
        saveButton.setBounds (presetRow);

        bounds.removeFromTop (4);
        savedStatusLabel.setBounds (bounds.removeFromTop (22));

        bounds.removeFromTop (6);
        layoutTable.setBounds (bounds.removeFromTop (190));
        bounds.removeFromTop (18);
    }

    stripBounds = bounds.removeFromTop (18);
    bounds.removeFromTop (10);
    keyboardBounds = bounds.removeFromTop (36);
    bounds.removeFromTop (8);
    hintLabel.setBounds (bounds);
}

void PianoLEDAudioProcessorEditor::timerCallback()
{
    const auto& layout = processorRef.ledLayout();
    hintLabel.setText (juce::String (layout.keyCount()) + " keys  |  "
                           + noteNameFromNumber (layout.lowestNote) + "-"
                           + noteNameFromNumber (layout.highestNote()),
                       juce::dontSendNotification);

    if (layoutMode)
        tickVerifyPlayback();

    if (! layoutMode)
    {
        const auto snap = processorRef.getActiveNotes();
        const int held = snap.count();

        if (processorRef.isStripTestRunning())
            statusLabel.setText (utf8 (u8"\u0422\u0435\u0441\u0442: \u0431\u0435\u0433\u0443\u0449\u0438\u0439 \u0434\u0438\u043e\u0434 \u043f\u043e \u043b\u0435\u043d\u0442\u0435\u2026"),
                                 juce::dontSendNotification);
        else if (held == 0)
            statusLabel.setText ("Waiting for MIDI...", juce::dontSendNotification);
        else
        {
            const auto suffix = held == 1 ? " note" : " notes";
            statusLabel.setText (juce::String (held) + suffix + "  |  " + chordTextFromSnapshot (snap),
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
    }

    repaint (stripBounds.expanded (2, 18));
    repaint (keyboardBounds.expanded (2, 2));
}
