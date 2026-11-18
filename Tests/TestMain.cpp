// Headless tests for the TrueAir processor. Run via `ctest`.
#include "PluginProcessor.h"

#include <cmath>

namespace
{
    constexpr double testRate = 44100.0;

    void setParam (TrueAirAudioProcessor& p, const char* id, float plainValue)
    {
        auto* param = p.getState().getParameter (id);
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
    }

    void setGainDb (TrueAirAudioProcessor& p, float db) { setParam (p, TrueAirParams::gainId, db); }

    float renderSteadyState (TrueAirAudioProcessor& p, float input)
    {
        constexpr int blockSize = 512;
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        // Run past the 50 ms gain ramp and the latency, then measure the last sample.
        for (int i = 0; i < 20; ++i)
        {
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), input, blockSize);
            p.processBlock (buffer, midi);
        }
        return buffer.getSample (0, blockSize - 1);
    }

    //==========================================================================
    // Test signals (generated in double precision, deterministic seeds).

    std::vector<double> whiteNoise (int numSamples, juce::int64 seed, double amplitude)
    {
        juce::Random rng (seed);
        std::vector<double> x (static_cast<size_t> (numSamples));
        for (auto& v : x)
            v = amplitude * (2.0 * rng.nextDouble() - 1.0);
        return x;
    }

    void applyFirstOrderLowPass (std::vector<double>& x, double rate, double hz)
    {
        juce::dsp::IIR::Filter<double> f (juce::dsp::IIR::Coefficients<double>::makeFirstOrderLowPass (rate, hz));
        for (auto& v : x)
            v = f.processSample (v);
    }

    /** Steep Butterworth low-pass standing in for a codec's brickwall. */
    void applyBrickwall (std::vector<double>& x, double rate, double hz, int order = 48)
    {
        auto coeffs = juce::dsp::FilterDesign<double>::designIIRLowpassHighOrderButterworthMethod (hz, rate, order);
        for (auto* c : coeffs)
        {
            juce::dsp::IIR::Filter<double> f (c);
            for (auto& v : x)
                v = f.processSample (v);
        }
    }

    juce::AudioBuffer<float> toStereo (const std::vector<double>& x)
    {
        juce::AudioBuffer<float> b (2, static_cast<int> (x.size()));
        for (int ch = 0; ch < 2; ++ch)
            for (size_t i = 0; i < x.size(); ++i)
                b.setSample (ch, static_cast<int> (i), static_cast<float> (x[i]));
        return b;
    }

    /** Runs the processor over a whole buffer, hostBlock samples at a time. */
    juce::AudioBuffer<float> runProcessor (TrueAirAudioProcessor& p, const juce::AudioBuffer<float>& in, int hostBlock)
    {
        juce::AudioBuffer<float> out (in);
        juce::MidiBuffer midi;
        for (int start = 0; start < out.getNumSamples(); start += hostBlock)
        {
            const int len = juce::jmin (hostBlock, out.getNumSamples() - start);
            juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), out.getNumChannels(), start, len);
            p.processBlock (view, midi);
        }
        return out;
    }

    /** Mean power (dBFS, sine-referenced) of channel 0 in [loHz, hiHz], from sample `from` on. */
    float bandLevelDb (const juce::AudioBuffer<float>& b, int from, float loHz, float hiHz, double rate = testRate)
    {
        constexpr int order = 12, size = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> data (size * 2), win (size), acc (size / 2 + 1, 0.0f);
        for (int i = 0; i < size; ++i)
            win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size);

        int frames = 0;
        for (int start = from; start + size <= b.getNumSamples(); start += size / 2, ++frames)
        {
            for (int i = 0; i < size; ++i)
                data[(size_t) i] = b.getSample (0, start + i) * win[(size_t) i];
            std::fill (data.begin() + size, data.end(), 0.0f);
            fft.performFrequencyOnlyForwardTransform (data.data(), true);
            for (size_t k = 0; k < acc.size(); ++k)
            {
                const float m = data[k] * 4.0f / (float) size;
                acc[k] += m * m;
            }
        }

        const auto binHz = (float) rate / (float) size;
        const int lo = juce::roundToInt (loHz / binHz), hi = juce::roundToInt (hiHz / binHz);
        double sum = 0.0;
        for (int k = lo; k <= hi; ++k)
            sum += acc[(size_t) k] / (float) juce::jmax (1, frames);
        return (float) (10.0 * std::log10 (sum / (hi - lo + 1) + 1.0e-30));
    }

    /** Total power (dB) of channel 0 in [loHz, hiHz]: mean per bin plus 10*log10(number of bins). */
    float bandTotalDb (const juce::AudioBuffer<float>& b, int from, float loHz, float hiHz, double rate = testRate)
    {
        const auto binHz = (float) rate / 4096.0f;
        const int bins = juce::roundToInt (hiHz / binHz) - juce::roundToInt (loHz / binHz) + 1;
        // For noise, a Hann-windowed bin normalised to read sines in dBFS holds 3x the true
        // per-bin power (one-sided spectrum x Hann ENBW of 1.5 bins); undo that.
        return bandLevelDb (b, from, loHz, hiHz, rate) + 10.0f * std::log10 ((float) bins / 3.0f);
    }
}

