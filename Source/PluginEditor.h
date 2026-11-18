#pragma once

#include "PluginProcessor.h"

class TrueAirAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                          private juce::Timer
{
public:
    explicit TrueAirAudioProcessorEditor (TrueAirAudioProcessor&);
    ~TrueAirAudioProcessorEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<SliderAttachment> attachment;
    };

    void timerCallback() override;
    void setUpKnob (Knob&, const juce::String& paramId, const juce::String& name);

    TrueAirAudioProcessor& processor;

    Knob amount, brightness, drive, cutoff, gain;
    juce::ToggleButton autoCutoffButton { "Auto" };
    std::unique_ptr<ButtonAttachment> autoCutoffAttachment;
    juce::Label statusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrueAirAudioProcessorEditor)
};
