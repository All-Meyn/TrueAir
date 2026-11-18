#include "PluginProcessor.h"
#include "PluginEditor.h"

TrueAirAudioProcessor::TrueAirAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "TrueAirState", createParameterLayout())
{
    gainDb = parameters.getRawParameterValue (TrueAirParams::gainId);
    jassert (gainDb != nullptr);
}

juce::AudioProcessorValueTreeState::ParameterLayout TrueAirAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { TrueAirParams::gainId, 1 },
        "Output Gain",
        juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    return layout;
}

void TrueAirAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const juce::dsp::ProcessSpec spec { sampleRate,
                                        static_cast<juce::uint32> (samplesPerBlock),
                                        static_cast<juce::uint32> (getTotalNumOutputChannels()) };
    outputGain.prepare (spec);
    outputGain.setRampDurationSeconds (0.05);
    outputGain.setGainDecibels (gainDb->load());
    outputGain.reset();
}

void TrueAirAudioProcessor::releaseResources() {}

bool TrueAirAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return out == layouts.getMainInputChannelSet();
}

void TrueAirAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (auto ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    outputGain.setGainDecibels (gainDb->load());

    juce::dsp::AudioBlock<float> block (buffer);
    outputGain.process (juce::dsp::ProcessContextReplacing<float> (block));
}

juce::AudioProcessorEditor* TrueAirAudioProcessor::createEditor()
{
    return new TrueAirAudioProcessorEditor (*this);
}

void TrueAirAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = parameters.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void TrueAirAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (parameters.state.getType()))
            parameters.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TrueAirAudioProcessor();
}