//==============================================================================
class TrueAirProcessorTests final : public juce::UnitTest
{
public:
    TrueAirProcessorTests() : juce::UnitTest ("TrueAirProcessor", "TrueAir") {}

    void runTest() override
    {
        beginTest ("Unity gain passes audio unchanged");
        {
            TrueAirAudioProcessor p;
            p.prepareToPlay (48000.0, 512);
            expectWithinAbsoluteError (renderSteadyState (p, 0.5f), 0.5f, 1.0e-6f);
        }

        beginTest ("+6 dB roughly doubles amplitude");
        {
            TrueAirAudioProcessor p;
            p.prepareToPlay (48000.0, 512);
            setGainDb (p, 6.0f);
            const auto expected = 0.25f * juce::Decibels::decibelsToGain (6.0f);
            expectWithinAbsoluteError (renderSteadyState (p, 0.25f), expected, 1.0e-4f);
        }

        beginTest ("State round-trips through get/setStateInformation");
        {
            TrueAirAudioProcessor a;
            setGainDb (a, -12.0f);
            setParam (a, TrueAirParams::amountId, 80.0f);
            setParam (a, TrueAirParams::autoCutoffId, 0.0f);
            setParam (a, TrueAirParams::cutoffId, 17500.0f);
            juce::MemoryBlock state;
            a.getStateInformation (state);

            TrueAirAudioProcessor b;
            b.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
            auto& s = b.getState();
            expectWithinAbsoluteError (s.getRawParameterValue (TrueAirParams::gainId)->load(), -12.0f, 1.0e-3f);
            expectWithinAbsoluteError (s.getRawParameterValue (TrueAirParams::amountId)->load(), 80.0f, 1.0e-3f);
            expectWithinAbsoluteError (s.getRawParameterValue (TrueAirParams::autoCutoffId)->load(), 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (s.getRawParameterValue (TrueAirParams::cutoffId)->load(), 17500.0f, 1.0f);
        }
    }
};

//==============================================================================
class CutoffDetectorTests final : public juce::UnitTest
{
public:
    CutoffDetectorTests() : juce::UnitTest ("CutoffDetector", "TrueAir") {}

