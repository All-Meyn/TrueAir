#include "CutoffDetector.h"

#include <cmath>

namespace
{
    constexpr float silenceGateDb  = -80.0f; // skip frames whose 6–10 kHz level is below this (dBFS)
    constexpr float enterCliffDb   = 24.0f;  // drop needed to declare a codec cutoff
    constexpr float leaveCliffDb   = 12.0f;  // drop below which the cliff is considered gone
    constexpr float averageCoeff   = 0.25f;  // per-frame spectrum averaging (~200 ms at 44.1 kHz)
    constexpr float windowOuterHz  = 1000.0f;
    constexpr float windowInnerHz  = 200.0f;
}

void CutoffDetector::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;

    // Keep bins around 20–25 Hz wide whatever the host rate.
    fftOrder = sampleRate <= 50000.0 ? 11 : (sampleRate <= 100000.0 ? 12 : 13);
    fftSize = 1 << fftOrder;
    binWidthHz = static_cast<float> (sampleRate / fftSize);

    fft = std::make_unique<juce::dsp::FFT> (fftOrder);

    window.resize (static_cast<size_t> (fftSize));
    for (int i = 0; i < fftSize; ++i) // periodic Hann
        window[static_cast<size_t> (i)] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);

    fifo.assign (static_cast<size_t> (fftSize), 0.0f);
    fftData.assign (static_cast<size_t> (fftSize * 2), 0.0f);
    avgPower.assign (static_cast<size_t> (fftSize / 2 + 1), 0.0f);
    prefixDb.assign (static_cast<size_t> (fftSize / 2 + 2), 0.0f);

    regionFirstBin = hzToBin (6000.0f);
    regionLastBin  = juce::jmin (fftSize / 2 - 1,
                                 hzToBin (juce::jmin (maxSearchHz + windowOuterHz, 0.49f * (float) sampleRate)));

    reset();
}

void CutoffDetector::reset() noexcept
{
    std::fill (fifo.begin(), fifo.end(), 0.0f);
    std::fill (avgPower.begin(), avgPower.end(), 0.0f);
    fifoIndex = 0;
    haveAverage = false;
    cliff = false;
    cutoffHz = 16000.0f;
    lastDropDb = 0.0f;
}

int CutoffDetector::hzToBin (float hz) const noexcept
{
    return juce::jlimit (0, fftSize / 2, juce::roundToInt (hz / binWidthHz));
}

float CutoffDetector::meanDb (int fromBin, int toBin) const noexcept
{
    fromBin = juce::jlimit (regionFirstBin, regionLastBin, fromBin);
    toBin   = juce::jlimit (regionFirstBin, regionLastBin, toBin);
    if (toBin < fromBin)
        return -200.0f;

    const auto a = static_cast<size_t> (fromBin - regionFirstBin);
    const auto b = static_cast<size_t> (toBin - regionFirstBin + 1);
    return (prefixDb[b] - prefixDb[a]) / static_cast<float> (b - a);
}

void CutoffDetector::pushSamples (const float* mono, int numSamples) noexcept
{
    if (fft == nullptr)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        fifo[static_cast<size_t> (fifoIndex++)] = mono[i];

        if (fifoIndex == fftSize)
        {
            analyseFrame();
            fifoIndex = 0;
        }
    }
}

void CutoffDetector::analyseFrame() noexcept
{
    for (size_t i = 0; i < static_cast<size_t> (fftSize); ++i)
        fftData[i] = fifo[i] * window[i];
    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);

    fft->performFrequencyOnlyForwardTransform (fftData.data(), true);

    // Normalise so a full-scale sine reads ~0 dBFS (Hann coherent gain = 0.5).
    const float scale = 4.0f / static_cast<float> (fftSize);

    // Silence gate on this frame's 6–10 kHz level: don't let quiet passages change the decision.
    {
        const int lo = hzToBin (6000.0f), hi = hzToBin (10000.0f);
        float p = 0.0f;
        for (int b = lo; b <= hi; ++b)
        {
            const float m = fftData[static_cast<size_t> (b)] * scale;
            p += m * m;
        }
        p /= static_cast<float> (hi - lo + 1);
        if (10.0f * std::log10 (p + 1.0e-30f) < silenceGateDb)
            return;
    }

    // Long-term average of power per bin, then dB prefix sums over the analysed region.
    for (int b = regionFirstBin; b <= regionLastBin; ++b)
    {
        const auto idx = static_cast<size_t> (b);
        const float m = fftData[idx] * scale;
        const float p = m * m;
        avgPower[idx] = haveAverage ? avgPower[idx] + averageCoeff * (p - avgPower[idx]) : p;
    }
    haveAverage = true;

    prefixDb[0] = 0.0f;
    for (int b = regionFirstBin; b <= regionLastBin; ++b)
    {
        const auto r = static_cast<size_t> (b - regionFirstBin);
        prefixDb[r + 1] = prefixDb[r] + 10.0f * std::log10 (avgPower[static_cast<size_t> (b)] + 1.0e-30f);
    }

    // Find the steepest cliff: level just below f minus level just above f.
    const float maxCandidateHz = juce::jmin (maxSearchHz, 0.49f * (float) sampleRate - windowOuterHz);
    const int firstCandidate = hzToBin (minSearchHz);
    const int lastCandidate  = hzToBin (maxCandidateHz);

    const int outer = juce::roundToInt (windowOuterHz / binWidthHz);
    const int inner = juce::roundToInt (windowInnerHz / binWidthHz);

    // The largest drop decides WHETHER there is a cliff; the first candidate (going up
    // from the passband) whose drop crosses the threshold says WHERE it starts. Using
    // the largest drop for the location drifts up into a stopband that keeps falling.
    const float locateThreshold = cliff ? leaveCliffDb : enterCliffDb;
    float bestDrop = -1000.0f;
    int bestBin = firstCandidate;
    int firstStrongBin = -1;

    for (int b = firstCandidate; b <= lastCandidate; ++b)
    {
        const float below = meanDb (b - outer, b - inner);
        const float above = meanDb (b + inner, b + outer);
        const float drop = below - above;

        if (drop > bestDrop)
        {
            bestDrop = drop;
            bestBin = b;
        }

        if (firstStrongBin < 0 && drop >= locateThreshold)
            firstStrongBin = b;
    }

    lastDropDb = bestDrop;

    if (! cliff && bestDrop >= enterCliffDb)
        cliff = true;
    else if (cliff && bestDrop < leaveCliffDb)
        cliff = false;

    if (cliff)
    {
        // Walk up from the passband to where the level is 6 dB down, so restoration
        // starts where the content ends.
        const int anchor = firstStrongBin >= 0 ? firstStrongBin : bestBin;
        const float passband = meanDb (anchor - outer, anchor - inner);
        int edge = anchor;
        for (int b = anchor - outer; b <= anchor + outer; ++b)
        {
            if (meanDb (b - 1, b + 1) < passband - 6.0f)
            {
                edge = b;
                break;
            }
        }
        cutoffHz = static_cast<float> (edge) * binWidthHz;
    }
}
