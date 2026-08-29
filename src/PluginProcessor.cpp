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
}

PianoLEDAudioProcessor::~PianoLEDAudioProcessor()
{
    persistLayout();
}

void PianoLEDAudioProcessor::prepareToPlay (double, int)
{
    if (! ledBridge.isConnected() && ! ledBridge.isConnecting())
        ledBridge.reconnect();
}

void PianoLEDAudioProcessor::releaseResources()
{
}

void PianoLEDAudioProcessor::reset()
{
    ledBridge.panic();
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
}

juce::AudioProcessorEditor* PianoLEDAudioProcessor::createEditor()
{
    return new PianoLEDAudioProcessorEditor (*this);
}

void PianoLEDAudioProcessor::commitLayout (piano_led::StripLayout layout)
{
    ledBridge.setLayout (std::move (layout));
    syncCurrentPreset();
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
}

void PianoLEDAudioProcessor::setStartLed (int led)
{
    auto layout = ledBridge.layout();
    layout.startLed = juce::jlimit (0, std::max (0, layout.ledCount - 1), led);
    ledBridge.setLayout (std::move (layout));
    syncCurrentPreset();
}

void PianoLEDAudioProcessor::setMappedKeyCount (int keys)
{
    auto layout = ledBridge.layout();
    layout.setMappedKeyCount (keys);
    ledBridge.setLayout (std::move (layout));
    syncCurrentPreset();
}

void PianoLEDAudioProcessor::setKeySize (int keyIndex, int size)
{
    auto layout = ledBridge.layout();
    layout.setKeySize (keyIndex, size);
    ledBridge.setLayout (std::move (layout));
    syncCurrentPreset();
}

namespace
{
void writeLayoutXml (juce::XmlElement& el, const piano_led::StripLayout& layout)
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
} // namespace

void PianoLEDAudioProcessor::ensureDefaultPreset()
{
    if (! presets.empty()) return;
    auto layout = ledBridge.layout();
    layout.makeSizesExplicit();
    ledBridge.setLayout (layout);
    presets.push_back ({ "Default", std::move (layout) });
    currentProgram = 0;
}

void PianoLEDAudioProcessor::applyPreset (int index)
{
    if (index < 0 || index >= static_cast<int> (presets.size())) return;
    currentProgram = index;
    ledBridge.setLayout (presets[static_cast<std::size_t> (index)].layout);
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
    writeLayoutXml (xml, ledBridge.layout());
    for (const auto& preset : presets)
    {
        auto* child = xml.createNewChildElement ("Preset");
        child->setAttribute ("name", preset.name);
        writeLayoutXml (*child, preset.layout);
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
        loaded.push_back (std::move (preset));
    }

    if (loaded.empty())
    {
        LayoutPreset preset;
        preset.name = "Default";
        preset.layout = readLayoutXml (xml);
        loaded.push_back (std::move (preset));
    }

    presets = std::move (loaded);
    currentProgram = xml.getIntAttribute ("current", 0);
    if (currentProgram < 0 || currentProgram >= static_cast<int> (presets.size()))
        currentProgram = 0;
    applyPreset (currentProgram);
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

    auto name = requestedName.trim();
    if (name.isEmpty()) name = presets[static_cast<std::size_t> (currentProgram)].name;
    if (name.isEmpty()) name = "Layout";

    int found = -1;
    for (int i = 0; i < static_cast<int> (presets.size()); ++i)
        if (presets[static_cast<std::size_t> (i)].name == name) found = i;

    if (found >= 0)
    {
        presets[static_cast<std::size_t> (found)].layout = layout;
        currentProgram = found;
    }
    else
    {
        presets.push_back ({ name, layout });
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
    writeLayoutXml (xml, ledBridge.layout());
    for (const auto& preset : presets)
    {
        auto* child = xml.createNewChildElement ("Preset");
        child->setAttribute ("name", preset.name);
        writeLayoutXml (*child, preset.layout);
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
