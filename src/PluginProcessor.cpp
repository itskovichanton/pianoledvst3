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
}

void PianoLEDAudioProcessor::prepareToPlay (double, int)
{
}

void PianoLEDAudioProcessor::releaseResources()
{
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

    for (auto channel = 0; channel < buffer.getNumChannels(); ++channel)
        buffer.clear (channel, 0, buffer.getNumSamples());

    for (const auto metadata : midiMessages)
    {
        const auto message = metadata.getMessage();

        if (message.isNoteOn())
        {
            lastMidiNote.store (message.getNoteNumber(), std::memory_order_relaxed);
            lastMidiVelocity.store (message.getVelocity(), std::memory_order_relaxed);
        }
        else if (message.isNoteOff()
                 && message.getNoteNumber() == lastMidiNote.load (std::memory_order_relaxed))
        {
            lastMidiVelocity.store (0, std::memory_order_relaxed);
        }
    }
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
