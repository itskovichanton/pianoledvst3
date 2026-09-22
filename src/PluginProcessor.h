#pragma once

#include "LedBridgeGlue.h"
#include "MidiDevicePlayer.h"
#include "piano_led/midi_thru.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>

class PianoLEDAudioProcessor final : public juce::AudioProcessor
{
public:
    PianoLEDAudioProcessor();
    ~PianoLEDAudioProcessor() override;

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
    const juce::String getProgramName (int) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    piano_led::NoteBitmask::Snapshot getActiveNotes() const { return ledBridge.activeNotes(); }
    piano_led::NoteBitmask::Snapshot getDisplayNotes() const
    {
        return ledBridge.isHistoryPreview() ? ledBridge.historyPreviewNotes()
                                            : ledBridge.activeNotes();
    }
    bool isHistoryPreview() const { return ledBridge.isHistoryPreview(); }
    const piano_led::StripLayout& ledLayout() const { return ledBridge.layout(); }
    const std::vector<std::uint8_t>& lastLedFrame() const { return ledBridge.lastFrame(); }

    bool isLedConnected() const { return ledBridge.isConnected(); }
    juce::String ledDevicePath() const { return ledBridge.devicePath(); }
    juce::String ledLastError() const { return ledBridge.lastError(); }
    bool reconnectLeds() { return ledBridge.reconnect(); }
    bool isLedConnecting() const { return ledBridge.isConnecting(); }

    void commitLayout (piano_led::StripLayout layout);
    void setLayoutPreviewNote (int midiNote) { ledBridge.setLayoutPreviewNote (midiNote); }
    void setLayoutPreviewHold (bool hold) { ledBridge.setLayoutPreviewHold (hold); }
    void setSettingsFillPreview (bool on) { ledBridge.setSettingsFillPreview (on); }
    void startStripTest() { ledBridge.startChase(); }
    void stopStripTest() { ledBridge.stopChase(); }
    bool isStripTestRunning() const { return ledBridge.isChasing(); }
    void recallLastNotes();
    void recallLastChord();
    void clearHistoryPreview() { ledBridge.clearHistoryPreview(); }
    int historySize() const { return ledBridge.historySize(); }
    int getHistoryCapacity() const { return historyCapacity; }
    void setHistoryCapacity (int n);
    int getRecallCount() const { return recallCount; }
    void setRecallCount (int m);
    int getChordWindowMs() const { return chordWindowMs; }
    void setChordWindowMs (int ms);
    void setFirstNote (int midiNote);
    void setStartLed (int led);
    void setMappedKeyCount (int keys);
    void setKeySize (int keyIndex, int size);

    const piano_led::LedStyle& ledStyle() const { return ledBridge.ledStyle(); }
    void setLedStyle (piano_led::LedStyle style);
    void setLedBrightness (float percent);
    void setLedHueSat (float hue, float saturation);

    juce::String saveLayoutPreset (const juce::String& name);
    void refreshPresetCombo (juce::ComboBox& box) const;
    void persistLayout();
    int getLayoutProgramIndex() const { return currentProgram; }
    void setLayoutProgram (int index);
    juce::String getLayoutProgramName() const;

    bool isPlayOnDevice() const { return playOnDevice; }
    void setPlayOnDevice (bool on);
    juce::String midiDeviceIdentifier() const { return midiDeviceId; }
    juce::String midiDeviceName() const { return midiDeviceName_; }
    void setMidiDevice (const juce::String& identifier, const juce::String& name);
    int getMidiChannel() const { return midiConfig.channel; }
    void setMidiChannel (int channel);
    bool midiMappedKeysOnly() const { return midiConfig.mappedKeysOnly; }
    void setMidiMappedKeysOnly (bool on);
    bool midiSendSustain() const { return midiConfig.sendSustain; }
    void setMidiSendSustain (bool on);
    bool midiSendPitchBend() const { return midiConfig.sendPitchBend; }
    void setMidiSendPitchBend (bool on);
    bool midiSendModulation() const { return midiConfig.sendModulation; }
    void setMidiSendModulation (bool on);
    bool midiSendProgramChange() const { return midiConfig.sendProgramChange; }
    void setMidiSendProgramChange (bool on);
    bool isMidiDeviceOpen() const { return midiPlayer.isOpen(); }
    juce::String midiPlayStatusText() const { return midiPlayer.statusText(); }
    void panicMidi() { midiPlayer.panic(); }
    void playChordOnDevice (const piano_led::NoteBitmask::Snapshot& notes, int seconds);
    void stopChordOnDevice();
    int getRecallPlaySeconds() const { return recallPlaySeconds; }
    void setRecallPlaySeconds (int seconds);

private:
    struct LayoutPreset
    {
        juce::String name;
        piano_led::StripLayout layout;
        piano_led::LedStyle style;
        int historyCapacity = 30;
        int recallCount = 8;
        int chordWindowMs = 50;
    };

    piano_led::PluginLedBridge ledBridge;
    MidiDevicePlayer midiPlayer;
    std::vector<LayoutPreset> presets;
    int currentProgram = 0;
    int historyCapacity = 30;
    int recallCount = 8;
    int chordWindowMs = 50;
    bool playOnDevice = false;
    juce::String midiDeviceId;
    juce::String midiDeviceName_;
    piano_led::MidiThruConfig midiConfig;
    int recallPlaySeconds = 10;

    void ensureDefaultPreset();
    void applyPreset (int index);
    void syncCurrentPreset();
    void savePresetsToDisk();
    void loadPresetsFromDisk();
    bool applyStateXml (const juce::XmlElement&);
    void notifyHostState();
    void pushMidiConfig();
    void applyMidiToPlayer();
    static std::vector<juce::File> presetStoreFiles();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoLEDAudioProcessor)
};
