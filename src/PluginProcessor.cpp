#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>

PianoLEDAudioProcessor::PianoLEDAudioProcessor()
    : AudioProcessor (BusesProperties()
#if ! JucePlugin_IsMidiEffect
#if ! JucePlugin_IsSynth
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
#endif
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
#endif
      )
{
    ledBridge.start();
    loadPresetsFromDisk();
    ensureDefaultPreset();
    ledBridge.setHistoryCapacity (historyCapacity);
    applyMidiToPlayer();
}

PianoLEDAudioProcessor::~PianoLEDAudioProcessor()
{
    midiPlayer.panic();
    persistLayout();
}

void PianoLEDAudioProcessor::prepareToPlay (double, int)
{
    if (! ledBridge.isConnected() && ! ledBridge.isConnecting())
        ledBridge.reconnect();
}

void PianoLEDAudioProcessor::releaseResources()
{
    midiPlayer.panic();
}

void PianoLEDAudioProcessor::reset()
{
    ledBridge.panic();
    midiPlayer.panic();
}

bool PianoLEDAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
#else
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

#if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
#endif

    return true;
#endif
}

void PianoLEDAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                           juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    /* Инструмент не издаёт звука — тишина, чтобы не шипело в микшере.
     * MIDI всё равно приходит: GarageBand отдаёт ноты зелёному слоту. */
    for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
        buffer.clear (channel, 0, buffer.getNumSamples());

    ledBridge.processMidi (midiMessages);
    midiPlayer.processMidi (midiMessages);
}

juce::AudioProcessorEditor* PianoLEDAudioProcessor::createEditor()
{
    return new PianoLEDAudioProcessorEditor (*this);
}

void PianoLEDAudioProcessor::commitLayout (piano_led::StripLayout layout)
{
    ledBridge.setLayout (std::move (layout));
    syncCurrentPreset();
    pushMidiConfig();
}

void PianoLEDAudioProcessor::setFirstNote (int midiNote)
{
    auto layout = ledBridge.layout();
    layout.makeSizesExplicit();
    midiNote = juce::jlimit (0, 127, midiNote);
    layout.lowestNote = midiNote;
    if (layout.highestNote() > 127)
        layout.setMappedKeyCount (128 - midiNote);
    ledBridge.setLayout (std::move (layout));
    syncCurrentPreset();
    pushMidiConfig();
}

void PianoLEDAudioProcessor::setStartLed (int led)
{
    auto layout = ledBridge.layout();
    layout.startLed = juce::jlimit (0, std::max (0, layout.ledCount - 1), led);
    ledBridge.setLayout (std::move (layout));
    syncCurrentPreset();
    pushMidiConfig();
}

void PianoLEDAudioProcessor::setMappedKeyCount (int keys)
{
    auto layout = ledBridge.layout();
    layout.setMappedKeyCount (keys);
    ledBridge.setLayout (std::move (layout));
    syncCurrentPreset();
    pushMidiConfig();
}

void PianoLEDAudioProcessor::setKeySize (int keyIndex, int size)
{
    auto layout = ledBridge.layout();
    layout.setKeySize (keyIndex, size);
    ledBridge.setLayout (std::move (layout));
    syncCurrentPreset();
    pushMidiConfig();
}

void PianoLEDAudioProcessor::setLedStyle (piano_led::LedStyle style)
{
    ledBridge.setLedStyle (std::move (style));
    syncCurrentPreset();
}

void PianoLEDAudioProcessor::setLedBrightness (float percent)
{
    auto style = ledBridge.ledStyle();
    style.brightnessPercent = juce::jlimit (0.1f, 20.0f, percent);
    setLedStyle (style);
}

void PianoLEDAudioProcessor::setLedHueSat (float hue, float saturation)
{
    auto style = ledBridge.ledStyle();
    style.hue = hue;
    style.saturation = juce::jlimit (0.0f, 1.0f, saturation);
    setLedStyle (style);
}

void PianoLEDAudioProcessor::setHistoryCapacity (int n)
{
    historyCapacity = juce::jlimit (1, piano_led::NoteHistory::kMax, n);
    if (recallCount > historyCapacity)
        recallCount = historyCapacity;
    ledBridge.setHistoryCapacity (historyCapacity);
    syncCurrentPreset();
}