    void runTest() override
    {
        const int n = (int) testRate * 2;

        auto detect = [] (const std::vector<double>& x, double rate)
        {
            CutoffDetector d;
            d.prepare (rate);
            std::vector<float> f (x.begin(), x.end());
            d.pushSamples (f.data(), (int) f.size());
            return d;
        };

        for (const float cutoff : { 16000.0f, 19000.0f })
        {
            beginTest ("Finds a " + juce::String (cutoff / 1000.0f, 0) + " kHz brickwall in white noise");
            auto x = whiteNoise (n, 1, 0.25);
            applyBrickwall (x, testRate, cutoff);
            const auto d = detect (x, testRate);
            logMessage ("  drop " + juce::String (d.getLastDropDb(), 1) + " dB at " + juce::String (d.getCutoffHz(), 0) + " Hz");
            expect (d.hasCliff());
            expectWithinAbsoluteError (d.getCutoffHz(), cutoff, 600.0f);
        }

        beginTest ("Finds a 16 kHz brickwall in dark (pink-ish) material");
        {
            auto x = whiteNoise (n, 2, 0.5);
            applyFirstOrderLowPass (x, testRate, 2000.0);
            applyBrickwall (x, testRate, 16000.0);
            const auto d = detect (x, testRate);
            logMessage ("  drop " + juce::String (d.getLastDropDb(), 1) + " dB at " + juce::String (d.getCutoffHz(), 0) + " Hz");
            expect (d.hasCliff());
            expectWithinAbsoluteError (d.getCutoffHz(), 16000.0f, 600.0f);
        }

        beginTest ("Full-bandwidth white noise is not a codec cutoff");
        {
            const auto d = detect (whiteNoise (n, 3, 0.25), testRate);
            logMessage ("  drop " + juce::String (d.getLastDropDb(), 1) + " dB");
            expect (! d.hasCliff());
        }

        beginTest ("Dark lossless recording (natural roll-off) is not a codec cutoff");
        {
            auto x = whiteNoise (n, 4, 0.5);
            applyFirstOrderLowPass (x, testRate, 1500.0);
            applyFirstOrderLowPass (x, testRate, 6000.0); // ~12 dB/oct above 6 kHz
            const auto d = detect (x, testRate);
            logMessage ("  drop " + juce::String (d.getLastDropDb(), 1) + " dB");
            expect (! d.hasCliff());
        }

        beginTest ("Silence leaves the detector idle");
        {
            const auto d = detect (std::vector<double> ((size_t) n, 0.0), testRate);
            expect (! d.hasCliff());
        }
    }
};

//==============================================================================
class RestorationTests final : public juce::UnitTest
{
public:
    RestorationTests() : juce::UnitTest ("Restoration", "TrueAir") {}

    void runTest() override
    {
        const int n = (int) testRate * 3;

        beginTest ("Restores energy above a 16 kHz codec cutoff");
        {
            auto x = whiteNoise (n, 10, 0.25);
            applyBrickwall (x, testRate, 16000.0);
            const auto in = toStereo (x);

            TrueAirAudioProcessor p;
            p.prepareToPlay (testRate, 512);
            const auto out = runProcessor (p, in, 512);

            const int from = (int) testRate; // after detection + fade-in
            const float inAir   = bandLevelDb (in,  from, 17000.0f, 20000.0f);
            const float outAir  = bandLevelDb (out, from, 17000.0f, 20000.0f);
            const float outSeed = bandLevelDb (out, from, 8000.0f, 15000.0f);
            logMessage ("  17-20 kHz: in " + juce::String (inAir, 1) + " dB, out " + juce::String (outAir, 1)
                        + " dB; 8-15 kHz out " + juce::String (outSeed, 1) + " dB; latency "
                        + juce::String (p.getLatencySamples()) + " samples");

            expect (p.isRestoring());
            expectGreaterThan (outAir - inAir, 20.0f, "air band should be restored");
            expectLessThan (outAir, outSeed - 6.0f, "restored air should sit well below the seed band");
        }

        auto expectBitExactDelay = [this] (const std::vector<double>& x, int prepareBlock, int hostBlock)
        {
            const auto in = toStereo (x);
            TrueAirAudioProcessor p;
            p.prepareToPlay (testRate, prepareBlock);
            const int latency = p.getLatencySamples();
            const auto out = runProcessor (p, in, hostBlock);

            expect (latency > 0, "4x oversampling at 44.1 kHz should report latency");
            expect (! p.isRestoring());

            int mismatches = 0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < out.getNumSamples(); ++i)
                {
                    const float expected = i >= latency ? in.getSample (ch, i - latency) : 0.0f;
                    if (! juce::exactlyEqual (out.getSample (ch, i), expected))
                        ++mismatches;
                }
            expectEquals (mismatches, 0, "output must equal input delayed by the reported latency");
        };

        beginTest ("Full-bandwidth material passes bit-exact, delayed by reported latency");
        expectBitExactDelay (whiteNoise (n, 11, 0.25), 512, 512);

