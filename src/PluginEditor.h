#pragma once

#include "PluginProcessor.h"

#include <juce_gui_extra/juce_gui_extra.h>
#include <memory>

class PianoLEDAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                           private juce::Timer,
                                           private juce::TableListBoxModel,
                                           private juce::ChangeListener
{
public:
    explicit PianoLEDAudioProcessorEditor (PianoLEDAudioProcessor&);
    ~PianoLEDAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void setKeySizeFromCell (int row, int size);

private:
    enum class Page { play, layout, settings, specials, synth };

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void drawStrip (juce::Graphics&);
    void drawKeyboard (juce::Graphics&);
    void showPage (Page next);
    void leaveCurrentPage();
    void syncLayoutControls();
    void syncSettingsControls();
    void syncSpecialsControls();
    void syncSynthControls();
    void refreshMidiDeviceBox();
    void applyMidiDevice();
    void applyMidiChannel();
    void applyFirstNote();
    void applyStartLed();
    void applyKeyCount();
    void applyBrightness();
    void applyHistoryCapacity();
    void applyRecallCount();
    void applyChordWindow();
    void recallLastNotes();
    void recallLastChord();
    void savePreset();
    void loadSelectedPreset();
    void startVerifyPlayback();
    void tickVerifyPlayback();
    juce::Colour accentColour() const;
    void updateStatusLabel();
    void updateMidiStatusLabel();
    void styleToggle (juce::ToggleButton& button);

    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int rowNumber, int width, int height,
                             bool rowIsSelected) override;
    void paintCell (juce::Graphics&, int rowNumber, int columnId, int width, int height,
                    bool rowIsSelected) override;
    juce::Component* refreshComponentForCell (int rowNumber, int columnId, bool isRowSelected,
                                              juce::Component* existing) override;
    void selectedRowsChanged (int lastRowSelected) override;

    PianoLEDAudioProcessor& processorRef;
    juce::Label titleLabel;
    juce::Label statusLabel;
    juce::Label connectionLabel;
    juce::TextButton reconnectButton;
    std::unique_ptr<juce::Component> connectSpinner;
    juce::TextButton testButton;
    juce::TextButton layoutButton;
    juce::TextButton settingsButton;
    juce::TextButton specialsButton;
    juce::TextButton synthButton;
    juce::TextButton backButton;
    juce::Label hintLabel;

    juce::ToggleButton playOnDeviceButton;
    juce::Label midiStatusLabel;

    juce::Label firstNoteLabel;
    juce::ComboBox firstNoteBox;
    juce::Label startLedLabel;
    juce::Slider startLedSlider;
    juce::Label keyCountLabel;
    juce::Slider keyCountSlider;
    juce::ComboBox presetBox;
    juce::TextEditor presetNameEditor;
    juce::TextButton saveButton;
    juce::Label savedStatusLabel;
    juce::TableListBox layoutTable;

    juce::Label brightnessLabel;
    juce::Slider brightnessSlider;
    juce::Label colourLabel;
    juce::ColourSelector colourSelector { juce::ColourSelector::showColourAtTop
                                              | juce::ColourSelector::showColourspace
                                              | juce::ColourSelector::showSliders,
                                          4, 7 };

    juce::Label historySizeLabel;
    juce::Slider historySizeSlider;
    juce::Label recallCountLabel;
    juce::Slider recallCountSlider;
    juce::TextButton recallButton;
    juce::Label chordWindowLabel;
    juce::Slider chordWindowSlider;
    juce::TextButton chordButton;

    juce::Label midiDeviceLabel;
    juce::ComboBox midiDeviceBox;
    juce::TextButton midiRefreshButton;
    juce::Label midiChannelLabel;
    juce::ComboBox midiChannelBox;
    juce::TextButton midiPanicButton;
    juce::ToggleButton midiMappedKeysButton;
    juce::Label midiExtraLabel;
    juce::ToggleButton midiSustainButton;
    juce::ToggleButton midiPitchBendButton;
    juce::ToggleButton midiModulationButton;
    juce::ToggleButton midiProgramChangeButton;
    juce::Label midiHintLabel;
    juce::Array<juce::MidiDeviceInfo> midiDevices;

    juce::Rectangle<int> stripBounds;
    juce::Rectangle<int> keyboardBounds;
    Page page = Page::play;
    bool verifying = false;
    bool ignorePresetBox = false;
    bool ignoreColour = false;
    bool ignoreMidiDeviceBox = false;
    bool lastRecallWasChord = false;
    juce::int64 verifyStartMs = 0;
    int verifyKey = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoLEDAudioProcessorEditor)
};
