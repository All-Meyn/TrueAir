#pragma once

#include "PluginProcessor.h"

class TrueAirAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit TrueAirAudioProcessorEditor (TrueAirAudioProcessor&);
    ~TrueAirAudioProcessorEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::Slider gainSlider;
    juce::Label gainLabel;
    juce::AudioProcessorValueTreeState::SliderAttachment gainAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrueAirAudioProcessorEditor)
};
