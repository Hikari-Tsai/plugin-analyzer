#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "core/AnalyzerCore.h"
#include "ui/ChainPanel.h"
#include "ui/AnalysisTabs.h"

namespace pa
{

class MainComponent : public juce::Component,
                      private juce::ChangeListener
{
public:
    explicit MainComponent (AnalyzerCore&);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Loads a plugin file into chain 1, slot 1 (used for command-line launches). */
    void loadPluginFile (const juce::File& file);

private:
    class Tabs : public juce::TabbedComponent
    {
    public:
        Tabs() : TabbedComponent (juce::TabbedButtonBar::TabsAtTop) {}
        std::function<void (int)> onTabChanged;
        void currentTabChanged (int index, const juce::String&) override
        {
            if (onTabChanged)
                onTabChanged (index);
        }
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    AnalysisTab* currentTab() const;
    void showSettings();
    void showPluginList();
    void takeSnapshot();
    void updateToggleButtons();

    AnalyzerCore& core;
    AnalysisEngine& engine;

    ChainPanel chainPanel { core };
    juce::TextButton diffButton, msButton, snapshotButton { "Snapshot" }, settingsButton { "Settings" },
                     pluginsButton { "Plugins..." };
    Segmented playSelect { { "Off", "Play 1", "Play 2" } };   // plugin version only
    bool isPlugin() const { return core.hostType == AnalyzerCore::HostType::Plugin; }
    Tabs tabs;
    std::vector<AnalysisTab*> analysisTabs;
    juce::Label status;

    std::unique_ptr<juce::DocumentWindow> pluginListWindow;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltips { this, 600 };
    juce::uint64 lastSequence = 0;
    juce::uint32 lastResultTime = 0;
};

} // namespace pa
