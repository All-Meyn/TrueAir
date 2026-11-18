#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "AirRestorer.h"
#include "CutoffDetector.h"

namespace TrueAirParams
{
    // Parameter IDs are saved in host sessions and presets: do not rename.
    inline constexpr auto gainId       = "gain";
    inline constexpr auto amountId     = "amount";
    inline constexpr auto brightnessId = "brightness";
    inline constexpr auto driveId      = "drive";
    inline constexpr auto autoCutoffId = "autoCutoff";
    inline constexpr auto cutoffId     = "cutoff";
}

class TrueAirAudioProcessor final : public juce::AudioProcessor
{
public:
    TrueAirAudioProcessor();
    ~TrueAirAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getState() noexcept { return parameters; }

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Safe to read from any thread (e.g. the editor's timer).
    float getCurrentCutoffHz() const noexcept { return uiCutoffHz.load (std::memory_order_relaxed); }
    bool isRestoring() const noexcept         { return uiRestoring.load (std::memory_order_relaxed); }

private:
    void processChunk (juce::dsp::AudioBlock<float> block) noexcept;

    juce::AudioProcessorValueTreeState parameters;

    // Cached parameter pointers, read on the audio thread.
    std::atomic<float>* gainDb       = nullptr;
    std::atomic<float>* amountPct    = nullptr;
    std::atomic<float>* brightnessDb = nullptr;
    std::atomic<float>* driveDb      = nullptr;
    std::atomic<float>* autoCutoff   = nullptr;
    std::atomic<float>* manualCutoff = nullptr;

    CutoffDetector detector;
    AirRestorer restorer;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dryDelay;
    juce::dsp::Gain<float> outputGain;

    juce::AudioBuffer<float> airBuffer;
    std::vector<float> monoScratch, mixScratch;

    juce::SmoothedValue<float> smoothedCutoff;
    juce::SmoothedValue<float> smoothedAmount;
    juce::SmoothedValue<float> restoreFade; // 1 = restoring, 0 = bypassed (no codec cutoff found)

    int maxChunk = 0;
    bool airWasActive = false;

    std::atomic<float> uiCutoffHz { 16000.0f };
    std::atomic<bool> uiRestoring { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrueAirAudioProcessor)
};
