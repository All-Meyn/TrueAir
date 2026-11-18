// Headless tests for the TrueAir processor. Run via `ctest`.
#include "PluginProcessor.h"

namespace
{
    void setGainDb (TrueAirAudioProcessor& p, float db)
    {
        auto* param = p.getState().getParameter (TrueAirParams::gainId);
        param->setValueNotifyingHost (param->convertTo0to1 (db));
    }

    float renderSteadyState (TrueAirAudioProcessor& p, float input)
    {
        constexpr int blockSize = 512;
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        // Run past the 50 ms gain ramp, then measure the last sample.
        for (int i = 0; i < 20; ++i)
        {
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                juce::FloatVectorOperations::fill (buffer.getWritePointer (ch), input, blockSize);
            p.processBlock (buffer, midi);
        }
        return buffer.getSample (0, blockSize - 1);
    }
}

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
            juce::MemoryBlock state;
            a.getStateInformation (state);

            TrueAirAudioProcessor b;
            b.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
            const auto* raw = b.getState().getRawParameterValue (TrueAirParams::gainId);
            expectWithinAbsoluteError (raw->load(), -12.0f, 1.0e-3f);
        }
    }
};

static TrueAirProcessorTests trueAirProcessorTests;

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
