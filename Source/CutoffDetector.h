#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>

/**
    Detects the brickwall low-pass that lossy codecs (MP3/AAC/SBC) leave in a signal.

    Runs on the audio thread: feed it mono samples at the host rate. Every FFT
    frame it updates a long-term averaged spectrum and looks for a *cliff*: a
    drop of many dB within a few hundred Hz. Natural high-frequency roll-off in
    lossless recordings is far gentler, so it does not trigger the detector.

    All memory is allocated in prepare(); pushSamples() never allocates.
*/
class CutoffDetector
{
public:
    static constexpr float minSearchHz = 11000.0f;
    static constexpr float maxSearchHz = 20500.0f;

    void prepare (double sampleRate);
    void reset() noexcept;

    void pushSamples (const float* mono, int numSamples) noexcept;

    /** True while a codec-style cliff is detected (with hysteresis). */
    bool hasCliff() const noexcept       { return cliff; }

    /** Frequency of the most recently detected cliff, in Hz. */
    float getCutoffHz() const noexcept   { return cutoffHz; }

    /** Largest dB drop measured in the last analysed frame (for tests/diagnostics). */
    float getLastDropDb() const noexcept { return lastDropDb; }

private:
    void analyseFrame() noexcept;
    int hzToBin (float hz) const noexcept;
    float meanDb (int fromBin, int toBin) const noexcept; // inclusive range, in region-relative prefix sums

    double sampleRate = 44100.0;
    int fftOrder = 11;
    int fftSize = 1 << 11;
    float binWidthHz = 0.0f;

    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, fifo, fftData, avgPower, prefixDb;
    int fifoIndex = 0;
    bool haveAverage = false;

    int regionFirstBin = 0, regionLastBin = 0; // bins analysed (gate band up to Nyquist guard)

    bool cliff = false;
    float cutoffHz = 16000.0f;
    float lastDropDb = 0.0f;
};
