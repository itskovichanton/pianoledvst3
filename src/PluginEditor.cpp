#include "PluginEditor.h"

#include <algorithm>
#include <cstdint>

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

/** Каналы ленты на 2% почти чёрные на экране — растягиваем к полной яркости,
 *  сохраняя оттенок, чтобы превью в плагине было читаемым. */
juce::Colour visibleLedColour (std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    const int peak = std::max ({ static_cast<int> (r), static_cast<int> (g), static_cast<int> (b) });
    if (peak <= 0)
        return juce::Colour (0xff1a1e28);
    return juce::Colour (static_cast<juce::uint8> (r * 255 / peak),
                         static_cast<juce::uint8> (g * 255 / peak),
                         static_cast<juce::uint8> (b * 255 / peak));
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

class ConnectSpinner final : public juce::Component
{
public:
    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (2.0f);
        const auto t = (float) (juce::Time::getMillisecondCounter() % 900) / 900.0f;
        const auto start = t * juce::MathConstants<float>::twoPi;
        juce::Path arc;
        arc.addCentredArc (bounds.getCentreX(), bounds.getCentreY(),
                           bounds.getWidth() * 0.5f, bounds.getHeight() * 0.5f,
                           0.0f, start, start + 4.3f, true);
        g.setColour (juce::Colour (0xff7ee0a8));
        g.strokePath (arc, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    }
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
    layoutButton.onClick = [this] { showPage (Page::layout); };
    addAndMakeVisible (layoutButton);

    settingsButton.setButtonText (utf8 (u8"\u041d\u0430\u0441\u0442\u0440\u043e\u0439\u043a\u0438"));
    settingsButton.onClick = [this] { showPage (Page::settings); };
    addAndMakeVisible (settingsButton);

    specialsButton.setButtonText (utf8 (u8"\u0421\u043f\u0435\u0446\u0438\u0430\u043b\u044c\u043d\u044b\u0435 \u0444\u0443\u043d\u043a\u0446\u0438\u0438"));
    specialsButton.onClick = [this] { showPage (Page::specials); };
    addAndMakeVisible (specialsButton);

    backButton.setButtonText (utf8 (u8"\u041d\u0430\u0437\u0430\u0434"));
    backButton.onClick = [this] { showPage (Page::play); };
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

    connectSpinner = std::make_unique<ConnectSpinner>();
    connectSpinner->setInterceptsMouseClicks (false, false);
    connectSpinner->setVisible (false);
    addAndMakeVisible (*connectSpinner);

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

    brightnessLabel.setText (utf8 (u8"\u042f\u0440\u043a\u043e\u0441\u0442\u044c"), juce::dontSendNotification);
    brightnessLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa3b2));
    brightnessLabel.setVisible (false);
    addAndMakeVisible (brightnessLabel);

    brightnessSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    brightnessSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 20);
    brightnessSlider.setRange (0.1, 20.0, 0.1);
    brightnessSlider.setTextValueSuffix (" %");
    brightnessSlider.onValueChange = [this] { applyBrightness(); };
    brightnessSlider.setVisible (false);
    addAndMakeVisible (brightnessSlider);

    colourLabel.setText (utf8 (u8"\u0426\u0432\u0435\u0442 \u0441\u0432\u0435\u0442\u043e\u0434\u0438\u043e\u0434\u043e\u0432"), juce::dontSendNotification);
    colourLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa3b2));
    colourLabel.setVisible (false);
    addAndMakeVisible (colourLabel);

    colourSelector.setColour (juce::ColourSelector::backgroundColourId, juce::Colour (0xff161820));
    colourSelector.setOpaque (true);
    colourSelector.setVisible (false);
    colourSelector.addChangeListener (this);
    addAndMakeVisible (colourSelector);

    historySizeLabel.setText (utf8 (u8"\u0418\u0441\u0442\u043e\u0440\u0438\u044f \u043d\u043e\u0442 (N)"), juce::dontSendNotification);
    historySizeLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa3b2));
    historySizeLabel.setVisible (false);
    addAndMakeVisible (historySizeLabel);

    historySizeSlider.setSliderStyle (juce::Slider::IncDecButtons);
    historySizeSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, 18);
    historySizeSlider.setRange (1.0, static_cast<double> (piano_led::NoteHistory::kMax), 1.0);
    historySizeSlider.onValueChange = [this] { applyHistoryCapacity(); };
    historySizeSlider.setVisible (false);
    addAndMakeVisible (historySizeSlider);

    recallCountLabel.setText ("M", juce::dontSendNotification);
    recallCountLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa3b2));
    recallCountLabel.setJustificationType (juce::Justification::centred);
    recallCountLabel.setVisible (false);
    addAndMakeVisible (recallCountLabel);

    recallCountSlider.setSliderStyle (juce::Slider::IncDecButtons);
    recallCountSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 44, 18);
    recallCountSlider.setRange (1.0, 30.0, 1.0);
    recallCountSlider.onValueChange = [this] { applyRecallCount(); };
    recallCountSlider.setVisible (false);
    addAndMakeVisible (recallCountSlider);

    recallButton.setButtonText (utf8 (u8"\u041f\u043e\u0441\u043b\u0435\u0434\u043d\u0438\u0435 \u043d\u043e\u0442\u044b"));
    recallButton.onClick = [this] { recallLastNotes(); };
    recallButton.setVisible (false);
    addAndMakeVisible (recallButton);

    chordWindowLabel.setText (utf8 (u8"\u041e\u043a\u043d\u043e \u0430\u043a\u043a\u043e\u0440\u0434\u0430, \u043c\u0441"), juce::dontSendNotification);
    chordWindowLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa3b2));
    chordWindowLabel.setVisible (false);
    addAndMakeVisible (chordWindowLabel);

    chordWindowSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    chordWindowSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
    chordWindowSlider.setRange (5.0, 250.0, 5.0);
    chordWindowSlider.setTextValueSuffix (" ms");
    chordWindowSlider.onValueChange = [this] { applyChordWindow(); };
    chordWindowSlider.setVisible (false);
    addAndMakeVisible (chordWindowSlider);

    chordButton.setButtonText (utf8 (u8"\u041f\u043e\u0441\u043b\u0435\u0434\u043d\u0438\u0439 \u0430\u043a\u043a\u043e\u0440\u0434"));
    chordButton.onClick = [this] { recallLastChord(); };
    chordButton.setVisible (false);
    addAndMakeVisible (chordButton);

    hintLabel.setText ("3 LEDs per key  |  48 keys  |  C2-B5", juce::dontSendNotification);
    hintLabel.setFont (juce::FontOptions (13.0f));
    hintLabel.setJustificationType (juce::Justification::centred);
    hintLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa3b2));
    addAndMakeVisible (hintLabel);

    startTimerHz (30);
    syncSpecialsControls();
}

