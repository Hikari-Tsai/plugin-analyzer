#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "core/AnalyzerCore.h"
#include "MainComponent.h"

namespace pa
{

/** The analyzer as an AAX / VST3 / AU plugin. It hosts its own chains and measures them in
    the background while its window is open. The DAW's audio can be routed through chain 1 or
    chain 2 (separate "playback twin" instances) to listen to the plugins on real material. */
class AnalyzerPluginProcessor : public juce::AudioProcessor
{
public:
    AnalyzerPluginProcessor();
    ~AnalyzerPluginProcessor() override;

    void prepareToPlay (double sampleRate, int maximumBlockSize) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

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

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    AnalyzerCore core { AnalyzerCore::HostType::Plugin };

private:
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalyzerPluginProcessor)
};

class AnalyzerPluginEditor : public juce::AudioProcessorEditor
{
public:
    explicit AnalyzerPluginEditor (AnalyzerPluginProcessor&);
    ~AnalyzerPluginEditor() override;

    void resized() override;

private:
    AnalyzerPluginProcessor& owner;
    MainComponent main;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalyzerPluginEditor)
};

} // namespace pa
