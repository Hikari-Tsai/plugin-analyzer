// End-to-end check of the built VST3: loads it like a DAW would, configures chain 1 through the
// plugin's own state (gain +6 dB and a 256-sample latency), and verifies what the DAW hears.
// Usage: PluginSmokeTest "/path/to/Plugin Analyzer.vst3"   (exit code 0 = passed)

#include <juce_audio_processors/juce_audio_processors.h>
#include "host/BuiltinProcessors.h"
#include <iostream>

namespace
{
int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (! ok)
        ++failures;
}

juce::String builtinState (const juce::String& id, const juce::String& param, float value)
{
    auto p = pa::createBuiltin (id);
    pa::setBuiltinParameter (*p, param, value);
    juce::MemoryBlock mb;
    p->getStateInformation (mb);
    return mb.toBase64Encoding();
}

void setPluginState (juce::AudioPluginInstance& plugin, int play)
{
    juce::XmlElement root ("PLUGINANALYZER");
    root.setAttribute ("play", play);
    auto* session = root.createNewChildElement ("SESSION");

    auto addSlot = [session] (int slot, const juce::String& id, const juce::String& state)
    {
        auto* e = session->createNewChildElement ("SLOT");
        e->setAttribute ("chain", 0);
        e->setAttribute ("slot", slot);
        e->setAttribute ("name", id);
        e->createNewChildElement ("BUILTIN")->setAttribute ("id", id);
        e->createNewChildElement ("STATE")->addTextElement (state);
    };

    addSlot (0, "gain", builtinState ("gain", "gain", 6.0f));
    addSlot (1, "latency", builtinState ("latency", "delay", 256.0f));

    // The plugin's own state, wrapped the way JUCE's VST3 host expects it (component state).
    juce::MemoryBlock data;
    juce::AudioProcessor::copyXmlToBinary (root, data);
    juce::XmlElement hostState ("VST3PluginState");
    hostState.createNewChildElement ("IComponent")->addTextElement (data.toBase64Encoding());

    juce::MemoryBlock wrapped;
    juce::AudioProcessor::copyXmlToBinary (hostState, wrapped);
    plugin.setStateInformation (wrapped.getData(), (int) wrapped.getSize());
}

/** The plugin's own state XML, unwrapped from the VST3 host state. */
std::unique_ptr<juce::XmlElement> readPluginState (juce::AudioPluginInstance& plugin)
{
    juce::MemoryBlock state;
    plugin.getStateInformation (state);
    auto host = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
    if (host == nullptr || host->getChildByName ("IComponent") == nullptr)
        return nullptr;

    juce::MemoryBlock component;
    component.fromBase64Encoding (host->getChildByName ("IComponent")->getAllSubText());
    return juce::AudioProcessor::getXmlFromBinary (component.getData(), (int) component.getSize());
}

std::pair<int, float> processImpulse (juce::AudioPluginInstance& plugin, int length = 8192)
{
    juce::AudioBuffer<float> io (juce::jmax (2, plugin.getTotalNumOutputChannels()), length);
    io.clear();
    io.setSample (0, 0, 1.0f);
    io.setSample (1, 0, 1.0f);
    juce::MidiBuffer midi;

    for (int pos = 0; pos < length; pos += 512)
    {
        juce::AudioBuffer<float> block (io.getArrayOfWritePointers(), io.getNumChannels(), pos, juce::jmin (512, length - pos));
        plugin.processBlock (block, midi);
    }

    int peak = 0;
    for (int i = 1; i < length; ++i)
        if (std::abs (io.getSample (0, i)) > std::abs (io.getSample (0, peak)))
            peak = i;
    return { peak, io.getSample (0, peak) };
}

void pumpMessages (int ms)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
}
} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (argc < 2)
    {
        std::cout << "usage: PluginSmokeTest <path to .vst3>" << std::endl;
        return 2;
    }

    const juce::String path = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]).getFullPathName();
    juce::AudioPluginFormatManager formats;
    formats.addFormat (new juce::VST3PluginFormat());   // the manager takes ownership

    juce::OwnedArray<juce::PluginDescription> types;
    formats.getFormats()[0]->findAllTypesForFile (types, path);
    check (! types.isEmpty(), "VST3 found: " + path);
    if (types.isEmpty())
        return 1;

    std::cout << "  name: " << types[0]->name << "   manufacturer: " << types[0]->manufacturerName
              << "   version: " << types[0]->version << std::endl;
    check (types[0]->manufacturerName == "HMusic", "manufacturer is HMusic");

    for (int play : { 0, -1 })
    {
        juce::String error;
        auto plugin = formats.createPluginInstance (*types[0], 48000.0, 512, error);
        check (plugin != nullptr, "instantiated" + (error.isEmpty() ? juce::String() : " (" + error + ")"));
        if (plugin == nullptr)
            return 1;

        plugin->enableAllBuses();
        plugin->setRateAndBufferSizeDetails (48000.0, 512);
        plugin->prepareToPlay (48000.0, 512);

        setPluginState (*plugin, play);
        pumpMessages (500);             // state is applied on the message thread

        {
            auto xml = readPluginState (*plugin);
            int slots = 0;
            if (xml != nullptr)
                if (auto* session = xml->getChildByName ("SESSION"))
                    slots = session->getNumChildElements();
            check (xml != nullptr && slots == 2 && xml->getIntAttribute ("play", -2) == play,
                   "state applied (" + juce::String (slots) + " slots, play=" + (xml ? xml->getStringAttribute ("play") : juce::String ("?")) + ")");
        }
        processImpulse (*plugin);       // the latency processor publishes its latency while processing
        pumpMessages (1200);            // twin sync / latency update timer

        const juce::String name = play == 0 ? "Play 1" : "Off";
        check (plugin->getLatencySamples() == 256, name + ": reports 256 samples latency to the DAW (got "
                                                   + juce::String (plugin->getLatencySamples()) + ")");

        const auto [index, value] = processImpulse (*plugin);
        const float expected = play == 0 ? juce::Decibels::decibelsToGain (6.0f) : 1.0f;
        check (index == 256, name + ": output at 256 samples (got " + juce::String (index) + ")");
        check (std::abs (value - expected) < 1.0e-3f, name + ": gain " + juce::String (value, 4)
                                                       + " (expected " + juce::String (expected, 4) + ")");

        plugin->releaseResources();
        plugin.reset();
        pumpMessages (200);
    }

    std::cout << (failures == 0 ? "\nPlugin smoke test passed." : "\nPlugin smoke test FAILED.") << std::endl;
    return failures == 0 ? 0 : 1;
}