PianoLEDAudioProcessorEditor::~PianoLEDAudioProcessorEditor()
{
    colourSelector.removeChangeListener (this);
    layoutTable.setModel (nullptr);
    leaveCurrentPage();
    processorRef.persistLayout();
    processorRef.stopStripTest();
}

void PianoLEDAudioProcessorEditor::leaveCurrentPage()
{
    processorRef.clearHistoryPreview();
    if (page == Page::layout)
    {
        verifying = false;
        processorRef.setLayoutPreviewHold (false);
        processorRef.setLayoutPreviewNote (-1);
        processorRef.persistLayout();
    }
    else if (page == Page::settings)
    {
        processorRef.setSettingsFillPreview (false);
        processorRef.persistLayout();
    }
    else if (page == Page::specials)
    {
        processorRef.persistLayout();
    }
}

void PianoLEDAudioProcessorEditor::showPage (Page next)
{
    if (page != next)
        leaveCurrentPage();

    page = next;
    const bool play = page == Page::play;
    const bool layout = page == Page::layout;
    const bool settings = page == Page::settings;
    const bool specials = page == Page::specials;

    titleLabel.setText (specials ? utf8 (u8"\u0421\u043f\u0435\u0446. \u0444\u0443\u043d\u043a\u0446\u0438\u0438") : "PianoLED",
                        juce::dontSendNotification);

    statusLabel.setVisible (play || specials);
    connectionLabel.setVisible (play);
    reconnectButton.setVisible (play);
    if (connectSpinner != nullptr)
        connectSpinner->setVisible (play && processorRef.isLedConnecting());
    testButton.setVisible (play);
    layoutButton.setVisible (play);
    settingsButton.setVisible (play);
    specialsButton.setVisible (play);
    backButton.setVisible (! play);

    firstNoteLabel.setVisible (layout);
    firstNoteBox.setVisible (layout);
    startLedLabel.setVisible (layout);
    startLedSlider.setVisible (layout);
    keyCountLabel.setVisible (layout);
    keyCountSlider.setVisible (layout);
    layoutTable.setVisible (layout);

    brightnessLabel.setVisible (settings);
    brightnessSlider.setVisible (settings);
    colourLabel.setVisible (settings);
    colourSelector.setVisible (settings);

    historySizeLabel.setVisible (specials);
    historySizeSlider.setVisible (specials);
    recallCountLabel.setVisible (specials);
    recallCountSlider.setVisible (specials);
    recallButton.setVisible (specials);
    chordWindowLabel.setVisible (specials);
    chordWindowSlider.setVisible (specials);
    chordButton.setVisible (specials);

    const bool editorPage = layout || settings;
    presetBox.setVisible (editorPage);
    presetNameEditor.setVisible (editorPage);
    saveButton.setVisible (editorPage);
    savedStatusLabel.setVisible (editorPage);

    if (layout || settings)
    {
        processorRef.stopStripTest();
        ignorePresetBox = true;
        processorRef.refreshPresetCombo (presetBox);
        ignorePresetBox = false;
        presetNameEditor.setText (processorRef.getLayoutProgramName(),
                                  juce::dontSendNotification);
        savedStatusLabel.setText ({}, juce::dontSendNotification);
        setSize (520, 620);
    }

    if (layout)
    {
        auto layoutCopy = processorRef.ledLayout();
        layoutCopy.makeSizesExplicit();
        processorRef.commitLayout (layoutCopy);
        syncLayoutControls();
        layoutTable.updateContent();
        layoutTable.selectRow (0);
    }
    else if (settings)
    {
        processorRef.setSettingsFillPreview (true);
        syncSettingsControls();
    }
    else if (specials)
    {
        processorRef.stopStripTest();
        syncSpecialsControls();
        setSize (520, 500);
    }
    else
    {
        verifying = false;
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

void PianoLEDAudioProcessorEditor::syncSettingsControls()
{
    ignoreColour = true;
    const auto& style = processorRef.ledStyle();
    brightnessSlider.setValue (style.brightnessPercent, juce::dontSendNotification);
    float hue = style.hue;
    while (hue < 0.0f) hue += 360.0f;
    while (hue >= 360.0f) hue -= 360.0f;
    colourSelector.setCurrentColour (juce::Colour::fromHSV (hue / 360.0f, style.saturation, 1.0f, 1.0f),
                                     juce::dontSendNotification);
    ignoreColour = false;
}

void PianoLEDAudioProcessorEditor::syncSpecialsControls()
{
    historySizeSlider.setValue (processorRef.getHistoryCapacity(), juce::dontSendNotification);
    const int n = std::max (1, processorRef.getHistoryCapacity());
    recallCountSlider.setRange (1.0, static_cast<double> (n), 1.0);
    recallCountSlider.setValue (processorRef.getRecallCount(), juce::dontSendNotification);
    chordWindowSlider.setValue (processorRef.getChordWindowMs(), juce::dontSendNotification);
}

void PianoLEDAudioProcessorEditor::applyHistoryCapacity()
{
    processorRef.setHistoryCapacity (static_cast<int> (historySizeSlider.getValue()));
    syncSpecialsControls();
}

void PianoLEDAudioProcessorEditor::applyRecallCount()
{
    processorRef.setRecallCount (static_cast<int> (recallCountSlider.getValue()));
}

void PianoLEDAudioProcessorEditor::applyChordWindow()
{
    processorRef.setChordWindowMs (static_cast<int> (chordWindowSlider.getValue()));
}

void PianoLEDAudioProcessorEditor::recallLastNotes()
{
    lastRecallWasChord = false;
    processorRef.recallLastNotes();
}

void PianoLEDAudioProcessorEditor::recallLastChord()
{
    lastRecallWasChord = true;
    processorRef.recallLastChord();
}

void PianoLEDAudioProcessorEditor::updateStatusLabel()
{
    if (processorRef.isStripTestRunning() && page == Page::play)
    {
        statusLabel.setText (utf8 (u8"\u0422\u0435\u0441\u0442: \u0431\u0435\u0433\u0443\u0449\u0438\u0439 \u0434\u0438\u043e\u0434 \u043f\u043e \u043b\u0435\u043d\u0442\u0435\u2026"),
                             juce::dontSendNotification);
        return;
    }

    if (processorRef.isHistoryPreview())
    {
        const auto recalled = processorRef.getDisplayNotes();
        if (recalled.count() == 0)
        {
            statusLabel.setText (utf8 (u8"\u0418\u0441\u0442\u043e\u0440\u0438\u044f \u043f\u0443\u0441\u0442\u0430 \u2014 \u0441\u043d\u0430\u0447\u0430\u043b\u0430 \u0441\u044b\u0433\u0440\u0430\u0439"),
                                 juce::dontSendNotification);
            return;
        }
        const auto prefix = lastRecallWasChord
                                ? utf8 (u8"\u041f\u043e\u0441\u043b\u0435\u0434\u043d\u0438\u0439 \u0430\u043a\u043a\u043e\u0440\u0434: ")
                                : utf8 (u8"\u041f\u043e\u0441\u043b\u0435\u0434\u043d\u0438\u0435 \u043d\u043e\u0442\u044b: ");
        statusLabel.setText (prefix + juce::String (recalled.count()) + "  |  "
                                 + chordTextFromSnapshot (recalled),
                             juce::dontSendNotification);
        return;
    }

    const auto snap = processorRef.getActiveNotes();
    const int held = snap.count();
    if (held == 0)
        statusLabel.setText ("Waiting for MIDI...", juce::dontSendNotification);
    else
    {
        const auto suffix = held == 1 ? " note" : " notes";
        statusLabel.setText (juce::String (held) + suffix + "  |  " + chordTextFromSnapshot (snap),
                             juce::dontSendNotification);
    }
}

void PianoLEDAudioProcessorEditor::applyBrightness()
{
    processorRef.setLedBrightness (static_cast<float> (brightnessSlider.getValue()));
}

void PianoLEDAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (ignoreColour || page != Page::settings)
        return;

    const auto colour = colourSelector.getCurrentColour();
    float hue = 0.0f, sat = 0.0f, bri = 1.0f;
    colour.getHSB (hue, sat, bri);
    processorRef.setLedHueSat (hue * 360.0f, sat);

    ignoreColour = true;
    colourSelector.setCurrentColour (juce::Colour::fromHSV (hue, sat, 1.0f, 1.0f),
                                     juce::dontSendNotification);
    ignoreColour = false;
}

juce::Colour PianoLEDAudioProcessorEditor::accentColour() const
{
    const auto rgb = processorRef.ledStyle().toRgb();
    return visibleLedColour (rgb.r, rgb.g, rgb.b);
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
    processorRef.setLayoutProgram (id - 1);
    presetNameEditor.setText (processorRef.getLayoutProgramName(), juce::dontSendNotification);
    syncLayoutControls();
    layoutTable.updateContent();
    layoutTable.selectRow (0);
    selectedRowsChanged (0);
    if (page == Page::settings)
        syncSettingsControls();
    if (page == Page::specials)
        syncSpecialsControls();
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
    if (page == Page::layout)
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

        g.setColour (on ? visibleLedColour (r, gg, b) : juce::Colour (0xff1a1e28));
        g.fillRect (static_cast<float> (stripBounds.getX()) + static_cast<float> (led) * ledWidth,
                    static_cast<float> (stripBounds.getY()),
                    std::max (1.0f, ledWidth - 0.4f),
                    static_cast<float> (stripBounds.getHeight()));
    }
}

