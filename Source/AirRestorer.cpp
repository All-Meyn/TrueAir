#include "AirRestorer.h"

#include <cmath>

namespace
{
    constexpr float rectifierMix   = 0.35f;   // weight of |x| (even harmonics) vs tanh (odd)
    // Cap on envelope matching. Broadband (white) seeds need ~+29 dB because most of
    // their harmonic energy lands above host Nyquist; darker real-world seeds need less.
    // The cap is what limits junk when a seed has no audible harmonics (e.g. a pure
    // 12 kHz tone at 44.1 kHz), so it is kept just above the broadband requirement.
    constexpr float maxEnvGain     = 31.6f;   // +30 dB
    constexpr float gateLow        = 0.00025f; // -72 dBFS seed RMS: fully gated
    constexpr float gateHigh       = 0.001f;   // -60 dBFS seed RMS: fully open
    constexpr float attackSeconds  = 0.0015f;
    constexpr float releaseSeconds = 0.025f;

    // Q of each 2nd-order section of an 8th-order Butterworth filter.
    constexpr std::array<float, 4> butterworth8Q { 0.5098f, 0.6013f, 0.9000f, 2.5629f };

    void prepareFilter (juce::dsp::StateVariableTPTFilter<float>& f, const juce::dsp::ProcessSpec& spec,
                        juce::dsp::StateVariableTPTFilterType type)
    {
        f.prepare (spec);
        f.setType (type);
    }
}

int AirRestorer::chooseOversamplingFactor (double hostSampleRate) noexcept
{
    if (hostSampleRate <= 50000.0)  return 2; // 44.1/48 kHz -> 4x
    if (hostSampleRate <= 100000.0) return 1; // 88.2/96 kHz -> 2x
    return 0;                                 // 176.4 kHz and up -> 1x
}

void AirRestorer::prepare (double hostSampleRate, int maximumBlockSize, int numChannels, int oversamplingFactor)
{
    jassert (maximumBlockSize > 0 && numChannels > 0);

    if (oversamplingFactor < 0)
        oversamplingFactor = chooseOversamplingFactor (hostSampleRate);

    hostRate = hostSampleRate;
    maxBlockSize = maximumBlockSize;

    oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
        static_cast<size_t> (numChannels),
        static_cast<size_t> (oversamplingFactor),
        juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
        true,   // maximum quality
        true);  // integer latency, so the dry path can be delayed exactly
    oversampler->initProcessing (static_cast<size_t> (maximumBlockSize));

    latencySamples = juce::roundToInt (oversampler->getLatencyInSamples());

    const auto osFactor = static_cast<int> (oversampler->getOversamplingFactor());
    oversampledRate = hostSampleRate * osFactor;
    const auto osBlock = static_cast<size_t> (maximumBlockSize * osFactor);

    using Type = juce::dsp::StateVariableTPTFilterType;
    const juce::dsp::ProcessSpec osSpec   { oversampledRate, static_cast<juce::uint32> (osBlock), 1 };
    const juce::dsp::ProcessSpec hostSpec { hostSampleRate, static_cast<juce::uint32> (maximumBlockSize), 1 };

    channels.resize (static_cast<size_t> (numChannels));
    for (auto& ch : channels)
    {
        prepareFilter (ch.seedHighPass, osSpec, Type::highpass);
        prepareFilter (ch.seedLowPass1, osSpec, Type::lowpass);
        prepareFilter (ch.seedLowPass2, osSpec, Type::lowpass);
        for (size_t k = 0; k < ch.airHighPass.size(); ++k)
        {
            prepareFilter (ch.airHighPass[k], osSpec, Type::highpass);
            ch.airHighPass[k].setResonance (butterworth8Q[k]);
        }
        for (size_t k = 0; k < ch.airLowPass.size(); ++k)
        {
            prepareFilter (ch.airLowPass[k], osSpec, Type::lowpass);
            ch.airLowPass[k].setResonance (butterworth8Q[k]);
            ch.airLowPass[k].setCutoffFrequency (static_cast<float> (0.45 * hostSampleRate));
        }

        prepareFilter (ch.envSeedHighPass, hostSpec, Type::highpass);
        prepareFilter (ch.envSeedLowPass1, hostSpec, Type::lowpass);
        prepareFilter (ch.envSeedLowPass2, hostSpec, Type::lowpass);
    }

    seedDelay.prepare ({ hostSampleRate, static_cast<juce::uint32> (maximumBlockSize), static_cast<juce::uint32> (numChannels) });
    seedDelay.setMaximumDelayInSamples (latencySamples + 1);
    seedDelay.setDelay (static_cast<float> (latencySamples));

    driveRamp.assign (osBlock, 1.0f);
    tiltRamp.assign (static_cast<size_t> (maximumBlockSize), 0.0f);

    smoothedDrive.reset (oversampledRate, 0.05);
    smoothedTilt.reset (hostSampleRate, 0.05);
    smoothedDrive.setCurrentAndTargetValue (1.0f); // never 0: the shaper divides by drive
    smoothedTilt.setCurrentAndTargetValue (0.0f);

    attackCoeff  = std::exp (-1.0f / (attackSeconds  * static_cast<float> (hostSampleRate)));
    releaseCoeff = std::exp (-1.0f / (releaseSeconds * static_cast<float> (hostSampleRate)));

    lastCutoffHz = -1.0f;
    setFilterCutoffs (16000.0f);
    reset();
}