void PianoLEDAudioProcessor::setRecallCount (int m)
{
    recallCount = juce::jlimit (1, historyCapacity, m);
    syncCurrentPreset();
}

void PianoLEDAudioProcessor::setChordWindowMs (int ms)
{
    chordWindowMs = juce::jlimit (5, 250, ms);
    syncCurrentPreset();
}

void PianoLEDAudioProcessor::pushMidiConfig()
{
    midiConfig.lowestNote = ledBridge.layout().lowestNote;
    midiConfig.highestNote = ledBridge.layout().highestNote();
    midiPlayer.setConfig (midiConfig);
}

void PianoLEDAudioProcessor::applyMidiToPlayer()
{
    pushMidiConfig();
    midiPlayer.setDevice (midiDeviceId, midiDeviceName_);
    midiPlayer.setEnabled (playOnDevice);
}

void PianoLEDAudioProcessor::setPlayOnDevice (bool on)
{
    playOnDevice = on;
    applyMidiToPlayer();
    persistLayout();
}

void PianoLEDAudioProcessor::setMidiDevice (const juce::String& identifier, const juce::String& name)
{
    midiDeviceId = identifier;
    midiDeviceName_ = name;
    midiPlayer.setDevice (midiDeviceId, midiDeviceName_);
    if (playOnDevice)
        midiPlayer.setEnabled (true);
    persistLayout();
}

void PianoLEDAudioProcessor::setMidiChannel (int channel)
{
    midiConfig.channel = juce::jlimit (0, 16, channel);
    pushMidiConfig();
}

void PianoLEDAudioProcessor::setMidiMappedKeysOnly (bool on)
{
    midiConfig.mappedKeysOnly = on;
    pushMidiConfig();
}

void PianoLEDAudioProcessor::setMidiSendSustain (bool on)
{
    midiConfig.sendSustain = on;
    pushMidiConfig();
}

void PianoLEDAudioProcessor::setMidiSendPitchBend (bool on)
{
    midiConfig.sendPitchBend = on;
    pushMidiConfig();
}

void PianoLEDAudioProcessor::setMidiSendModulation (bool on)
{
    midiConfig.sendModulation = on;
    pushMidiConfig();
}

void PianoLEDAudioProcessor::setMidiSendProgramChange (bool on)
{
    midiConfig.sendProgramChange = on;
    pushMidiConfig();
}

void PianoLEDAudioProcessor::recallLastNotes()
{
    ledBridge.stopChase();
    ledBridge.recallLastNotes (recallCount);
}

void PianoLEDAudioProcessor::recallLastChord()
{
    ledBridge.stopChase();
    ledBridge.recallLastChord (chordWindowMs);
}

