#pragma once

#include "PluginProcessor.h"

class PianoLEDAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                           private juce::Timer,
                                           private juce::TableListBoxModel
{
public:
    explicit PianoLEDAudioProcessorEditor (PianoLEDAudioProcessor&);
    ~PianoLEDAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void setKeySizeFromCell (int row, int size);

private:
    void timerCallback() override;
    void drawStrip (juce::Graphics&);
    void drawKeyboard (juce::Graphics&);
    void showLayout (bool on);
    void syncLayoutControls();
    void applyFirstNote();
    void applyStartLed();
    void applyKeyCount();
    void savePreset();
    void loadSelectedPreset();
    void startVerifyPlayback();
    void tickVerifyPlayback();

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
    juce::TextButton testButton;
    juce::TextButton layoutButton;
    juce::TextButton backButton;
    juce::Label hintLabel;

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

    juce::Rectangle<int> stripBounds;
    juce::Rectangle<int> keyboardBounds;
    bool layoutMode = false;
    bool verifying = false;
    bool ignorePresetBox = false;
    juce::int64 verifyStartMs = 0;
    int verifyKey = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoLEDAudioProcessorEditor)
};