void PianoLEDAudioProcessorEditor::drawKeyboard (juce::Graphics& g)
{
    const auto layout = processorRef.ledLayout();
    const auto notes = processorRef.getDisplayNotes();
    if (! layout.isValid() || keyboardBounds.isEmpty())
        return;

    const int lowest = layout.lowestNote;
    const int keys = layout.keyCount();
    if (keys <= 0)
        return;

    const int preview = verifying ? verifyKey : layoutTable.getSelectedRow();
    const bool blinkOn = verifying || ((juce::Time::currentTimeMillis() / 500) % 2) == 0;
    const auto accent = accentColour();

    const float keyWidth = static_cast<float> (keyboardBounds.getWidth()) / static_cast<float> (keys);

    for (int i = 0; i < keys; ++i)
    {
        const int note = lowest + i;
        const bool midiOn = notes.isOn (note);
        const bool editBlink = page == Page::layout && i == preview && blinkOn;
        const bool settingsFill = page == Page::settings;
        const bool on = midiOn || editBlink || settingsFill;
        const bool black = isBlackKey (note);
        const auto x = static_cast<float> (keyboardBounds.getX()) + static_cast<float> (i) * keyWidth;

        if (on)
            g.setColour (accent);
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
    if (page != Page::play)
        backButton.setBounds (titleRow.removeFromLeft (80));
    else
    {
        layoutButton.setBounds (titleRow.removeFromRight (110));
        titleRow.removeFromRight (6);
        testButton.setBounds (titleRow.removeFromRight (72));
        titleRow.removeFromRight (6);
        settingsButton.setBounds (titleRow.removeFromRight (110));
    }
    titleLabel.setBounds (titleRow);

    bounds.removeFromTop (8);

    if (page == Page::play)
    {
        statusLabel.setBounds (bounds.removeFromTop (48));
        bounds.removeFromTop (4);
        connectionLabel.setBounds (bounds.removeFromTop (32));
        bounds.removeFromTop (6);
        auto connectRow = bounds.removeFromTop (28).reduced (70, 0);
        if (connectSpinner != nullptr)
        {
            connectSpinner->setBounds (connectRow.removeFromLeft (22).withSizeKeepingCentre (20, 20));
            connectRow.removeFromLeft (8);
        }
        reconnectButton.setBounds (connectRow);
        bounds.removeFromTop (16);
        specialsButton.setBounds (bounds.removeFromTop (28).reduced (40, 0));
        bounds.removeFromTop (12);
    }
    else if (page == Page::layout)
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
    else if (page == Page::settings)
    {
        auto brightRow = bounds.removeFromTop (26);
        brightnessLabel.setBounds (brightRow.removeFromLeft (120));
        brightnessSlider.setBounds (brightRow);

        bounds.removeFromTop (6);
        colourLabel.setBounds (bounds.removeFromTop (20));
        colourSelector.setBounds (bounds.removeFromTop (230));

        bounds.removeFromTop (8);
        auto presetRow = bounds.removeFromTop (26);
        presetBox.setBounds (presetRow.removeFromLeft (140));
        presetRow.removeFromLeft (6);
        presetNameEditor.setBounds (presetRow.removeFromLeft (160));
        presetRow.removeFromLeft (6);
        saveButton.setBounds (presetRow);

        bounds.removeFromTop (4);
        savedStatusLabel.setBounds (bounds.removeFromTop (22));
        bounds.removeFromTop (10);
    }
    else if (page == Page::specials)
    {
        statusLabel.setBounds (bounds.removeFromTop (40));
        bounds.removeFromTop (8);

        auto historyRow = bounds.removeFromTop (24);
        historySizeLabel.setBounds (historyRow.removeFromLeft (160));
        historySizeSlider.setBounds (historyRow.removeFromLeft (120));

        bounds.removeFromTop (8);
        auto notesRow = bounds.removeFromTop (28);
        recallCountLabel.setBounds (notesRow.removeFromLeft (28));
        recallCountSlider.setBounds (notesRow.removeFromLeft (100));
        notesRow.removeFromLeft (8);
        recallButton.setBounds (notesRow);

        bounds.removeFromTop (10);
        auto windowRow = bounds.removeFromTop (26);
        chordWindowLabel.setBounds (windowRow.removeFromLeft (160));
        chordWindowSlider.setBounds (windowRow);

        bounds.removeFromTop (6);
        chordButton.setBounds (bounds.removeFromTop (28).reduced (40, 0));
        bounds.removeFromTop (12);
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

    if (page == Page::layout)
        tickVerifyPlayback();

    if (page == Page::play || page == Page::specials)
        updateStatusLabel();

    if (page == Page::play)
    {
        if (processorRef.isLedConnecting())
        {
            connectSpinner->setVisible (true);
            connectSpinner->repaint();
            reconnectButton.setEnabled (false);
            reconnectButton.setButtonText (utf8 (u8"\u041f\u043e\u0434\u043a\u043b\u044e\u0447\u0430\u044e\u2026"));
            connectionLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd8c56a));
            connectionLabel.setText (utf8 (u8"LED strip: \u043f\u043e\u0434\u043a\u043b\u044e\u0447\u0430\u044e \u043b\u0435\u043d\u0442\u0443\u2026 \u043f\u043e\u0434\u043e\u0436\u0434\u0438"),
                                     juce::dontSendNotification);
        }
        else if (processorRef.isLedConnected())
        {
            connectSpinner->setVisible (false);
            reconnectButton.setEnabled (true);
            reconnectButton.setButtonText (utf8 (u8"\u041f\u043e\u0434\u043a\u043b\u044e\u0447\u0438\u0442\u044c \u043b\u0435\u043d\u0442\u0443"));
            connectionLabel.setColour (juce::Label::textColourId, juce::Colour (0xff7ee0a8));
            connectionLabel.setText ("LED strip: " + processorRef.ledDevicePath() + "  |  "
                                         + juce::String (processorRef.ledStyle().brightnessPercent, 1) + "%",
                                     juce::dontSendNotification);
        }
        else
        {
            connectSpinner->setVisible (false);
            reconnectButton.setEnabled (true);
            reconnectButton.setButtonText (utf8 (u8"\u041f\u043e\u0434\u043a\u043b\u044e\u0447\u0438\u0442\u044c \u043b\u0435\u043d\u0442\u0443"));
            connectionLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe08a7e));
            auto error = processorRef.ledLastError().trim();
            juce::String text = utf8 (u8"LED strip: \u043d\u0435 \u043f\u043e\u0434\u043a\u043b\u044e\u0447\u0435\u043d\u0430");
            if (error.isNotEmpty())
                text += "  |  " + error.substring (0, 220);
            connectionLabel.setText (text, juce::dontSendNotification);
        }
    }

    repaint (stripBounds.expanded (2, 18));
    repaint (keyboardBounds.expanded (2, 2));
}
