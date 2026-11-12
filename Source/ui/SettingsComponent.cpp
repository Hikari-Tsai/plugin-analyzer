#include "SettingsComponent.h"
#include "PlotComponents.h"

namespace pa
{

static const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
static const int blockSizes[] = { 32, 64, 128, 256, 512, 1024, 2048, 4096 };
static const int paces[] = { 0, 30, 200, 1000 };

void SettingsComponent::setReloadSessionVisible (bool shouldBeVisible)
{
    reloadSession.setVisible (shouldBeVisible);
    labels[rows.indexOf (&reloadSession)]->setVisible (shouldBeVisible);
}

SettingsComponent::SettingsComponent (AnalysisEngine& e, juce::PropertiesFile& p) : engine (e), props (p)
{
    const auto s = engine.getSettings();

    for (int i = 0; i < (int) std::size (rates); ++i)
    {
        sampleRate.addItem (juce::String (rates[i] / 1000.0, 1) + " kHz", i + 1);
        if (juce::exactlyEqual (rates[i], s.sampleRate))
            sampleRate.setSelectedId (i + 1, juce::dontSendNotification);
    }

    quality.addItem ("Normal (16384)", 1);
    quality.addItem ("Higher (32768)", 2);
    quality.addItem ("Highest (65536)", 3);
    quality.setSelectedId (s.quality + 1, juce::dontSendNotification);

    for (int i = 0; i < (int) std::size (blockSizes); ++i)
    {
        blockSize.addItem (juce::String (blockSizes[i]) + " samples", i + 1);
        if (blockSizes[i] == s.blockSize)
            blockSize.setSelectedId (i + 1, juce::dontSendNotification);
    }

    pace.addItem ("As fast as possible", 1);
    pace.addItem ("Fast (30 ms pause)", 2);
    pace.addItem ("Relaxed (200 ms pause)", 3);
    pace.addItem ("Slow (1 s pause)", 4);
    for (int i = 0; i < (int) std::size (paces); ++i)
        if (paces[i] == s.pauseMs)
            pace.setSelectedId (i + 1, juce::dontSendNotification);

    reloadSession.setToggleState (props.getBoolValue ("reloadSession", true), juce::dontSendNotification);

    addRow ("Sample rate", sampleRate);
    addRow ("Analysis length (FFT)", quality);
    addRow ("Processing block size", blockSize);
    addRow ("Measurement pace", pace);
    addRow ({}, reloadSession);
    addRow ({}, pluginListButton);

    sampleRate.onChange = [this]
    {
        const double sr = rates[sampleRate.getSelectedId() - 1];
        engine.updateSettings ([sr] (AnalysisSettings& st) { st.sampleRate = sr; });
        save();
        if (onSampleRateChanged)
            onSampleRateChanged();
    };
    quality.onChange = [this]
    {
        const int q = quality.getSelectedId() - 1;
        engine.updateSettings ([q] (AnalysisSettings& st) { st.quality = q; });
        save();
    };
    blockSize.onChange = [this]
    {
        const int bs = blockSizes[blockSize.getSelectedId() - 1];
        engine.updateSettings ([bs] (AnalysisSettings& st) { st.blockSize = bs; });
        save();
    };
    pace.onChange = [this]
    {
        const int ms = paces[pace.getSelectedId() - 1];
        engine.updateSettings ([ms] (AnalysisSettings& st) { st.pauseMs = ms; });
        save();
    };
    reloadSession.onClick = [this] { save(); };
    pluginListButton.onClick = [this] { if (onShowPluginList) onShowPluginList(); };

    setSize (440, 36 * rows.size() + 24);
}

void SettingsComponent::addRow (const juce::String& text, juce::Component& c)
{
    auto* l = labels.add (new juce::Label ({}, text));
    l->setColour (juce::Label::textColourId, Theme::textDim);
    l->setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (l);
    addAndMakeVisible (c);
    rows.add (&c);
}

void SettingsComponent::save()
{
    const auto s = engine.getSettings();
    props.setValue ("sampleRate", s.sampleRate);
    props.setValue ("quality", s.quality);
    props.setValue ("blockSize", s.blockSize);
    props.setValue ("pauseMs", s.pauseMs);
    props.setValue ("reloadSession", reloadSession.getToggleState());
    props.saveIfNeeded();
}

void SettingsComponent::paint (juce::Graphics& g)
{
    g.fillAll (Theme::panel);
}

void SettingsComponent::resized()
{
    auto r = getLocalBounds().reduced (12);
    for (int i = 0; i < rows.size(); ++i)
    {
        auto row = r.removeFromTop (36).reduced (0, 4);
        labels[i]->setBounds (row.removeFromLeft (170));
        row.removeFromLeft (10);
        rows[i]->setBounds (row);
    }
}

} // namespace pa
