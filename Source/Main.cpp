#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
#include "ui/PlotComponents.h"

class PluginAnalyzerApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        pa::Theme::applyToDefaultLookAndFeel();

        core = std::make_unique<pa::AnalyzerCore> (pa::AnalyzerCore::HostType::App);
        core->hostDescription = "Plugin Analyzer app";
        core->startLiveControlIfEnabled();
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *core);

        // Restore the previous session (plugins + their state)
        auto& props = core->props();
        if (props.getBoolValue ("reloadSession", true))
            if (auto xml = props.getXmlValue ("session"))
                core->restoreSessionXml (*xml);

        // Optional: plugin path(s) on the command line -> load into chain 1 / slot 1
        for (auto& arg : getCommandLineParameterArray())
        {
            const juce::File f (juce::File::getCurrentWorkingDirectory().getChildFile (arg.unquoted()));
            if (! arg.startsWith ("-") && f.exists())
            {
                mainWindow->getMain()->loadPluginFile (f);
                break;
            }
        }
    }

    void shutdown() override
    {
        if (core != nullptr)
            if (auto xml = core->createSessionXml())
                core->props().setValue ("session", xml.get());

        mainWindow = nullptr;   // UI first (closes plugin windows)
        core = nullptr;         // then the engine and the hosted plugins
    }

    void systemRequestedQuit() override { quit(); }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, pa::AnalyzerCore& core)
            : DocumentWindow (name, pa::Theme::background, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new pa::MainComponent (core), true);
            setResizable (true, true);
            setResizeLimits (1000, 640, 4000, 3000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        pa::MainComponent* getMain() { return dynamic_cast<pa::MainComponent*> (getContentComponent()); }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

private:
    std::unique_ptr<pa::AnalyzerCore> core;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (PluginAnalyzerApplication)