        beginTest ("Dark lossless material passes bit-exact (host blocks larger than prepared)");
        {
            auto x = whiteNoise (n, 12, 0.5);
            applyFirstOrderLowPass (x, testRate, 1500.0);
            applyFirstOrderLowPass (x, testRate, 6000.0);
            expectBitExactDelay (x, 256, 1000);
        }

        beginTest ("Manual cutoff restores even without detection");
        {
            auto x = whiteNoise (n, 13, 0.25);
            applyBrickwall (x, testRate, 16000.0);
            const auto in = toStereo (x);

            TrueAirAudioProcessor p;
            setParam (p, TrueAirParams::autoCutoffId, 0.0f);
            setParam (p, TrueAirParams::cutoffId, 16000.0f);
            p.prepareToPlay (testRate, 512);
            const auto out = runProcessor (p, in, 512);
            expectGreaterThan (bandLevelDb (out, (int) testRate, 17000.0f, 20000.0f)
                                 - bandLevelDb (in, (int) testRate, 17000.0f, 20000.0f), 20.0f);
        }

        beginTest ("Air level follows Brightness relative to the seed band");
        {
            auto x = whiteNoise ((int) testRate * 2, 15, 0.25);
            applyBrickwall (x, testRate, 16000.0);
            juce::AudioBuffer<float> in (1, (int) x.size()), air (1, (int) x.size());
            for (size_t i = 0; i < x.size(); ++i)
                in.setSample (0, (int) i, (float) x[i]);

            for (const float tiltDb : { 0.0f, -18.0f })
            {
                AirRestorer r;
                r.prepare (testRate, 512, 1);
                for (int start = 0; start < in.getNumSamples(); start += 512)
                {
                    const auto blk = (size_t) juce::jmin (512, in.getNumSamples() - start);
                    juce::dsp::AudioBlock<const float> ib (in);
                    juce::dsp::AudioBlock<float> ob (air);
                    auto obSub = ob.getSubBlock ((size_t) start, blk);
                    r.process (ib.getSubBlock ((size_t) start, blk), obSub, 16000.0f,
                               juce::Decibels::decibelsToGain (6.0f), juce::Decibels::decibelsToGain (tiltDb));
                }

                const int from = (int) testRate / 2;
                const float seedTotal = bandTotalDb (in,  from, 8000.0f, 16000.0f);
                const float airTotal  = bandTotalDb (air, from, 16000.0f, 21500.0f);
                logMessage ("  tilt " + juce::String (tiltDb, 0) + " dB: seed band " + juce::String (seedTotal, 1)
                            + " dB, air " + juce::String (airTotal, 1) + " dB, ratio " + juce::String (airTotal - seedTotal, 1) + " dB");
                expectWithinAbsoluteError (airTotal - seedTotal, tiltDb, 3.0f);

                // Air must land ABOVE the cutoff, not boost the existing treble below it.
                // Measured against the treble that is already there (10-15 kHz input):
                // -12 dB worst case changes it by ~0.3 dB; -30 dB by ~0.03 dB.
                const float leakDb  = bandLevelDb (air, from, 10000.0f, 15000.0f)
                                    - bandLevelDb (in,  from, 10000.0f, 15000.0f);
                logMessage ("    air leaking into 10-15 kHz: " + juce::String (leakDb, 1) + " dB vs existing treble");
                expectLessThan (leakDb, tiltDb < -12.0f ? -30.0f : -12.0f, "synthesised air leaking below the cutoff");
            }
        }

