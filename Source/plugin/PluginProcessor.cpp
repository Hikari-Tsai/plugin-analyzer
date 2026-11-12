#include "PluginProcessor.h"
#include "ui/PlotComponents.h"

namespace pa
{

AnalyzerPluginProcessor::AnalyzerPluginProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    // Measure only while the window is open.
    core.engine.setPaused (true);

    // Report the latency of the slower chain; the router aligns the other paths to it.
    core.playback.onLatencyChanged = [this] (int samples) { setLatencySamples (samples); };
}

void AnalyzerPluginProcessor::prepareToPlay (double sampleRate, int maximumBlockSize)
{
    core.playback.prepare (sampleRate, maximumBlockSize);
    core.playback.updateLatency();
    setLatencySamples (core.playback.getReportedLatency());
}

AnalyzerPluginProcessor::~AnalyzerPluginProcessor()
{
    *alive = false;
    // The core is destroyed after this body runs and clears its chains on the way out. It must not
    // call back into this half-destroyed processor (setLatencySamples -> host wrapper -> crash in AAX).
    core.playback.onLatencyChanged = nullptr;
}

bool AnalyzerPluginProcessor::isBusesLayoutSupported (const BusesLayout& layout) const
{
    const auto& out = layout.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
           && layout.getMainInputChannelSet() == out;
}

void AnalyzerPluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // Off: dry (latency-aligned). Play 1 / Play 2: through that chain's playback twins.
    // The analysis itself runs on its own thread with its own instances and test signals.
    core.playback.process (buffer);
}

juce::AudioProcessorEditor* AnalyzerPluginProcessor::createEditor()
{
    return new AnalyzerPluginEditor (*this);
}

void AnalyzerPluginProcessor::getStateInformation (juce::MemoryBlock& data)
{
    juce::XmlElement root ("PLUGINANALYZER");
    root.setAttribute ("version", JucePlugin_VersionString);

    const auto s = core.engine.getSettings();
    root.setAttribute ("difference", s.difference);
    root.setAttribute ("midSide", s.midSide);
    root.setAttribute ("play", core.playback.getPlayChain());

    if (auto session = core.createSessionXml())
        root.addChildElement (session.release());

    copyXmlToBinary (root, data);
}

void AnalyzerPluginProcessor::setStateInformation (const void* data, int size)
{
    std::shared_ptr<juce::XmlElement> xml (getXmlFromBinary (data, size));
    if (xml == nullptr || ! xml->hasTagName ("PLUGINANALYZER"))
        return;

    // Hosts may call this from any thread; loading plugins must happen on the message thread.
    std::weak_ptr<bool> weak = alive;
    juce::MessageManager::callAsync ([this, weak, xml]
    {
        if (weak.expired())
            return;

        core.playback.setPlayChain (xml->getIntAttribute ("play", -1));
        const bool diff = xml->getBoolAttribute ("difference");
        const bool ms = xml->getBoolAttribute ("midSide");
        core.engine.updateSettings ([diff, ms] (AnalysisSettings& s) { s.difference = diff; s.midSide = ms; });

        core.clearAll();
        if (auto* session = xml->getChildByName ("SESSION"))
            core.restoreSessionXml (*session);
    });
}

//==============================================================================
AnalyzerPluginEditor::AnalyzerPluginEditor (AnalyzerPluginProcessor& p)
    : AudioProcessorEditor (p), owner (p), main (p.core)
{
    Theme::applyToDefaultLookAndFeel();
    addAndMakeVisible (main);
    setResizable (true, true);
    setResizeLimits (1000, 640, 4000, 3000);
    setSize (1240, 820);
    owner.core.engine.setPaused (false);
}

AnalyzerPluginEditor::~AnalyzerPluginEditor()
{
    owner.core.engine.setPaused (true);
}

void AnalyzerPluginEditor::resized()
{
    main.setBounds (getLocalBounds());
}

} // namespace pa

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new pa::AnalyzerPluginProcessor();
}
