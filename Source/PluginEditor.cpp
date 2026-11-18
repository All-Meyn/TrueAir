#include "PluginEditor.h"

TrueAirAudioProcessorEditor::TrueAirAudioProcessorEditor (TrueAirAudioProcessor& p)
    : AudioProcessorEditor (&p),
      gainAttachment (p.getState(), TrueAirParams::gainId, gainSlider)
{
    gainSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    gainSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 20);
    addAndMakeVisible (gainSlider);

    gainLabel.setText ("Output Gain", juce::dontSendNotification);
    gainLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (gainLabel);

    setSize (320, 240);
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
    auto area = getLocalBounds().reduced (20);
    area.removeFromTop (30);
    gainLabel.setBounds (area.removeFromTop (20));
    gainSlider.setBounds (area);
}