namespace
{
void writeLayoutXml (juce::XmlElement& el, const piano_led::StripLayout& layout,
                     const piano_led::LedStyle& style, int historySize, int recallM,
                     int chordMs)
{
    el.setAttribute ("lowestNote", layout.lowestNote);
    el.setAttribute ("startLed", layout.startLed);
    el.setAttribute ("ledCount", layout.ledCount);
    el.setAttribute ("keyCount", layout.keyCount());
    juce::String sizes;
    for (int i = 0; i < layout.keyCount(); ++i)
    {
        if (i > 0) sizes += ",";
        sizes += juce::String (layout.sizeForKey (i));
    }
    el.setAttribute ("sizes", sizes);
    el.setAttribute ("brightness", style.brightnessPercent);
    el.setAttribute ("hue", static_cast<double> (style.hue));
    el.setAttribute ("sat", static_cast<double> (style.saturation));
    el.setAttribute ("historySize", historySize);
    el.setAttribute ("recallCount", recallM);
    el.setAttribute ("chordWindowMs", chordMs);
}

piano_led::StripLayout readLayoutXml (const juce::XmlElement& el)
{
    piano_led::StripLayout layout;
    layout.ledCount = el.getIntAttribute ("ledCount", 144);
    layout.lowestNote = el.getIntAttribute ("lowestNote", 36);
    layout.startLed = el.getIntAttribute ("startLed", 0);
    layout.setMappedKeyCount (el.getIntAttribute ("keyCount", 48));
    const auto sizes = el.getStringAttribute ("sizes");
    if (sizes.isNotEmpty())
    {
        juce::StringArray parts;
        parts.addTokens (sizes, ",", {});
        for (int i = 0; i < parts.size() && i < layout.keyCount(); ++i)
            layout.setKeySize (i, parts[i].getIntValue());
    }
    return layout;
}

piano_led::LedStyle readStyleXml (const juce::XmlElement& el)
{
    piano_led::LedStyle style;
    style.brightnessPercent = static_cast<float> (
        juce::jlimit (0.1, 20.0, el.getDoubleAttribute ("brightness", 2.0)));
    style.hue = static_cast<float> (el.getDoubleAttribute ("hue", 0.0));
    style.saturation = static_cast<float> (juce::jlimit (0.0, 1.0, el.getDoubleAttribute ("sat", 1.0)));
    return style;
}

int readHistorySizeXml (const juce::XmlElement& el)
{
    return juce::jlimit (1, piano_led::NoteHistory::kMax,
                         el.getIntAttribute ("historySize", piano_led::NoteHistory::kDefaultCapacity));
}

int readRecallCountXml (const juce::XmlElement& el, int historySize)
{
    return juce::jlimit (1, historySize, el.getIntAttribute ("recallCount", 8));
}

int readChordWindowXml (const juce::XmlElement& el)
{
    return juce::jlimit (5, 250, el.getIntAttribute ("chordWindowMs", 50));
}

void writeMidiXml (juce::XmlElement& el, bool playOnDevice, const juce::String& deviceId,
                   const juce::String& deviceName, const piano_led::MidiThruConfig& cfg)
{
    el.setAttribute ("playOnDevice", playOnDevice ? 1 : 0);
    el.setAttribute ("midiDeviceId", deviceId);
    el.setAttribute ("midiDeviceName", deviceName);
    el.setAttribute ("midiChannel", cfg.channel);
    el.setAttribute ("midiMappedKeysOnly", cfg.mappedKeysOnly ? 1 : 0);
    el.setAttribute ("midiSustain", cfg.sendSustain ? 1 : 0);
    el.setAttribute ("midiPitchBend", cfg.sendPitchBend ? 1 : 0);
    el.setAttribute ("midiModulation", cfg.sendModulation ? 1 : 0);
    el.setAttribute ("midiProgramChange", cfg.sendProgramChange ? 1 : 0);
}

void readMidiXml (const juce::XmlElement& el, bool& playOnDevice, juce::String& deviceId,
                  juce::String& deviceName, piano_led::MidiThruConfig& cfg)
{
    playOnDevice = el.getIntAttribute ("playOnDevice", 0) != 0;
    deviceId = el.getStringAttribute ("midiDeviceId");
    deviceName = el.getStringAttribute ("midiDeviceName");
    cfg.channel = juce::jlimit (0, 16, el.getIntAttribute ("midiChannel", 1));
    cfg.mappedKeysOnly = el.getIntAttribute ("midiMappedKeysOnly", 0) != 0;
    cfg.sendSustain = el.getIntAttribute ("midiSustain", 1) != 0;
    cfg.sendPitchBend = el.getIntAttribute ("midiPitchBend", 0) != 0;
    cfg.sendModulation = el.getIntAttribute ("midiModulation", 0) != 0;
    cfg.sendProgramChange = el.getIntAttribute ("midiProgramChange", 0) != 0;
}
} // namespace

void PianoLEDAudioProcessor::ensureDefaultPreset()
{
    if (! presets.empty()) return;
    auto layout = ledBridge.layout();
    layout.makeSizesExplicit();
    ledBridge.setLayout (layout);
    presets.push_back ({ "Default", std::move (layout), ledBridge.ledStyle(),
                         historyCapacity, recallCount, chordWindowMs });
    currentProgram = 0;
}

