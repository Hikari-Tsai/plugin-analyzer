#include "AnalyzerCore.h"
#include "host/BuiltinProcessors.h"

namespace pa
{

using juce::String;

AnalyzerCore::AnalyzerCore (HostType type) : hostType (type)
{
    juce::PropertiesFile::Options options;
    options.applicationName = "PluginAnalyzer";
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    options.folderName = "HMusic/PluginAnalyzer";   // ~/Library/Application Support/HMusic/PluginAnalyzer, %APPDATA%/HMusic/PluginAnalyzer
    appProperties.setStorageParameters (options);

    formatManager.addDefaultFormats();

    if (auto xml = props().getXmlValue ("pluginList"))
        knownPlugins.recreateFromXml (*xml);
    knownPlugins.addChangeListener (this);

    applyStoredSettings();
    engine.start();

    if (wantsTwins())
        startTimer (500);
}

AnalyzerCore::~AnalyzerCore()
{
    *alive = false;
    shuttingDown = true;
    playback.onLatencyChanged = nullptr;
    stopTimer();
    knownPlugins.removeChangeListener (this);
    engine.stop();
    clearAll();
    mirrors.clear();
    appProperties.saveIfNeeded();
}

void AnalyzerCore::applyStoredSettings()
{
    auto& p = props();
    engine.updateSettings ([&p] (AnalysisSettings& s)
    {
        s.sampleRate = p.getDoubleValue ("sampleRate", s.sampleRate);
        s.quality = p.getIntValue ("quality", s.quality);
        s.blockSize = p.getIntValue ("blockSize", s.blockSize);
        s.pauseMs = p.getIntValue ("pauseMs", s.pauseMs);
    });
}

void AnalyzerCore::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &knownPlugins)
        if (auto xml = knownPlugins.createXml())
        {
            props().setValue ("pluginList", xml.get());
            appProperties.saveIfNeeded();
        }
}

//==============================================================================
void AnalyzerCore::installProcessor (int chain, int slot, std::unique_ptr<juce::AudioProcessor> processor,
                                     const String& name, std::unique_ptr<juce::XmlElement> source,
                                     std::unique_ptr<juce::AudioProcessor> playTwin)
{
    const int key = chain * 16 + slot;
    if (! shuttingDown)
        listeners.call ([chain, slot] (Listener& l) { l.slotAboutToChange (chain, slot); });
    mirrors.erase (key);                  // stop listening before the old processor goes away
    lastSyncedState.erase (key);

    if (processor != nullptr && playTwin != nullptr)
    {
        // The twin starts as an exact copy of the instance the user edits.
        juce::MemoryBlock state;
        processor->getStateInformation (state);
        if (state.getSize() > 0)
            playTwin->setStateInformation (state.getData(), (int) state.getSize());
        lastSyncedState[key] = state;
    }

    {
        AnalysisEngine::EditScope edit (engine);
        const juce::ScopedLock playLock (playback.getLock());
        auto* twin = playTwin.get();
        engine.getChain (chain).setSlot (slot, std::move (processor), name, std::move (source), std::move (playTwin));
        if (twin != nullptr)
            playback.prepareProcessor (*twin);
    }

    auto& s = engine.getChain (chain).slot (slot);
    if (s.processor != nullptr && s.playProcessor != nullptr)
        mirrors[key] = std::make_unique<ParamMirror> (*s.processor, *s.playProcessor);

    if (shuttingDown)
        return;

    playback.updateLatency();
    listeners.call ([] (Listener& l) { l.chainsChanged(); });
}

void AnalyzerCore::timerCallback()
{
    // Parameter changes are mirrored instantly; this catches state that is not a parameter
    // (modes, loaded files, ...) by comparing the full state twice a second.
    for (int c = 0; c < 2; ++c)
    {
        for (int s = 0; s < PluginChain::numSlots; ++s)
        {
            auto& slot = engine.getChain (c).slot (s);
            if (slot.processor == nullptr || slot.playProcessor == nullptr)
                continue;

            juce::MemoryBlock state;
            slot.processor->getStateInformation (state);
            auto& last = lastSyncedState[c * 16 + s];

            if (state.getSize() == 0 || state == last)
                continue;

            last = state;
            const juce::ScopedLock playLock (playback.getLock());   // audio passes through dry meanwhile
            slot.playProcessor->setStateInformation (state.getData(), (int) state.getSize());
        }
    }

    playback.updateLatency();   // some plugins change latency with their settings
}

bool AnalyzerCore::loadBuiltin (int chain, int slot, const String& id)
{
    for (auto& info : getBuiltinList())
    {
        if (info.id == id)
        {
            auto xml = std::make_unique<juce::XmlElement> ("BUILTIN");
            xml->setAttribute ("id", id);
            installProcessor (chain, slot, createBuiltin (id), info.name, std::move (xml),
                              wantsTwins() ? createBuiltin (id) : nullptr);
            return true;
        }
    }
    return false;
}

