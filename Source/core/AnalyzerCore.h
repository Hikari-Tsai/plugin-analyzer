#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>
#include "engine/AnalysisEngine.h"
#include "PlaybackRouter.h"
#include <map>

namespace pa
{

class LiveServer;

/** Everything that must outlive the UI: settings, plugin list, analysis engine and the
    chains' contents. Owned by the standalone app or by the plugin's AudioProcessor, so that
    in a DAW the editor window can be closed and reopened without losing anything. */
class AnalyzerCore : private juce::ChangeListener,
                     private juce::Timer
{
public:
    enum class HostType { App, Plugin };

    explicit AnalyzerCore (HostType);
    ~AnalyzerCore() override;

    const HostType hostType;
    juce::ApplicationProperties appProperties;
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPlugins;
    AnalysisEngine engine;

    /** Plugin version only: routes the DAW's audio through one chain's playback twins. */
    PlaybackRouter playback { engine };

    juce::PropertiesFile& props() { return *appProperties.getUserSettings(); }

    //==============================================================================
    // Live control by local tools (MCP). Off unless the user enables it in Settings.
    /** e.g. "Pro Tools (AAX)"; set by the owner before startLiveControlIfEnabled(). */
    juce::String hostDescription;
    /** Captures the main window; set by the UI while it exists (message thread only). */
    std::function<juce::Image()> snapshotProvider;

    void startLiveControlIfEnabled();
    void setLiveControlEnabled (bool shouldBeEnabled);
    bool isLiveControlEnabled() const;
    int getLiveControlPort() const;
    void setTrackName (const juce::String& name);

    struct Listener
    {
        virtual ~Listener() = default;
        /** Called before a slot's processor is replaced or deleted: close its editor now. */
        virtual void slotAboutToChange (int /*chain*/, int /*slot*/) {}
        virtual void chainsChanged() {}
    };

    void addListener (Listener* l) { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

    // Chain editing (message thread)
    void installProcessor (int chain, int slot, std::unique_ptr<juce::AudioProcessor> processor,
                           const juce::String& name, std::unique_ptr<juce::XmlElement> source,
                           std::unique_ptr<juce::AudioProcessor> playTwin = nullptr);
    bool loadBuiltin (int chain, int slot, const juce::String& id);
    void loadDescription (int chain, int slot, const juce::PluginDescription& desc,
                          std::function<void (juce::AudioProcessor&)> beforeInstall = nullptr,
                          std::function<void()> afterInstall = nullptr);
    bool loadFile (int chain, int slot, const juce::File& file, std::function<void()> afterInstall = nullptr);
    void removeSlot (int chain, int slot);
    void setBypass (int chain, int slot, bool shouldBypass);
    void clearAll();

    /** All slots with their plugin state. */
    std::unique_ptr<juce::XmlElement> createSessionXml();
    void restoreSessionXml (const juce::XmlElement& xml);

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;   // keeps playback twins in sync with what the user edits
    void applyStoredSettings();
    bool wantsTwins() const { return hostType == HostType::Plugin; }

    std::unique_ptr<LiveServer> live;
    juce::String trackName;
    bool shuttingDown = false;   // no callbacks / latency updates while the destructor clears the chains
    std::map<int, std::unique_ptr<ParamMirror>> mirrors;     // key = chain * 16 + slot
    std::map<int, juce::MemoryBlock> lastSyncedState;

    juce::ListenerList<Listener> listeners;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);   // guards async callbacks

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalyzerCore)
};

} // namespace pa