        for (const int factor : { 1, 2 })
        {
            beginTest ("Worst-case aliasing at " + juce::String (1 << factor) + "x oversampling (pure 12 kHz tone)");
            // A 12 kHz seed has no harmonics below 24 kHz, so ANY 16-22 kHz output is aliasing.
            // Worst case: 0 dB tilt, envelope matching pinned at its +24 dB cap.
            AirRestorer r;
            r.prepare (testRate, 512, 1, factor);

            const int len = (int) testRate;
            juce::AudioBuffer<float> in (1, len), air (1, len);
            for (int i = 0; i < len; ++i)
                in.setSample (0, i, 0.5f * std::sin (juce::MathConstants<float>::twoPi * 12000.0f * (float) i / (float) testRate));

            for (int start = 0; start < len; start += 512)
            {
                const auto blk = (size_t) juce::jmin (512, len - start);
                juce::dsp::AudioBlock<const float> ib (in);
                juce::dsp::AudioBlock<float> ob (air);
                auto obSub = ob.getSubBlock ((size_t) start, blk);
                r.process (ib.getSubBlock ((size_t) start, blk), obSub, 16000.0f, juce::Decibels::decibelsToGain (6.0f), 1.0f);
            }

            // Total alias power vs the tone's power (0.5 amplitude sine = -9 dB).
            const float toneDb  = 10.0f * std::log10 (0.5f * 0.5f * 0.5f);
            const float aliasDb = bandTotalDb (air, len / 2, 16000.0f, 21500.0f) - toneDb;
            logMessage ("  alias in 16-21.5 kHz: " + juce::String (aliasDb, 1) + " dB vs the tone at 0 dB Brightness, 100% Amount ("
                        + juce::String (aliasDb - 18.0f - 6.0f, 1) + " dB at the default -18 dB / 50%)");
            if (factor == AirRestorer::chooseOversamplingFactor (testRate))
                expectLessThan (aliasDb, -35.0f, "worst-case aliasing at the shipped oversampling factor");
        }

        for (const double rate : { 48000.0, 96000.0, 192000.0 })
        {
            beginTest ("Restores a 16 kHz cutoff at " + juce::String (rate / 1000.0, 0) + " kHz host rate");
            auto x = whiteNoise ((int) rate * 3, 16, 0.25);
            applyBrickwall (x, rate, 16000.0);
            const auto in = toStereo (x);

            TrueAirAudioProcessor p;
            p.prepareToPlay (rate, 480);
            const auto out = runProcessor (p, in, 480);

            bool finite = true;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < out.getNumSamples(); ++i)
                    finite = finite && std::isfinite (out.getSample (ch, i));

            // From 17.5 kHz: at high host rates the test brickwall leaves real input energy near 17 kHz.
            const float outAir  = bandLevelDb (out, (int) rate, 17500.0f, 20000.0f, rate);
            const float inAir   = bandLevelDb (in,  (int) rate, 17500.0f, 20000.0f, rate);
            const float outSeed = bandLevelDb (out, (int) rate, 8000.0f, 15000.0f, rate);
            logMessage ("  latency " + juce::String (p.getLatencySamples()) + " samples, cutoff "
                        + juce::String (p.getCurrentCutoffHz(), 0) + " Hz, 17.5-20 kHz +" + juce::String (outAir - inAir, 1)
                        + " dB over input, " + juce::String (outAir - outSeed, 1) + " dB vs 8-15 kHz");
            expect (finite, "output must be finite");
            expect (p.isRestoring());
            expectWithinAbsoluteError (p.getCurrentCutoffHz(), 16000.0f, 600.0f);
            expectGreaterThan (outAir - inAir, 20.0f, "air band should be restored");
            expect (outAir - outSeed > -35.0f && outAir - outSeed < -15.0f, "restored air level relative to the seed band");
        }

        beginTest ("CPU cost (informational)");
        {
            auto x = whiteNoise ((int) testRate * 10, 14, 0.25);
            applyBrickwall (x, testRate, 16000.0);
            const auto in = toStereo (x);
            TrueAirAudioProcessor p;
            p.prepareToPlay (testRate, 512);
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            runProcessor (p, in, 512);
            const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
            logMessage ("  10 s stereo @ 44.1 kHz processed in " + juce::String (ms, 1) + " ms ("
                        + juce::String (100.0 * ms / 10000.0, 2) + "% of one core, restoring="
                        + (p.isRestoring() ? "yes" : "no") + ")");
            expect (true);
        }
    }
};

static TrueAirProcessorTests trueAirProcessorTests;
static CutoffDetectorTests cutoffDetectorTests;
static RestorationTests restorationTests;

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.runTestsInCategory ("TrueAir");

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        failures += runner.getResult (i)->failures;

    return failures > 0 ? 1 : 0;
}
