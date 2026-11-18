#include "PluginEditor.h"

TrueAirAudioProcessorEditor::TrueAirAudioProcessorEditor (TrueAirAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setUpKnob (amount,     TrueAirParams::amountId,     "Amount");
    setUpKnob (brightness, TrueAirParams::brightnessId, "Brightness");
    setUpKnob (drive,      TrueAirParams::driveId,      "Drive");
    setUpKnob (cutoff,     TrueAirParams::cutoffId,     "Cutoff");
    setUpKnob (gain,       TrueAirParams::gainId,       "Output Gain");

    autoCutoffAttachment = std::make_unique<ButtonAttachment> (p.getState(), TrueAirParams::autoCutoffId, autoCutoffButton);
    addAndMakeVisible (autoCutoffButton);

    statusLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (statusLabel);

    setSize (560, 260);
    timerCallback();
    startTimerHz (10);
}

void TrueAirAudioProcessorEditor::setUpKnob (Knob& k, const juce::String& paramId, const juce::String& name)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 20);
    addAndMakeVisible (k.slider);

    k.label.setText (name, juce::dontSendNotification);
    k.label.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (k.label);

    k.attachment = std::make_unique<SliderAttachment> (processor.getState(), paramId, k.slider);
}

void TrueAirAudioProcessorEditor::timerCallback()
{
    const bool isAuto = autoCutoffButton.getToggleState();
    cutoff.slider.setEnabled (! isAuto);

    const auto khz = juce::String (processor.getCurrentCutoffHz() / 1000.0f, 1) + " kHz";

    juce::String text;
    if (! isAuto)
        text = "Manual cutoff: restoring above " + khz;
    else if (processor.isRestoring())
        text = "Codec cutoff detected at " + khz + ": restoring";
    else
        text = "No codec cutoff detected: passing through";

    statusLabel.setText (text, juce::dontSendNotification);
}

void TrueAirAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText ("TrueAir", getLocalBounds().removeFromTop (40), juce::Justification::centred);
}

void TrueAirAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    area.removeFromTop (30);

    statusLabel.setBounds (area.removeFromBottom (24));

    const int knobWidth = area.getWidth() / 5;
    for (auto* k : { &amount, &brightness, &drive, &cutoff, &gain })
    {
        auto column = area.removeFromLeft (knobWidth).reduced (4, 0);
        k->label.setBounds (column.removeFromTop (20));

        if (k == &cutoff)
            autoCutoffButton.setBounds (column.removeFromBottom (24).withSizeKeepingCentre (70, 24));

        k->slider.setBounds (column);
    }
}
