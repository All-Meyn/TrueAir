#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>

/**
    Synthesises the high-frequency "air" band that a lossy codec removed.

    Oversampled, per channel:
      seed  = band-pass [cutoff/2, cutoff]          (top surviving octave)
      shape = tanh(d*seed) - d*seed + 0.35*|d*seed| (odd + even harmonics; the linear
                                                     term is removed so the seed itself
                                                     never reaches the output)
      air   = 48 dB/oct high-pass at cutoff, 48 dB/oct low-pass at 0.45*host rate
    then downsampled, and at the host rate:
      out   = air * (seedEnv / airEnv) * tilt       (follow the seed's dynamics)

    Envelope matching runs AFTER downsampling, so it measures only the air that is
    actually output: most harmonic energy lands above host Nyquist and is discarded.
    The seed envelope comes from the input delayed by the oversampling latency so
    both envelopes line up.

    process() writes ONLY the synthesised air (host rate) into airOut. The caller adds
    it to the dry signal delayed by getLatencySamples(); the dry path never goes
    through the oversampling filters.

    All memory is allocated in prepare(); process() never allocates.
*/
class AirRestorer
{
public:
    /** Oversampling stages (2^factor) used for a given host rate: aim for >= ~176 kHz. */
    static int chooseOversamplingFactor (double hostSampleRate) noexcept;

    /** oversamplingFactor < 0 means chooseOversamplingFactor(hostSampleRate). */
    void prepare (double hostSampleRate, int maximumBlockSize, int numChannels, int oversamplingFactor = -1);
    void reset() noexcept;

    /** Whole-sample latency of the air path; delay the dry signal by this much. */
    int getLatencySamples() const noexcept { return latencySamples; }

    int getMaximumBlockSize() const noexcept { return maxBlockSize; }

    /**
        @param input      host-rate input (numChannels x n, n <= maximumBlockSize)
        @param airOut     receives the synthesised air, same size as input
        @param cutoffHz   codec cutoff: seed band ends here, air starts here
        @param drive      waveshaper drive, linear gain (>= 1)
        @param tilt       air level relative to the seed band, linear gain
    */
    void process (const juce::dsp::AudioBlock<const float>& input,
                  juce::dsp::AudioBlock<float>& airOut,
                  float cutoffHz, float drive, float tilt) noexcept;

private:
    using Filter = juce::dsp::StateVariableTPTFilter<float>;

    struct ChannelState
    {
        // Oversampled rate: harmonic generation.
        Filter seedHighPass, seedLowPass1, seedLowPass2;
        std::array<Filter, 4> airHighPass; // 8th-order Butterworth
        std::array<Filter, 4> airLowPass;  // 8th-order Butterworth

        // Host rate: seed band of the (latency-aligned) input, for envelope matching.
        Filter envSeedHighPass, envSeedLowPass1, envSeedLowPass2;
        float seedEnv = 0.0f; // power envelopes (mean square)
        float airEnv  = 0.0f;
    };

    void setFilterCutoffs (float cutoffHz) noexcept;

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> seedDelay;
    std::vector<ChannelState> channels;
    std::vector<float> driveRamp, tiltRamp;

    juce::SmoothedValue<float> smoothedDrive, smoothedTilt;

    double hostRate = 0.0, oversampledRate = 0.0;
    int maxBlockSize = 0;
    int latencySamples = 0;
    float lastCutoffHz = -1.0f;
    float attackCoeff = 0.0f, releaseCoeff = 0.0f;
};
