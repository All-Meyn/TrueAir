#include "PluginProcessor.h"
#include "PluginEditor.h"

TrueAirAudioProcessor::TrueAirAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "TrueAirState", createParameterLayout())
{
    gainDb       = parameters.getRawParameterValue (TrueAirParams::gainId);
    amountPct    = parameters.getRawParameterValue (TrueAirParams::amountId);
    brightnessDb = parameters.getRawParameterValue (TrueAirParams::brightnessId);
    driveDb      = parameters.getRawParameterValue (TrueAirParams::driveId);
    autoCutoff   = parameters.getRawParameterValue (TrueAirParams::autoCutoffId);
    manualCutoff = parameters.getRawParameterValue (TrueAirParams::cutoffId);

    jassert (gainDb != nullptr && amountPct != nullptr && brightnessDb != nullptr
             && driveDb != nullptr && autoCutoff != nullptr && manualCutoff != nullptr);
}

juce::AudioProcessorValueTreeState::ParameterLayout TrueAirAudioProcessor::createParameterLayout()
{
    using namespace TrueAirParams;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { gainId, 1 },
        "Output Gain",
        juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { amountId, 1 },
        "Amount",
        juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f),
        50.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { brightnessId, 1 },
        "Brightness",
        juce::NormalisableRange<float> (-36.0f, -6.0f, 0.1f),
        -18.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { driveId, 1 },
        "Drive",
        juce::NormalisableRange<float> (0.0f, 24.0f, 0.1f),
        6.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { autoCutoffId, 1 },
        "Auto Cutoff",
        true));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { cutoffId, 1 },
        "Cutoff",
        juce::NormalisableRange<float> (CutoffDetector::minSearchHz, 20000.0f, 10.0f),
        16000.0f,
        juce::AudioParameterFloatAttributes().withLabel ("Hz")));

    return layout;
}

void TrueAirAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const int numChannels = getTotalNumOutputChannels();
    maxChunk = juce::jmax (1, samplesPerBlock);

    detector.prepare (sampleRate);
    restorer.prepare (sampleRate, maxChunk, numChannels);

    const int latency = restorer.getLatencySamples();
    setLatencySamples (latency);

    const juce::dsp::ProcessSpec spec { sampleRate,
                                        static_cast<juce::uint32> (maxChunk),
                                        static_cast<juce::uint32> (numChannels) };

    dryDelay.prepare (spec);
    dryDelay.setMaximumDelayInSamples (latency + 1);
    dryDelay.setDelay (static_cast<float> (latency));
    dryDelay.reset();

    outputGain.prepare (spec);
    outputGain.setRampDurationSeconds (0.05);
    outputGain.setGainDecibels (gainDb->load());
    outputGain.reset();

    airBuffer.setSize (numChannels, maxChunk);
    monoScratch.assign (static_cast<size_t> (maxChunk), 0.0f);
    mixScratch.assign (static_cast<size_t> (maxChunk), 0.0f);

    smoothedCutoff.reset (sampleRate, 0.2);
    smoothedCutoff.setCurrentAndTargetValue (manualCutoff->load());

    smoothedAmount.reset (sampleRate, 0.05);
    smoothedAmount.setCurrentAndTargetValue (amountPct->load() * 0.01f);

    restoreFade.reset (sampleRate, 0.3);
    restoreFade.setCurrentAndTargetValue (0.0f); // start bypassed until a cutoff is found

    airWasActive = false;
    uiRestoring.store (false, std::memory_order_relaxed);
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

    juce::dsp::AudioBlock<float> block (buffer);
    block = block.getSubsetChannelBlock (0, static_cast<size_t> (getTotalNumOutputChannels()));

    // Hosts may exceed the prepared block size; process in prepared-size chunks.
    const auto total = block.getNumSamples();
    for (size_t start = 0; start < total; start += static_cast<size_t> (maxChunk))
    {
        const auto len = juce::jmin (static_cast<size_t> (maxChunk), total - start);
        processChunk (block.getSubBlock (start, len));
    }
}

void TrueAirAudioProcessor::processChunk (juce::dsp::AudioBlock<float> block) noexcept
{
    const auto numChannels = block.getNumChannels();
    const auto n = block.getNumSamples();
    const auto ni = static_cast<int> (n);

    // 1. Cutoff detection on the mono input.
    juce::FloatVectorOperations::copy (monoScratch.data(), block.getChannelPointer (0), ni);
    for (size_t ch = 1; ch < numChannels; ++ch)
        juce::FloatVectorOperations::add (monoScratch.data(), block.getChannelPointer (ch), ni);
    juce::FloatVectorOperations::multiply (monoScratch.data(), 1.0f / static_cast<float> (numChannels), ni);
    detector.pushSamples (monoScratch.data(), ni);

    // 2. Decide cutoff and whether to restore at all.
    const bool useAuto = autoCutoff->load() >= 0.5f;
    const bool restore = useAuto ? detector.hasCliff() : true;

    if (useAuto)
    {
        if (detector.hasCliff())
            smoothedCutoff.setTargetValue (detector.getCutoffHz());
    }
    else
    {
        smoothedCutoff.setTargetValue (manualCutoff->load());
    }

    restoreFade.setTargetValue (restore ? 1.0f : 0.0f);
    smoothedAmount.setTargetValue (amountPct->load() * 0.01f);

    // Filter cutoffs update once per chunk; the 200 ms ramp keeps the steps small.
    const float cutoff = smoothedCutoff.skip (ni);

    uiCutoffHz.store (cutoff, std::memory_order_relaxed);
    uiRestoring.store (restore, std::memory_order_relaxed);

    // 3. Synthesise the air band (skipped entirely while fully bypassed).
    const bool airActive = restoreFade.isSmoothing() || restoreFade.getTargetValue() > 0.0f;

    auto airBlock = juce::dsp::AudioBlock<float> (airBuffer).getSubsetChannelBlock (0, numChannels).getSubBlock (0, n);

    if (airActive)
    {
        const float drive = juce::Decibels::decibelsToGain (driveDb->load());
        const float tilt  = juce::Decibels::decibelsToGain (brightnessDb->load());
        restorer.process (juce::dsp::AudioBlock<const float> (block), airBlock, cutoff, drive, tilt);

        for (size_t i = 0; i < n; ++i)
            mixScratch[i] = smoothedAmount.getNextValue() * restoreFade.getNextValue();
    }
    else
    {
        smoothedAmount.skip (ni);
        restoreFade.skip (ni);
    }

    // 4. Output = dry (delayed by the air path's latency) + air * amount * fade.
    for (size_t ch = 0; ch < numChannels; ++ch)
    {
        auto* io = block.getChannelPointer (ch);
        const auto* air = airBlock.getChannelPointer (ch);
        const auto c = static_cast<int> (ch);

        for (size_t i = 0; i < n; ++i)
        {
            dryDelay.pushSample (c, io[i]);
            const float dry = dryDelay.popSample (c);
            io[i] = airActive ? dry + air[i] * mixScratch[i] : dry;
        }
    }

    if (airWasActive && ! airActive)
        restorer.reset(); // start clean next time restoration fades in
    airWasActive = airActive;

    // 5. Output gain.
    outputGain.setGainDecibels (gainDb->load());
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
