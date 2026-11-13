#pragma once

#include "engine/AnalysisEngine.h"
#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace pa
{

class SettingsComponent : public juce::Component
{
public:
    SettingsComponent (AnalysisEngine&, juce::PropertiesFile&);

    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void()> onSampleRateChanged;
    std::function<void()> onShowPluginList;
    std::function<void (bool)> onLiveControlChanged;

    /** Shows whether local tools (MCP) may control this window, and on which port. */
    void setLiveControlState (bool enabled, int port);

    /** "Reload last session" only applies to the standalone app (a DAW restores its own state). */
    void setReloadSessionVisible (bool shouldBeVisible);

private:
    void addRow (const juce::String& label, juce::Component& c);
    void save();

    AnalysisEngine& engine;
    juce::PropertiesFile& props;

    juce::ComboBox sampleRate, quality, blockSize, pace;
    juce::ToggleButton reloadSession { "Reload last session on start" };
    juce::ToggleButton liveControl { "Allow LLM control (MCP) of this window" };
    juce::TextButton pluginListButton { "Plugin list / scan..." };
    juce::OwnedArray<juce::Label> labels;
    juce::Array<juce::Component*> rows;
};

} // namespace pa
