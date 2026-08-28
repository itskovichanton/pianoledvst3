#include "PluginProcessor.h"
#include "PluginEditor.h"

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
}

void PianoLEDAudioProcessor::prepareToPlay (double, int)
{
    if (! ledBridge.isConnected())
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

void PianoLEDAudioProcessor::getStateInformation (juce::MemoryBlock&)
{
}

void PianoLEDAudioProcessor::setStateInformation (const void*, int)
{
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PianoLEDAudioProcessor();
}
