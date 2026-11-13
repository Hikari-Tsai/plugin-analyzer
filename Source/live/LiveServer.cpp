#include "LiveServer.h"
#include "core/AnalyzerCore.h"
#include "api/AnalyzerApi.h"

#if JUCE_WINDOWS
 #include <process.h>
#else
 #include <sys/stat.h>
 #include <unistd.h>
#endif

#ifndef PLUGIN_ANALYZER_VERSION_STRING
 #define PLUGIN_ANALYZER_VERSION_STRING "dev"
#endif

namespace pa
{

using juce::String;
using juce::var;

namespace
{
var object() { return var (new juce::DynamicObject()); }

void put (var& o, const juce::Identifier& key, const var& value) { o.getDynamicObject()->setProperty (key, value); }

var fail (const String& message)
{
    auto o = object();
    put (o, "error", message);
    return o;
}

int processId()
{
   #if JUCE_WINDOWS
    return (int) _getpid();
   #else
    return (int) getpid();
   #endif
}

String sourceSpec (const PluginChain::Slot& slot)
{
    if (slot.source == nullptr) return {};
    if (slot.source->hasTagName ("BUILTIN")) return "builtin:" + slot.source->getStringAttribute ("id");
    return slot.source->getStringAttribute ("file");
}

/** chain (1-2) and slot (1-6) from params, as 0-based indices. */
bool slotIndex (const var& params, int& chain, int& slot, String& error)
{
    chain = (int) params["chain"] - 1;
    slot = (int) params["slot"] - 1;
    if (chain < 0 || chain > 1 || slot < 0 || slot >= PluginChain::numSlots)
    {
        error = "\"chain\" must be 1 or 2 and \"slot\" 1-" + String (PluginChain::numSlots);
        return false;
    }
    return true;
}

String playName (int play) { return play < 0 ? "off" : String (play + 1); }
} // namespace

//==============================================================================
LiveServer::LiveServer (AnalyzerCore& c) : juce::Thread ("Live control"), core (c) {}

LiveServer::~LiveServer()
{
    *alive = false;
    signalThreadShouldExit();
    if (listener != nullptr)
        listener->close();
    stopThread (4000);

    const juce::ScopedLock sl (fileLock);
    discoveryFile.deleteFile();
}

bool LiveServer::start()
{
    listener = std::make_unique<juce::StreamingSocket>();
    if (! listener->createListener (0, "127.0.0.1"))   // any free port, local connections only
        return false;

    port = listener->getBoundPort();
    token = juce::Uuid().toString() + juce::Uuid().toString();
    instanceId = String (core.hostType == AnalyzerCore::HostType::Plugin ? "plugin-" : "app-")
               + juce::Uuid().toString().substring (0, 8);

    writeDiscoveryFile();
    startThread();
    return true;
}

void LiveServer::setTrackName (const String& name)
{
    {
        const juce::ScopedLock sl (fileLock);
        if (trackName == name) return;
        trackName = name;
    }
    writeDiscoveryFile();
}

void LiveServer::writeDiscoveryFile()
{
    const juce::ScopedLock sl (fileLock);
    if (instanceId.isEmpty()) return;

    auto info = object();
    put (info, "id", instanceId);
    put (info, "pid", processId());
    put (info, "port", port);
    put (info, "token", token);
    put (info, "kind", core.hostType == AnalyzerCore::HostType::Plugin ? "plugin" : "app");
    put (info, "host", core.hostDescription);
    if (trackName.isNotEmpty()) put (info, "track", trackName);
    put (info, "version", PLUGIN_ANALYZER_VERSION_STRING);
    put (info, "started", juce::Time::getCurrentTime().toISO8601 (true));

    const auto folder = live::instancesFolder();
    folder.createDirectory();
    discoveryFile = folder.getChildFile (instanceId + ".json");
    discoveryFile.replaceWithText (juce::JSON::toString (info, false));
   #if ! JUCE_WINDOWS
    ::chmod (discoveryFile.getFullPathName().toRawUTF8(), 0600);   // the token is for this user only
   #endif
}

//==============================================================================
void LiveServer::run()
{
    while (! threadShouldExit())
    {
        if (listener->waitUntilReady (true, 250) != 1)
            continue;

        std::unique_ptr<juce::StreamingSocket> client (listener->waitForNextConnection());
        if (client != nullptr && ! threadShouldExit())
            serve (*client);
    }
}

void LiveServer::serve (juce::StreamingSocket& client)
{
    std::string pending;
    char buffer[4096];
    int idleMs = 0;

    while (! threadShouldExit() && client.isConnected())
    {
        const auto newline = pending.find ('\n');
        if (newline != std::string::npos)
        {
            const auto line = String::fromUTF8 (pending.substr (0, newline).c_str()).trim();
            pending.erase (0, newline + 1);
            idleMs = 0;
            if (line.isEmpty()) continue;

            var request, reply;
            if (juce::JSON::parse (line, request).failed() || ! request.isObject())
                reply = fail ("Invalid JSON");
            else if (request["token"].toString() != token)
                reply = fail ("Invalid token");
            else
            {
                const auto result = handle (request["method"].toString(), request["params"]);
                if (result.hasProperty ("error"))
                    reply = result;
                else
                {
                    reply = object();
                    put (reply, "result", result);
                }
            }

            const auto text = juce::JSON::toString (reply, true, 6) + "\n";
            const auto* data = text.toRawUTF8();
            const int size = (int) std::strlen (data);
            for (int sent = 0; sent < size;)
            {
                const int n = client.write (data + sent, size - sent);
                if (n <= 0) return;
                sent += n;
            }
            continue;
        }

        if (pending.size() > (1u << 20))   // no request is this large
            return;

        const int ready = client.waitUntilReady (true, 100);
        if (ready < 0) return;
        if (ready == 0)
        {
            if ((idleMs += 100) > 30000) return;   // idle connection
            continue;
        }

        const int n = client.read (buffer, sizeof (buffer), false);
        if (n <= 0) return;
        pending.append (buffer, (size_t) n);
    }
}

var LiveServer::onMessageThread (std::function<var()> fn, int timeoutMs)
{
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        return fn();

    struct Call { var result; juce::WaitableEvent done; };
    auto call = std::make_shared<Call>();
    std::weak_ptr<bool> weak = alive;

    juce::MessageManager::callAsync ([call, fn, weak]
    {
        if (! weak.expired())
            call->result = fn();
        call->done.signal();
    });

    for (int waited = 0; waited < timeoutMs; waited += 50)
    {
        if (call->done.wait (50))
            return call->result;
        if (threadShouldExit())
            break;
    }
    return fail ("Timed out waiting for the app (is a modal dialog open?)");
}

//==============================================================================
var LiveServer::handle (const String& method, const var& params)
{
    if (method == "ping")
    {
        auto o = object();
        put (o, "id", instanceId);
        return o;
    }
    if (method == "state")          return onMessageThread ([this] { return state(); });
    if (method == "parameters")     return onMessageThread ([this, params] { return slotParameters (params); });
    if (method == "set_parameters") return onMessageThread ([this, params] { return setParameters (params); });
    if (method == "remove_plugin")  return onMessageThread ([this, params] { return removePlugin (params); });
    if (method == "set_bypass")     return onMessageThread ([this, params] { return setBypass (params); });
    if (method == "set_play")       return onMessageThread ([this, params] { return setPlay (params); });
    if (method == "snapshot")       return onMessageThread ([this] { return snapshot(); });
    if (method == "measure")        return measure (params);       // runs on the analysis thread
    if (method == "load_plugin")    return loadPlugin (params);    // waits for an asynchronous load

    return fail ("Unknown method: " + method);
}

var LiveServer::state()
{
    auto o = object();
    put (o, "id", instanceId);
    put (o, "kind", core.hostType == AnalyzerCore::HostType::Plugin ? "plugin" : "app");
    put (o, "host", core.hostDescription);
    if (trackName.isNotEmpty()) put (o, "track", trackName);

    const auto s = core.engine.getSettings();
    put (o, "view_mode", api::modeName (s));
    put (o, "sample_rate", s.sampleRate);
    put (o, "channels", s.midSide ? "mid/side" : "left/right");
    put (o, "difference", s.difference);

    juce::Array<var> chains;
    for (int c = 0; c < 2; ++c)
    {
        juce::Array<var> slots;
        for (int i = 0; i < PluginChain::numSlots; ++i)
        {
            auto& slot = core.engine.getChain (c).slot (i);
            if (slot.processor == nullptr) continue;

            auto so = object();
            put (so, "slot", i + 1);
            put (so, "name", slot.name);
            put (so, "plugin", sourceSpec (slot));
            put (so, "bypassed", slot.bypassed.load());
            put (so, "latency_samples", slot.processor->getLatencySamples());
            put (so, "parameter_count", slot.processor->getParameters().size());
            slots.add (so);
        }
        auto co = object();
        put (co, "chain", c + 1);
        put (co, "slots", var (slots));
        chains.add (co);
    }
    put (o, "chains", var (chains));

    if (core.hostType == AnalyzerCore::HostType::Plugin)
    {
        put (o, "play", playName (core.playback.getPlayChain()));
        put (o, "daw_latency_samples", core.playback.getReportedLatency());
    }

    // What the user is looking at right now (the continuous measurement)
    if (auto latest = core.engine.getLatestResult())
    {
        auto current = api::resultToJson (*latest, 0);
        put (current, "mode", api::modeName (s));
        put (o, "current_view", current);
    }

    return o;
}

var LiveServer::measure (const var& params)
{
    auto s = core.engine.getSettings();
    String error;

    const auto mode = params["mode"].toString();
    if (mode.isNotEmpty() && ! api::applyMode (s, mode, error)) return fail (error);
    if (! api::applySettings (s, params["settings"], error)) return fail (error);
    if (params.hasProperty ("difference")) s.difference = (bool) params["difference"];

    const auto result = core.engine.measureOnce (s, 180000);
    if (result == nullptr)
        return fail ("The measurement did not complete (timed out, or the chains changed while measuring)");

    auto o = api::resultToJson (*result, juce::jlimit (0, 5000, (int) params.getProperty ("max_points", 0)));
    put (o, "mode", api::modeName (s));
    put (o, "sample_rate", s.sampleRate);
    return o;
}

var LiveServer::slotParameters (const var& params)
{
    int c, i; String error;
    if (! slotIndex (params, c, i, error)) return fail (error);
    auto* p = core.engine.getChain (c).slot (i).processor.get();
    if (p == nullptr) return fail ("Chain " + String (c + 1) + " slot " + String (i + 1) + " is empty");

    auto o = object();
    put (o, "name", core.engine.getChain (c).slot (i).name);
    put (o, "parameters", api::parameters (*p, params["filter"].toString()));
    return o;
}

var LiveServer::setParameters (const var& params)
{
    int c, i; String error;
    if (! slotIndex (params, c, i, error)) return fail (error);
    auto* p = core.engine.getChain (c).slot (i).processor.get();
    if (p == nullptr) return fail ("Chain " + String (c + 1) + " slot " + String (i + 1) + " is empty");

    var applied;   // the playback twin follows through its parameter mirror
    if (! api::setParameters (*p, params["params"], applied, error)) return fail (error);
    auto o = object();
    put (o, "applied", applied);
    return o;
}

var LiveServer::loadPlugin (const var& params)
{
    int c, i; String error;
    if (! slotIndex (params, c, i, error)) return fail (error);
    const auto spec = params["plugin"].toString();
    if (spec.isEmpty()) return fail ("\"plugin\" is required");

    auto loaded = std::make_shared<juce::WaitableEvent>();
    const var started = onMessageThread ([this, c, i, spec, loaded]() -> var
    {
        if (spec.startsWithIgnoreCase ("builtin:"))
        {
            if (! core.loadBuiltin (c, i, spec.fromFirstOccurrenceOf (":", false, false).trim().toLowerCase()))
                return fail ("Unknown built-in: " + spec);
            loaded->signal();
            return object();
        }

        const juce::File file (spec);
        if (! file.exists()) return fail ("Plugin not found: " + spec);
        if (! core.loadFile (c, i, file, [loaded] { loaded->signal(); }))
            return fail ("No VST3 or AU plugin found in " + spec);
        return object();
    });

    if (started.hasProperty ("error")) return started;
    if (! loaded->wait (60000))
        return fail ("The plugin did not finish loading (the app may be showing an error)");

    return onMessageThread ([this, c, i]() -> var
    {
        auto& slot = core.engine.getChain (c).slot (i);
        auto o = object();
        put (o, "chain", c + 1);
        put (o, "slot", i + 1);
        put (o, "name", slot.name);
        put (o, "plugin", sourceSpec (slot));
        if (slot.processor != nullptr)
            put (o, "latency_samples", slot.processor->getLatencySamples());
        return o;
    });
}

var LiveServer::removePlugin (const var& params)
{
    int c, i; String error;
    if (! slotIndex (params, c, i, error)) return fail (error);
    core.removeSlot (c, i);
    return object();
}

var LiveServer::setBypass (const var& params)
{
    int c, i; String error;
    if (! slotIndex (params, c, i, error)) return fail (error);
    if (core.engine.getChain (c).slot (i).processor == nullptr) return fail ("That slot is empty");
    core.setBypass (c, i, (bool) params["bypass"]);
    return object();
}

var LiveServer::setPlay (const var& params)
{
    if (core.hostType != AnalyzerCore::HostType::Plugin)
        return fail ("Play only exists in the plugin version (in a DAW)");

    const auto play = params["play"].toString().trim().toLowerCase();
    const int chain = play == "off" ? -1 : play == "1" ? 0 : play == "2" ? 1 : -2;
    if (chain == -2) return fail ("\"play\" must be \"off\", \"1\" or \"2\"");

    core.playback.setPlayChain (chain);
    auto o = object();
    put (o, "play", playName (chain));
    return o;
}

var LiveServer::snapshot()
{
    if (! core.snapshotProvider)
        return fail ("No Plugin Analyzer window is open (in a DAW, open the plugin window first)");

    const auto image = core.snapshotProvider();
    if (! image.isValid())
        return fail ("Could not capture the window");

    juce::MemoryOutputStream png;
    juce::PNGImageFormat().writeImageToStream (image, png);

    auto o = object();
    put (o, "mime_type", "image/png");
    put (o, "width", image.getWidth());
    put (o, "height", image.getHeight());
    put (o, "data", juce::Base64::toBase64 (png.getData(), png.getDataSize()));
    return o;
}

} // namespace pa