void AirRestorer::reset() noexcept
{
    if (oversampler != nullptr)
        oversampler->reset();

    seedDelay.reset();

    for (auto& ch : channels)
    {
        for (auto* f : { &ch.seedHighPass, &ch.seedLowPass1, &ch.seedLowPass2,
                         &ch.envSeedHighPass, &ch.envSeedLowPass1, &ch.envSeedLowPass2 })
            f->reset();
        for (auto& f : ch.airHighPass)
            f.reset();
        for (auto& f : ch.airLowPass)
            f.reset();
        ch.seedEnv = 0.0f;
        ch.airEnv  = 0.0f;
    }

    smoothedDrive.setCurrentAndTargetValue (smoothedDrive.getTargetValue());
    smoothedTilt.setCurrentAndTargetValue (smoothedTilt.getTargetValue());
}

void AirRestorer::setFilterCutoffs (float cutoffHz) noexcept
{
    // Filter coefficient updates cost a std::tan each, so only do them when the cutoff moves.
    if (juce::exactlyEqual (cutoffHz, lastCutoffHz))
        return;

    lastCutoffHz = cutoffHz;

    for (auto& ch : channels)
    {
        for (auto* f : { &ch.seedHighPass, &ch.envSeedHighPass })
            f->setCutoffFrequency (0.5f * cutoffHz);

        for (auto* f : { &ch.seedLowPass1, &ch.seedLowPass2, &ch.envSeedLowPass1, &ch.envSeedLowPass2 })
            f->setCutoffFrequency (cutoffHz);

        for (auto& f : ch.airHighPass)
            f.setCutoffFrequency (cutoffHz);
    }
}

void AirRestorer::process (const juce::dsp::AudioBlock<const float>& input,
                           juce::dsp::AudioBlock<float>& airOut,
                           float cutoffHz, float drive, float tilt) noexcept
{
    jassert (oversampler != nullptr);
    jassert (input.getNumSamples() <= static_cast<size_t> (maxBlockSize));
    jassert (input.getNumChannels() == channels.size() && airOut.getNumChannels() == channels.size());
    jassert (cutoffHz < 0.5f * static_cast<float> (hostRate));

    setFilterCutoffs (cutoffHz);

    // ---- Oversampled: generate band-limited harmonics -----------------------
    auto os = oversampler->processSamplesUp (input);
    const auto osN = os.getNumSamples();

    smoothedDrive.setTargetValue (juce::jmax (1.0f, drive));
    for (size_t i = 0; i < osN; ++i)
        driveRamp[i] = smoothedDrive.getNextValue();

    for (size_t c = 0; c < channels.size(); ++c)
    {
        auto& st = channels[c];
        auto* x = os.getChannelPointer (c);

        for (size_t i = 0; i < osN; ++i)
        {
            float seed = st.seedHighPass.processSample (0, x[i]);
            seed = st.seedLowPass1.processSample (0, seed);
            seed = st.seedLowPass2.processSample (0, seed);

            const float d = driveRamp[i];
            const float driven = seed * d;
            // Nonlinear part only: tanh(dx) - dx is pure odd-order distortion, |dx| adds
            // even orders. Keeping tanh's linear term would pass the seed straight through
            // and boost the existing treble just below the cutoff.
            const float shaped = (std::tanh (driven) - driven + rectifierMix * std::abs (driven)) / d;

            // Keep only new content above the cutoff (also removes |x|'s DC and difference
            // tones), and roll off what the downsampler would otherwise reject or fold back.
            float air = shaped;
            for (auto& f : st.airHighPass)
                air = f.processSample (0, air);
            for (auto& f : st.airLowPass)
                air = f.processSample (0, air);
            x[i] = air;
        }
    }

    oversampler->processSamplesDown (airOut);

    // ---- Host rate: envelope matching on what will actually be heard --------
    const auto n = input.getNumSamples();

    smoothedTilt.setTargetValue (tilt);
    for (size_t i = 0; i < n; ++i)
        tiltRamp[i] = smoothedTilt.getNextValue();

    for (size_t c = 0; c < channels.size(); ++c)
    {
        auto& st = channels[c];
        const auto* in = input.getChannelPointer (c);
        auto* air = airOut.getChannelPointer (c);
        const auto ci = static_cast<int> (c);

        float seedEnv = st.seedEnv;
        float airEnv  = st.airEnv;

        for (size_t i = 0; i < n; ++i)
        {
            seedDelay.pushSample (ci, in[i]);
            float seed = st.envSeedHighPass.processSample (0, seedDelay.popSample (ci));
            seed = st.envSeedLowPass1.processSample (0, seed);
            seed = st.envSeedLowPass2.processSample (0, seed);

            // Power (RMS) envelopes, so Brightness sets the actual level ratio whatever
            // the crest factor of the seed and of the synthesised air.
            const float seedPow = seed * seed;
            seedEnv = seedPow + (seedPow > seedEnv ? attackCoeff : releaseCoeff) * (seedEnv - seedPow);

            const float airPow = air[i] * air[i];
            airEnv = airPow + (airPow > airEnv ? attackCoeff : releaseCoeff) * (airEnv - airPow);

            // Soft gate between -72 and -60 dBFS (seed RMS) so near-silence can't pump up noise.
            const float seedRms = std::sqrt (seedEnv);
            const float gate = juce::jlimit (0.0f, 1.0f, (seedRms - gateLow) / (gateHigh - gateLow));
            const float matchGain = juce::jmin (std::sqrt (seedEnv / (airEnv + 1.0e-18f)), maxEnvGain);

            air[i] *= matchGain * tiltRamp[i] * gate;
        }

        st.seedEnv = seedEnv;
        st.airEnv  = airEnv;
    }
}