void AnalyzerCore::loadDescription (int chain, int slot, const juce::PluginDescription& desc,
                                    std::function<void (juce::AudioProcessor&)> beforeInstall,
                                    std::function<void()> afterInstall)
{
    std::weak_ptr<bool> weak = alive;
    const double sr = engine.getSettings().sampleRate;

    auto finish = [this, chain, slot, desc, beforeInstall, afterInstall] (std::unique_ptr<juce::AudioPluginInstance> instance,
                                                                         std::unique_ptr<juce::AudioPluginInstance> twin)
    {
        if (beforeInstall)
            beforeInstall (*instance);

        installProcessor (chain, slot, std::move (instance), desc.name, desc.createXml(), std::move (twin));

        if (afterInstall)
            afterInstall();
    };

    formatManager.createPluginInstanceAsync (desc, sr, PluginChain::maxBlockSize,
        [this, weak, sr, desc, finish] (std::unique_ptr<juce::AudioPluginInstance> instance, const String& error)
        {
            if (weak.expired())
                return;

            if (instance == nullptr)
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                        "Could not load " + desc.name, error);
                return;
            }

            if (! wantsTwins())
            {
                finish (std::move (instance), nullptr);
                return;
            }

            // Plugin version: a second instance processes the DAW's audio.
            auto held = std::make_shared<std::unique_ptr<juce::AudioPluginInstance>> (std::move (instance));
            formatManager.createPluginInstanceAsync (desc, sr, PluginChain::maxBlockSize,
                [weak, held, finish] (std::unique_ptr<juce::AudioPluginInstance> twin, const String&)
                {
                    if (! weak.expired())
                        finish (std::move (*held), std::move (twin));   // without a twin the slot just isn't heard
                });
        });
}

bool AnalyzerCore::loadFile (int chain, int slot, const juce::File& file, std::function<void()> afterInstall)
{
    const auto path = file.getFullPathName();

    for (auto* format : formatManager.getFormats())
    {
        if (! format->fileMightContainThisPluginType (path))
            continue;

        juce::OwnedArray<juce::PluginDescription> found;
        format->findAllTypesForFile (found, path);

        if (! found.isEmpty())
        {
            knownPlugins.addType (*found[0]);
            loadDescription (chain, slot, *found[0], nullptr, std::move (afterInstall));
            return true;
        }
    }

    return false;
}

void AnalyzerCore::removeSlot (int chain, int slot)
{
    installProcessor (chain, slot, nullptr, {}, nullptr);
}

void AnalyzerCore::setBypass (int chain, int slot, bool shouldBypass)
{
    {
        AnalysisEngine::EditScope edit (engine);
        engine.getChain (chain).slot (slot).bypassed = shouldBypass;   // shared by the playback twin
    }
    playback.updateLatency();
    listeners.call ([] (Listener& l) { l.chainsChanged(); });
}

void AnalyzerCore::clearAll()
{
    for (int c = 0; c < 2; ++c)
        for (int s = 0; s < PluginChain::numSlots; ++s)
            if (engine.getChain (c).slot (s).processor != nullptr)
                removeSlot (c, s);
}

//==============================================================================
std::unique_ptr<juce::XmlElement> AnalyzerCore::createSessionXml()
{
    auto xml = std::make_unique<juce::XmlElement> ("SESSION");

    for (int c = 0; c < 2; ++c)
    {
        for (int s = 0; s < PluginChain::numSlots; ++s)
        {
            auto& slot = engine.getChain (c).slot (s);
            if (slot.processor == nullptr || slot.source == nullptr)
                continue;

            auto* e = xml->createNewChildElement ("SLOT");
            e->setAttribute ("chain", c);
            e->setAttribute ("slot", s);
            e->setAttribute ("name", slot.name);
            e->setAttribute ("bypass", slot.bypassed.load());
            e->addChildElement (new juce::XmlElement (*slot.source));

            juce::MemoryBlock state;
            slot.processor->getStateInformation (state);
            e->createNewChildElement ("STATE")->addTextElement (state.toBase64Encoding());
        }
    }

    return xml;
}

void AnalyzerCore::restoreSessionXml (const juce::XmlElement& xml)
{
    for (auto* e : xml.getChildWithTagNameIterator ("SLOT"))
    {
        const int c = juce::jlimit (0, 1, e->getIntAttribute ("chain"));
        const int s = juce::jlimit (0, PluginChain::numSlots - 1, e->getIntAttribute ("slot"));
        const bool bypass = e->getBoolAttribute ("bypass");

        juce::MemoryBlock state;
        if (auto* st = e->getChildByName ("STATE"))
            state.fromBase64Encoding (st->getAllSubText());

        auto applyState = [state] (juce::AudioProcessor& p)
        {
            if (state.getSize() > 0)
                p.setStateInformation (state.getData(), (int) state.getSize());
        };

        if (auto* builtin = e->getChildByName ("BUILTIN"))
        {
            auto processor = createBuiltin (builtin->getStringAttribute ("id"));
            if (processor == nullptr)
                continue;

            applyState (*processor);
            auto source = std::make_unique<juce::XmlElement> (*builtin);
            installProcessor (c, s, std::move (processor), e->getStringAttribute ("name"), std::move (source),
                              wantsTwins() ? createBuiltin (builtin->getStringAttribute ("id")) : nullptr);
            if (bypass)
                setBypass (c, s, true);
        }
        else if (auto* plug = e->getChildByName ("PLUGIN"))
        {
            juce::PluginDescription desc;
            if (desc.loadFromXml (*plug))
                loadDescription (c, s, desc, applyState, [this, c, s, bypass]
                {
                    if (bypass)
                        setBypass (c, s, true);
                });
        }
    }
}

} // namespace pa
