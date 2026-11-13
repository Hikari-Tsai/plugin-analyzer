#include "MainComponent.h"
#include "ui/SettingsComponent.h"

namespace pa
{

using juce::String;

namespace
{
class PluginListWindow : public juce::DocumentWindow
{
public:
    PluginListWindow (juce::AudioPluginFormatManager& fm, juce::KnownPluginList& list,
                      juce::PropertiesFile* props, std::function<void()> onCloseFn)
        : DocumentWindow ("Plugins", Theme::panel, DocumentWindow::closeButton), onClose (std::move (onCloseFn))
    {
        const auto deadMansPedal = props != nullptr ? props->getFile().getSiblingFile ("RecentlyCrashedPluginsList")
                                                    : juce::File();
        auto* content = new juce::PluginListComponent (fm, list, deadMansPedal, props, true);
        content->setSize (720, 520);
        setUsingNativeTitleBar (true);
        setContentOwned (content, true);
        setResizable (true, false);
        centreWithSize (720, 520);
        setVisible (true);
    }

    void closeButtonPressed() override { juce::MessageManager::callAsync ([fn = onClose] { fn(); }); }

private:
    std::function<void()> onClose;
};
} // namespace

//==============================================================================
MainComponent::MainComponent (AnalyzerCore& c) : core (c), engine (c.engine)
{
    auto* props = &core.props();

    // Top bar
    addAndMakeVisible (chainPanel);
    for (auto* b : { &diffButton, &msButton, &snapshotButton, &settingsButton, &pluginsButton })
    {
        addAndMakeVisible (b);
        b->setColour (juce::TextButton::buttonColourId, Theme::plotBg);
        b->setColour (juce::TextButton::textColourOffId, Theme::text);
    }

    diffButton.setTooltip ("1|2: show both chains.  1-2: show chain 1 minus chain 2 (null test).");
    msButton.setTooltip ("Analyse Left/Right or Mid/Side");
    snapshotButton.setTooltip ("Save a PNG of the window");

    diffButton.onClick = [this]
    {
        engine.updateSettings ([] (AnalysisSettings& s) { s.difference = ! s.difference; });
        updateToggleButtons();
    };
    msButton.onClick = [this]
    {
        engine.updateSettings ([] (AnalysisSettings& s) { s.midSide = ! s.midSide; });
        updateToggleButtons();
    };
    snapshotButton.onClick = [this] { takeSnapshot(); };

    if (isPlugin())
    {
        addAndMakeVisible (playSelect);
        playSelect.setSelected (core.playback.getPlayChain() + 1, false);
        playSelect.setTooltip ("What you hear from the DAW track: Off = dry, Play 1 / Play 2 = through that chain. "
                               "All three are latency-aligned, so you can A/B while the track plays.");
        playSelect.onChange = [this] (int i) { core.playback.setPlayChain (i - 1); };
    }
    settingsButton.onClick = [this] { showSettings(); };
    pluginsButton.onClick = [this] { showPluginList(); };
    updateToggleButtons();

    // Tabs
    analysisTabs = { new LinearTab (engine), new HarmonicTab (engine), new OscilloscopeTab (engine),
                     new DynamicsTab (engine), new HammersteinTab (engine), new ReverbTab (engine),
                     new PerformanceTab (engine), new SpecsTab (engine) };
    const char* names[] = { "Linear", "Harmonic", "Oscilloscope", "Dynamics", "Hammerstein", "Reverb", "Performance", "Specs" };

    tabs.setTabBarDepth (32);
    tabs.setOutline (0);
    tabs.setColour (juce::TabbedComponent::backgroundColourId, Theme::panel);
    for (size_t i = 0; i < analysisTabs.size(); ++i)
        tabs.addTab (names[i], Theme::panel, analysisTabs[i], true);

    tabs.onTabChanged = [this] (int index)
    {
        if (index < 0 || index >= (int) analysisTabs.size())
            return;
        const auto mode = analysisTabs[(size_t) index]->getMode();
        engine.updateSettings ([mode] (AnalysisSettings& s) { s.mode = mode; });
        core.props().setValue ("tab", index);
    };
    addAndMakeVisible (tabs);

    status.setColour (juce::Label::textColourId, Theme::textDim);
    status.setFont (juce::FontOptions (11.5f));
    status.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (status);

    engine.addChangeListener (this);

    tabs.setCurrentTabIndex (juce::jlimit (0, (int) analysisTabs.size() - 1, props->getIntValue ("tab", 0)));
    const auto mode = analysisTabs[(size_t) tabs.getCurrentTabIndex()]->getMode();
    engine.updateSettings ([mode] (AnalysisSettings& s) { s.mode = mode; });

    setSize (1320, 860);

    // Lets local tools (MCP) see what the user sees
    juce::Component::SafePointer<MainComponent> self (this);
    core.snapshotProvider = [self]
    {
        return self != nullptr ? self->createComponentSnapshot (self->getLocalBounds(), true, 1.0f) : juce::Image();
    };
}

MainComponent::~MainComponent()
{
    core.snapshotProvider = nullptr;
    engine.removeChangeListener (this);
    pluginListWindow.reset();
    chainPanel.closeAllEditors();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (88);
    auto buttons = top.removeFromRight (isPlugin() ? 400 : 250).reduced (6, 8);
    auto row1 = buttons.removeFromTop (buttons.getHeight() / 2).reduced (0, 2);
    auto row2 = buttons.reduced (0, 2);

    if (isPlugin())
        playSelect.setBounds (row1.removeFromLeft (playSelect.getIdealWidth()).reduced (2, 0));

    const int w1 = row1.getWidth() / 3;
    diffButton.setBounds (row1.removeFromLeft (w1).reduced (2, 0));
    msButton.setBounds (row1.removeFromLeft (w1).reduced (2, 0));
    snapshotButton.setBounds (row1.reduced (2, 0));
    const int w2 = row2.getWidth() / 2;
    pluginsButton.setBounds (row2.removeFromLeft (w2).reduced (2, 0));
    settingsButton.setBounds (row2.reduced (2, 0));

    chainPanel.setBounds (top);

    status.setBounds (r.removeFromBottom (20).reduced (8, 0));
    tabs.setBounds (r);
}

void MainComponent::updateToggleButtons()
{
    const auto s = engine.getSettings();
    diffButton.setButtonText (s.difference ? "1-2" : "1|2");
    diffButton.setColour (juce::TextButton::buttonColourId, s.difference ? Theme::curveColour (2, 0).withAlpha (0.5f) : Theme::plotBg);
    msButton.setButtonText (s.midSide ? "M/S" : "L/R");
    msButton.setColour (juce::TextButton::buttonColourId, s.midSide ? Theme::accent.withAlpha (0.4f) : Theme::plotBg);
}

AnalysisTab* MainComponent::currentTab() const
{
    const int index = tabs.getCurrentTabIndex();
    return index >= 0 && index < (int) analysisTabs.size() ? analysisTabs[(size_t) index] : nullptr;
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    auto result = engine.getLatestResult();
    if (result == nullptr || result->sequence == lastSequence)
        return;

    lastSequence = result->sequence;

    if (auto* tab = currentTab(); tab != nullptr && tab->getMode() == result->mode)
        tab->showResult (*result);

    const auto now = juce::Time::getMillisecondCounter();
    const auto s = engine.getSettings();
    String text;
    text << String (s.sampleRate / 1000.0, 1) << " kHz  |  FFT " << s.fftSize() << "  |  block " << s.blockSize;
    if (isPlugin())
    {
        text << "  |  DAW latency " << core.playback.getReportedLatency() << " smp";
        if (playSelect.getSelected() != core.playback.getPlayChain() + 1)   // may be changed remotely (MCP)
            playSelect.setSelected (core.playback.getPlayChain() + 1, false);
    }
    if (core.isLiveControlEnabled())
        text << "  |  MCP on";
    if (lastResultTime != 0)
        text << "  |  update " << (int) (now - lastResultTime) << " ms";
    status.setText (text, juce::dontSendNotification);
    lastResultTime = now;
}

void MainComponent::showSettings()
{
    auto* content = new SettingsComponent (engine, core.props());
    content->onSampleRateChanged = [this]
    {
        const double sr = engine.getSettings().sampleRate;
        for (auto* t : analysisTabs)
            t->sampleRateChanged (sr);
    };
    content->onShowPluginList = [this] { showPluginList(); };
    content->setLiveControlState (core.isLiveControlEnabled(), core.getLiveControlPort());
    juce::Component::SafePointer<SettingsComponent> settingsView (content);
    content->onLiveControlChanged = [this, settingsView] (bool enabled)
    {
        core.setLiveControlEnabled (enabled);
        if (settingsView != nullptr)
            settingsView->setLiveControlState (core.isLiveControlEnabled(), core.getLiveControlPort());
    };
    content->setReloadSessionVisible (core.hostType == AnalyzerCore::HostType::App);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (content);
    o.dialogTitle = "Settings";
    o.dialogBackgroundColour = Theme::panel;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = this;
    if (auto* dialog = o.launchAsync())
        dialog->setAlwaysOnTop (isPlugin());   // stay above the DAW's floating plugin windows
}

void MainComponent::showPluginList()
{
    if (pluginListWindow != nullptr)
    {
        pluginListWindow->toFront (true);
        return;
    }

    juce::Component::SafePointer<MainComponent> safe (this);
    pluginListWindow = std::make_unique<PluginListWindow> (core.formatManager, core.knownPlugins, &core.props(),
                                                           [safe] { if (safe != nullptr) safe->pluginListWindow.reset(); });
    pluginListWindow->setAlwaysOnTop (isPlugin());
}

void MainComponent::takeSnapshot()
{
    const auto image = createComponentSnapshot (getLocalBounds(), true, 2.0f);
    const auto name = "PluginAnalyzer " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S") + ".png";
    const auto start = juce::File::getSpecialLocation (juce::File::userDesktopDirectory).getChildFile (name);

    chooser = std::make_unique<juce::FileChooser> ("Save snapshot", start, "*.png");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
        [image] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File())
                return;

            file = file.withFileExtension ("png");
            file.deleteFile();
            juce::FileOutputStream out (file);
            if (out.openedOk())
                juce::PNGImageFormat().writeImageToStream (image, out);
        });
}

void MainComponent::loadPluginFile (const juce::File& file)
{
    if (! chainPanel.loadFile (0, 0, file))   // opens the plugin window once loaded
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Not a plugin",
                                                "No VST3/AU plugin was found in:\n" + file.getFullPathName());
}

} // namespace pa