void PianoLEDAudioProcessor::applyPreset (int index)
{
    if (index < 0 || index >= static_cast<int> (presets.size())) return;
    currentProgram = index;
    const auto& preset = presets[static_cast<std::size_t> (index)];
    ledBridge.setLayout (preset.layout);
    ledBridge.setLedStyle (preset.style);
    historyCapacity = juce::jlimit (1, piano_led::NoteHistory::kMax, preset.historyCapacity);
    recallCount = juce::jlimit (1, historyCapacity, preset.recallCount);
    chordWindowMs = juce::jlimit (5, 250, preset.chordWindowMs);
    ledBridge.setHistoryCapacity (historyCapacity);
    pushMidiConfig();
}

void PianoLEDAudioProcessor::setLayoutProgram (int index)
{
    ensureDefaultPreset();
    if (index < 0 || index >= static_cast<int> (presets.size()) || index == currentProgram)
        return;
    applyPreset (index);
}

juce::String PianoLEDAudioProcessor::getLayoutProgramName() const
{
    if (currentProgram < 0 || currentProgram >= static_cast<int> (presets.size()))
        return "Default";
    return presets[static_cast<std::size_t> (currentProgram)].name;
}

const juce::String PianoLEDAudioProcessor::getProgramName (int)
{
    return getLayoutProgramName();
}

void PianoLEDAudioProcessor::syncCurrentPreset()
{
    ensureDefaultPreset();
    if (currentProgram < 0 || currentProgram >= static_cast<int> (presets.size()))
        return;
    auto layout = ledBridge.layout();
    layout.makeSizesExplicit();
    presets[static_cast<std::size_t> (currentProgram)].layout = std::move (layout);
    presets[static_cast<std::size_t> (currentProgram)].style = ledBridge.ledStyle();
    presets[static_cast<std::size_t> (currentProgram)].historyCapacity = historyCapacity;
    presets[static_cast<std::size_t> (currentProgram)].recallCount = recallCount;
    presets[static_cast<std::size_t> (currentProgram)].chordWindowMs = chordWindowMs;
}

void PianoLEDAudioProcessor::notifyHostState()
{
    updateHostDisplay();
}

std::vector<juce::File> PianoLEDAudioProcessor::presetStoreFiles()
{
    return {
        juce::File::getSpecialLocation (juce::File::userHomeDirectory)
            .getChildFile ("Library/Application Support/PianoLED/layouts.xml"),
        juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
            .getChildFile ("PianoLED/layouts.xml")
    };
}

void PianoLEDAudioProcessor::savePresetsToDisk()
{
    ensureDefaultPreset();
    syncCurrentPreset();
    juce::XmlElement xml ("PianoLED");
    xml.setAttribute ("current", currentProgram);
    writeLayoutXml (xml, ledBridge.layout(), ledBridge.ledStyle(), historyCapacity, recallCount,
                    chordWindowMs);
    writeMidiXml (xml, playOnDevice, midiDeviceId, midiDeviceName_, midiConfig);
    for (const auto& preset : presets)
    {
        auto* child = xml.createNewChildElement ("Preset");
        child->setAttribute ("name", preset.name);
        writeLayoutXml (*child, preset.layout, preset.style, preset.historyCapacity,
                        preset.recallCount, preset.chordWindowMs);
    }

    for (auto file : presetStoreFiles())
    {
        file.getParentDirectory().createDirectory();
        xml.writeTo (file);
    }
}

bool PianoLEDAudioProcessor::applyStateXml (const juce::XmlElement& xml)
{
    if (! xml.hasTagName ("PianoLED"))
        return false;

    std::vector<LayoutPreset> loaded;
    for (auto* child : xml.getChildWithTagNameIterator ("Preset"))
    {
        LayoutPreset preset;
        preset.name = child->getStringAttribute ("name", "Layout");
        preset.layout = readLayoutXml (*child);
        preset.style = readStyleXml (*child);
        preset.historyCapacity = readHistorySizeXml (*child);
        preset.recallCount = readRecallCountXml (*child, preset.historyCapacity);
        preset.chordWindowMs = readChordWindowXml (*child);
        loaded.push_back (std::move (preset));
    }

    if (loaded.empty())
    {
        LayoutPreset preset;
        preset.name = "Default";
        preset.layout = readLayoutXml (xml);
        preset.style = readStyleXml (xml);
        preset.historyCapacity = readHistorySizeXml (xml);
        preset.recallCount = readRecallCountXml (xml, preset.historyCapacity);
        preset.chordWindowMs = readChordWindowXml (xml);
        loaded.push_back (std::move (preset));
    }

    presets = std::move (loaded);
    currentProgram = xml.getIntAttribute ("current", 0);
    if (currentProgram < 0 || currentProgram >= static_cast<int> (presets.size()))
        currentProgram = 0;
    applyPreset (currentProgram);
    readMidiXml (xml, playOnDevice, midiDeviceId, midiDeviceName_, midiConfig);
    applyMidiToPlayer();
    return true;
}

void PianoLEDAudioProcessor::loadPresetsFromDisk()
{
    juce::File newest;
    juce::int64 bestTime = -1;
    for (const auto& file : presetStoreFiles())
    {
        if (! file.existsAsFile())
            continue;
        const auto time = file.getLastModificationTime().toMilliseconds();
        if (time >= bestTime)
        {
            bestTime = time;
            newest = file;
        }
    }
    if (! newest.existsAsFile())
        return;
    if (auto xml = juce::XmlDocument::parse (newest))
        applyStateXml (*xml);
}

void PianoLEDAudioProcessor::persistLayout()
{
    syncCurrentPreset();
    savePresetsToDisk();
    notifyHostState();
}

juce::String PianoLEDAudioProcessor::saveLayoutPreset (const juce::String& requestedName)
{
    ensureDefaultPreset();
    auto layout = ledBridge.layout();
    layout.makeSizesExplicit();
    ledBridge.setLayout (layout);
    const auto style = ledBridge.ledStyle();

    auto name = requestedName.trim();
    if (name.isEmpty()) name = presets[static_cast<std::size_t> (currentProgram)].name;
    if (name.isEmpty()) name = "Layout";

    int found = -1;
    for (int i = 0; i < static_cast<int> (presets.size()); ++i)
        if (presets[static_cast<std::size_t> (i)].name == name) found = i;

    if (found >= 0)
    {
        presets[static_cast<std::size_t> (found)].layout = layout;
        presets[static_cast<std::size_t> (found)].style = style;
        presets[static_cast<std::size_t> (found)].historyCapacity = historyCapacity;
        presets[static_cast<std::size_t> (found)].recallCount = recallCount;
        presets[static_cast<std::size_t> (found)].chordWindowMs = chordWindowMs;
        currentProgram = found;
    }
    else
    {
        presets.push_back ({ name, layout, style, historyCapacity, recallCount, chordWindowMs });
        currentProgram = static_cast<int> (presets.size()) - 1;
    }

    persistLayout();
    return name;
}

void PianoLEDAudioProcessor::refreshPresetCombo (juce::ComboBox& box) const
{
    const auto previous = box.getText();
    box.clear (juce::dontSendNotification);
    for (int i = 0; i < static_cast<int> (presets.size()); ++i)
        box.addItem (presets[static_cast<std::size_t> (i)].name, i + 1);
    if (currentProgram >= 0 && currentProgram < static_cast<int> (presets.size()))
        box.setSelectedId (currentProgram + 1, juce::dontSendNotification);
    else if (previous.isNotEmpty())
        box.setText (previous, juce::dontSendNotification);
}

void PianoLEDAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    ensureDefaultPreset();
    syncCurrentPreset();
    juce::XmlElement xml ("PianoLED");
    xml.setAttribute ("current", currentProgram);
    writeLayoutXml (xml, ledBridge.layout(), ledBridge.ledStyle(), historyCapacity, recallCount,
                    chordWindowMs);
    writeMidiXml (xml, playOnDevice, midiDeviceId, midiDeviceName_, midiConfig);
    for (const auto& preset : presets)
    {
        auto* child = xml.createNewChildElement ("Preset");
        child->setAttribute ("name", preset.name);
        writeLayoutXml (*child, preset.layout, preset.style, preset.historyCapacity,
                        preset.recallCount, preset.chordWindowMs);
    }
    copyXmlToBinary (xml, destData);
}

void PianoLEDAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr)
        return;
    if (applyStateXml (*xml))
        savePresetsToDisk();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PianoLEDAudioProcessor();
}
