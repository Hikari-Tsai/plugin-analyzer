#include "McpServer.h"
#include "api/AnalyzerApi.h"
#include "live/LiveProtocol.h"
#include <juce_core/juce_core.h>

#include <cstdio>
#include <iostream>
#include <string>

#if JUCE_WINDOWS
 #include <fcntl.h>
 #include <io.h>
#else
 #include <csignal>
 #include <unistd.h>
#endif

namespace pa::mcp
{

using juce::String;
using juce::var;

namespace
{

const juce::StringArray supportedProtocolVersions { "2025-06-18", "2025-03-26", "2024-11-05" };

var object() { return var (new juce::DynamicObject()); }

void put (var& o, const juce::Identifier& key, const var& value)
{
    o.getDynamicObject()->setProperty (key, value);
}

var parseJson (const char* text)
{
    var v;
    juce::JSON::parse (text, v);
    return v;
}

//==============================================================================
// Protocol output. Hosted plugins sometimes print to stdout, which would corrupt the
// JSON-RPC stream, so the protocol gets a private copy of stdout and fd 1 is pointed
// at stderr for everything else.
//==============================================================================
FILE* protocolOut = nullptr;

void claimStdout()
{
    std::fflush (stdout);
   #if JUCE_WINDOWS
    const int fd = _dup (_fileno (stdout));
    _setmode (fd, _O_BINARY);
    _dup2 (_fileno (stderr), _fileno (stdout));
    protocolOut = _fdopen (fd, "wb");
   #else
    const int fd = dup (STDOUT_FILENO);
    dup2 (STDERR_FILENO, STDOUT_FILENO);
    protocolOut = fdopen (fd, "w");
   #endif
}

void send (const var& message)
{
    const auto text = juce::JSON::toString (message, true, 6) + "\n";   // one message per line
    std::fputs (text.toRawUTF8(), protocolOut);
    std::fflush (protocolOut);
}

void log (const String& text)
{
    std::cerr << "[plugin-analyzer mcp] " << text << std::endl;
}

var response (const var& id, const var& result)
{
    auto o = object();
    put (o, "jsonrpc", "2.0");
    put (o, "id", id);
    put (o, "result", result);
    return o;
}

var errorMessage (const var& id, int code, const String& message)
{
    auto e = object();
    put (e, "code", code);
    put (e, "message", message);
    auto o = object();
    put (o, "jsonrpc", "2.0");
    put (o, "id", id);
    put (o, "error", e);
    return o;
}

//==============================================================================
// Tools
//==============================================================================
String modeEnum()
{
    juce::StringArray names;
    const auto description = api::describe();   // keep it alive while iterating
    for (auto& m : *description["modes"].getArray())
        names.add ("\"" + m["name"].toString() + "\"");
    return names.joinIntoString (", ");
}

var toolDefinitions()
{
    const String slotSchema = R"({
        "type": "object",
        "properties": {
          "plugin": { "type": "string", "description": "builtin:<id> (see describe) or the path from list_plugins" },
          "params": { "type": "object", "description": "Parameter name or id -> value as text, e.g. {\"Drive\": \"50\", \"Style\": \"Warm Tape\"}. Use plugin_info to see names. Use {\"normalized\": 0.25} for a raw 0..1 value.", "additionalProperties": true },
          "bypass": { "type": "boolean" }
        },
        "required": ["plugin"]
      })";

    const String tools = R"([
      {
        "name": "describe",
        "title": "Describe Plugin Analyzer",
        "description": "Lists the measurement modes, every setting (with default, unit and meaning), the built-in test processors, and a glossary explaining each metric name. Call this first.",
        "inputSchema": { "type": "object", "properties": {} },
        "annotations": { "readOnlyHint": true, "openWorldHint": false }
      },
      {
        "name": "list_plugins",
        "title": "List installed plugins",
        "description": "Finds VST3 (and on macOS AU) plugins installed in the standard plugin folders, without loading them. Returns names and the paths to use in measure / plugin_info.",
        "inputSchema": {
          "type": "object",
          "properties": {
            "filter": { "type": "string", "description": "Case-insensitive part of the plugin name, e.g. \"Pro-Q\"" },
            "format": { "type": "string", "enum": ["all", "vst3", "au"], "default": "all" }
          }
        },
        "annotations": { "readOnlyHint": true, "openWorldHint": false }
      },
      {
        "name": "plugin_info",
        "title": "Plugin information",
        "description": "Loads one plugin and returns its name, manufacturer, version, latency, channels and all parameters (name, id, current value). Use it to find parameter names before measuring.",
        "inputSchema": {
          "type": "object",
          "properties": { "plugin": { "type": "string", "description": "builtin:<id> or a path from list_plugins" } },
          "required": ["plugin"]
        },
        "annotations": { "readOnlyHint": true, "openWorldHint": false }
      },
      {
        "name": "measure",
        "title": "Measure plugins",
        "description": "Runs one measurement on one or two chains of plugins (up to 6 each, in series) and returns named metrics such as thd_percent, attack_ms or rt60_t30_s (see describe for their meaning). Put two plugins in chain1 and chain2 to compare them, or set difference=true to measure chain1 minus chain2 (a null test). Curves are omitted unless max_points > 0.",
        "inputSchema": {
          "type": "object",
          "properties": {
            "mode": { "type": "string", "enum": [MODES], "description": "What to measure. 'specs' gives a full datasheet; 'thd' harmonic distortion; 'linear' frequency response; 'attack_release' compressor timing; 'reverb' decay times." },
            "chain1": { "type": "array", "maxItems": 6, "items": SLOT, "description": "Plugins in series. Empty = a wire (useful as a reference)." },
            "chain2": { "type": "array", "maxItems": 6, "items": SLOT, "description": "Optional second chain, measured separately (or subtracted with difference)." },
            "settings": { "type": "object", "additionalProperties": true, "description": "Measurement settings by key, e.g. {\"harmonic_freq_hz\": 100, \"harmonic_level_db\": -12}. See describe for keys, units and ranges." },
            "difference": { "type": "boolean", "default": false },
            "max_points": { "type": "integer", "minimum": 0, "maximum": 2000, "default": 0, "description": "Points per curve to include (0 = metrics only, which keeps responses small)." }
          },
          "required": ["mode"]
        },
        "annotations": { "readOnlyHint": true, "openWorldHint": false }
      },
      {
        "name": "list_instances",
        "title": "List running Plugin Analyzer windows",
        "description": "Lists Plugin Analyzer apps and plugin instances (e.g. inside Pro Tools or Logic) that are running now and allow LLM control (Settings > Allow LLM control). Each has an id, kind (app/plugin), host and, in a DAW, the track name. The instance_* tools act on one of them.",
        "inputSchema": { "type": "object", "properties": {} },
        "annotations": { "readOnlyHint": true, "openWorldHint": false }
      },
      {
        "name": "instance_state",
        "title": "What a running window shows",
        "description": "The user's current setup in a running instance: plugins in each chain (name, plugin path, bypass, latency), the view the user has open and its latest metrics, and in a DAW the Play routing and reported latency.",
        "inputSchema": { "type": "object", "properties": { "instance": INSTANCE } },
        "annotations": { "readOnlyHint": true, "openWorldHint": false }
      },
      {
        "name": "instance_parameters",
        "title": "Parameters of a plugin in a running window",
        "description": "Lists the parameters (name, id, value) of the plugin in one slot of a running instance. Use filter to narrow plugins with many parameters.",
        "inputSchema": { "type": "object", "properties": { "instance": INSTANCE, "chain": CHAIN, "slot": SLOTNUM,
                         "filter": { "type": "string", "description": "Case-insensitive part of the parameter name" } },
                         "required": ["chain", "slot"] },
        "annotations": { "readOnlyHint": true, "openWorldHint": false }
      },
      {
        "name": "instance_measure",
        "title": "Measure the plugins in a running window",
        "description": "Runs one measurement on the chains as the user has set them up (same modes and settings as measure). Does not change what the user's window shows. Works in a DAW even when the plugin window is closed.",
        "inputSchema": { "type": "object", "properties": { "instance": INSTANCE,
                         "mode": { "type": "string", "enum": [MODES] },
                         "settings": { "type": "object", "additionalProperties": true, "description": "See describe" },
                         "difference": { "type": "boolean" },
                         "max_points": { "type": "integer", "minimum": 0, "maximum": 2000, "default": 0 } },
                         "required": ["mode"] },
        "annotations": { "readOnlyHint": true, "openWorldHint": false }
      },
      {
        "name": "instance_set_parameters",
        "title": "Change plugin parameters in a running window",
        "description": "Sets parameters of the plugin in one slot (as text, e.g. {\"Drive\": \"50\"}). The user sees the change, and in a DAW it also changes what Play 1 / Play 2 sounds like. Returns the values the plugin ended up with.",
        "inputSchema": { "type": "object", "properties": { "instance": INSTANCE, "chain": CHAIN, "slot": SLOTNUM,
                         "params": { "type": "object", "additionalProperties": true } },
                         "required": ["chain", "slot", "params"] },
        "annotations": { "readOnlyHint": false, "destructiveHint": false, "idempotentHint": true, "openWorldHint": false }
      },
      {
        "name": "instance_load_plugin",
        "title": "Load a plugin into a running window",
        "description": "Loads a plugin (builtin:<id> or a path from list_plugins) into a slot, replacing what was there.",
        "inputSchema": { "type": "object", "properties": { "instance": INSTANCE, "chain": CHAIN, "slot": SLOTNUM,
                         "plugin": { "type": "string" } }, "required": ["chain", "slot", "plugin"] },
        "annotations": { "readOnlyHint": false, "destructiveHint": true, "openWorldHint": false }
      },
      {
        "name": "instance_remove_plugin",
        "title": "Remove a plugin from a running window",
        "description": "Empties one slot. The plugin's settings in that slot are lost.",
        "inputSchema": { "type": "object", "properties": { "instance": INSTANCE, "chain": CHAIN, "slot": SLOTNUM },
                         "required": ["chain", "slot"] },
        "annotations": { "readOnlyHint": false, "destructiveHint": true, "openWorldHint": false }
      },
      {
        "name": "instance_set_bypass",
        "title": "Bypass a slot in a running window",
        "description": "Bypasses (true) or enables (false) the plugin in one slot.",
        "inputSchema": { "type": "object", "properties": { "instance": INSTANCE, "chain": CHAIN, "slot": SLOTNUM,
                         "bypass": { "type": "boolean" } }, "required": ["chain", "slot", "bypass"] },
        "annotations": { "readOnlyHint": false, "destructiveHint": false, "idempotentHint": true, "openWorldHint": false }
      },
      {
        "name": "instance_set_play",
        "title": "Choose what the DAW track plays",
        "description": "Plugin version in a DAW only: \"off\" = dry, \"1\" or \"2\" = the DAW audio goes through that chain (latency-aligned, for A/B listening).",
        "inputSchema": { "type": "object", "properties": { "instance": INSTANCE,
                         "play": { "type": "string", "enum": ["off", "1", "2"] } }, "required": ["play"] },
        "annotations": { "readOnlyHint": false, "destructiveHint": false, "idempotentHint": true, "openWorldHint": false }
      },
      {
        "name": "instance_snapshot",
        "title": "Screenshot of a running window",
        "description": "Returns a PNG of the Plugin Analyzer window exactly as the user sees it (graphs included). The window must be open.",
        "inputSchema": { "type": "object", "properties": { "instance": INSTANCE } },
        "annotations": { "readOnlyHint": true, "openWorldHint": false }
      }
    ])";

    const String instance = R"({ "type": "string", "description": "Instance id from list_instances. May be omitted when exactly one instance is running." })";
    const String chain = R"({ "type": "integer", "enum": [1, 2] })";
    const String slotNumber = R"({ "type": "integer", "minimum": 1, "maximum": 6 })";

    return parseJson (tools.replace ("MODES", modeEnum()).replace ("SLOTNUM", slotNumber).replace ("SLOT", slotSchema)
                           .replace ("INSTANCE", instance).replace ("CHAIN", chain).toRawUTF8());
}

//==============================================================================
// Which plugin files may be loaded (loading a plugin runs its code)
//==============================================================================
struct Policy
{
    bool allowAnyPath = false;
    juce::Array<juce::File> folders;

    bool allows (const String& spec, String& error) const
    {
        if (spec.startsWithIgnoreCase ("builtin:") || allowAnyPath)
            return true;

        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (spec);
        for (auto& folder : folders)
            if (file.isAChildOf (folder))
                return true;

        juce::StringArray names;
        for (auto& f : folders) names.add (f.getFullPathName());
        error = "Plugin path not allowed: " + file.getFullPathName() + ". Allowed folders: " + names.joinIntoString ("; ")
              + ". Start the server with --allow-dir <folder> or --allow-any-path to permit others.";
        return false;
    }

    bool allowsChains (const var& args, String& error) const
    {
        for (auto* key : { "chain1", "chain2" })
            if (auto* slots = args[key].getArray())
                for (auto& slot : *slots)
                    if (! allows (slot.isString() ? slot.toString() : slot["plugin"].toString(), error))
                        return false;
        return true;
    }
};

//==============================================================================
// Running instances (live control, see live/LiveProtocol.h)
//==============================================================================
var fail (const String& message) { auto o = object(); put (o, "error", message); return o; }

bool processIsRunning (int pid)
{
   #if JUCE_WINDOWS
    juce::ignoreUnused (pid);
    return true;   // unknown: keep the file
   #else
    return pid > 0 && (::kill (pid, 0) == 0 || errno == EPERM);
   #endif
}

var callInstance (const var& info, const String& method, const var& params, int timeoutMs)
{
    juce::StreamingSocket socket;
    if (! socket.connect ("127.0.0.1", (int) info["port"], 2000))
        return fail ("Could not connect to instance " + info["id"].toString() + " (it may have closed)");

    auto request = object();
    put (request, "token", info["token"]);
    put (request, "method", method);
    put (request, "params", params.isVoid() ? object() : params);
    const auto text = juce::JSON::toString (request, true) + "\n";
    if (socket.write (text.toRawUTF8(), (int) std::strlen (text.toRawUTF8())) <= 0)
        return fail ("Could not send the request");

    std::string reply;
    char buffer[65536];
    for (int waited = 0; waited < timeoutMs;)
    {
        const int ready = socket.waitUntilReady (true, 200);
        if (ready < 0) break;
        if (ready == 0) { waited += 200; continue; }
        const int n = socket.read (buffer, sizeof (buffer), false);
        if (n <= 0) break;
        reply.append (buffer, (size_t) n);
        if (reply.find ('\n') != std::string::npos)
        {
            var parsed;
            if (juce::JSON::parse (String::fromUTF8 (reply.c_str()), parsed).failed())
                return fail ("Invalid reply from the instance");
            return parsed.hasProperty ("error") ? parsed : parsed["result"];
        }
    }
    return fail ("No reply from the instance within " + String (timeoutMs / 1000) + " s");
}

/** Live instances (stale discovery files of processes that are gone are removed). */
juce::Array<var> discoverInstances()
{
    juce::Array<var> result;
    for (const auto& entry : juce::RangedDirectoryIterator (live::instancesFolder(), false, "*.json"))
    {
        var info;
        if (juce::JSON::parse (entry.getFile().loadFileAsString(), info).failed() || ! info.isObject())
            continue;

        if (! processIsRunning ((int) info["pid"]))
        {
            entry.getFile().deleteFile();
            continue;
        }

        const auto ping = callInstance (info, "ping", var(), 1500);
        auto o = object();
        for (auto* key : { "id", "kind", "host", "track", "version", "started" })
            if (info.hasProperty (key)) put (o, key, info[key]);
        put (o, "responding", ! ping.hasProperty ("error"));
        put (o, "_info", info);   // internal: removed before returning to the client
        result.add (o);
    }
    return result;
}

var withoutSecrets (const var& instance)
{
    auto o = object();
    for (auto& prop : instance.getDynamicObject()->getProperties())
        if (prop.name.toString() != "_info") put (o, prop.name, prop.value);
    return o;
}

/** Picks the instance named in args, or the only one running. */
var pickInstance (const var& args, String& error)
{
    const auto wanted = args["instance"].toString();
    const auto instances = discoverInstances();

    if (instances.isEmpty())
    {
        error = "No running Plugin Analyzer allows LLM control. In the app or plugin window, open Settings and enable "
                "\"Allow LLM control (MCP)\".";
        return {};
    }

    if (wanted.isEmpty())
    {
        if (instances.size() == 1) return instances[0]["_info"];
        juce::StringArray ids;
        for (auto& i : instances) ids.add (i["id"].toString() + " (" + i["host"].toString()
                                           + (i.hasProperty ("track") ? ", track " + i["track"].toString() : String()) + ")");
        error = "Several instances are running; pass \"instance\": " + ids.joinIntoString ("; ");
        return {};
    }

    for (auto& i : instances)
        if (i["id"].toString() == wanted) return i["_info"];

    error = "No running instance with id " + wanted + " (see list_instances)";
    return {};
}

var callInstanceTool (const String& name, const var& args, const Policy& policy)
{
    String error;
    if (name == "list_instances")
    {
        juce::Array<var> list;
        for (auto& i : discoverInstances()) list.add (withoutSecrets (i));
        auto o = object();
        put (o, "instances", var (list));
        if (list.isEmpty())
            put (o, "hint", "None found. In Plugin Analyzer (app or plugin window) open Settings and enable \"Allow LLM control (MCP)\".");
        return o;
    }

    const auto info = pickInstance (args, error);
    if (! info.isObject()) return fail (error);

    auto params = object();
    for (auto* key : { "chain", "slot", "filter", "mode", "settings", "difference", "max_points", "params", "plugin", "bypass", "play" })
        if (args.hasProperty (key)) put (params, key, args[key]);

    if (name == "instance_state")          return callInstance (info, "state", params, 30000);
    if (name == "instance_parameters")     return callInstance (info, "parameters", params, 30000);
    if (name == "instance_measure")        return callInstance (info, "measure", params, 200000);
    if (name == "instance_set_parameters") return callInstance (info, "set_parameters", params, 30000);
    if (name == "instance_remove_plugin")  return callInstance (info, "remove_plugin", params, 30000);
    if (name == "instance_set_bypass")     return callInstance (info, "set_bypass", params, 30000);
    if (name == "instance_set_play")       return callInstance (info, "set_play", params, 30000);
    if (name == "instance_snapshot")       return callInstance (info, "snapshot", params, 30000);
    if (name == "instance_load_plugin")
    {
        if (! policy.allows (args["plugin"].toString(), error)) return fail (error);
        return callInstance (info, "load_plugin", params, 75000);
    }

    return fail ("Unknown tool: " + name);
}

var toolResult (const var& payload, bool structured)
{
    const bool isError = payload.hasProperty ("error");
    auto text = object();
    put (text, "type", "text");
    put (text, "text", api::toJson (payload, false));

    juce::Array<var> content;
    content.add (text);

    auto r = object();
    put (r, "content", var (content));
    if (structured && ! isError)
        put (r, "structuredContent", payload);
    put (r, "isError", isError);
    return r;
}

var callTool (const String& name, const var& args, const Policy& policy)
{
    auto fail = [] (const String& message) { auto o = object(); put (o, "error", message); return o; };
    String error;

    if (name == "describe")
        return api::describe();

    if (name == "list_plugins")
        return api::listInstalledPlugins (args["filter"].toString(), args["format"].toString());

    if (name == "plugin_info")
    {
        const auto plugin = args["plugin"].toString();
        if (plugin.isEmpty()) return fail ("\"plugin\" is required");
        if (! policy.allows (plugin, error)) return fail (error);
        return api::pluginInfo (plugin);
    }

    if (name == "measure")
    {
        if (! args.isObject()) return fail ("arguments must be an object");
        if (! policy.allowsChains (args, error)) return fail (error);

        auto request = object();
        for (auto& prop : args.getDynamicObject()->getProperties())
            put (request, prop.name, prop.value);
        if (! request.hasProperty ("max_points"))
            put (request, "max_points", 0);   // metrics only unless curves are asked for
        return api::run (request);
    }

    if (name.startsWith ("instance_") || name == "list_instances")
        return callInstanceTool (name, args, policy);

    return fail ("Unknown tool: " + name);
}

/** Screenshots come back as MCP image content. */
var imageResult (const var& payload)
{
    auto image = object();
    put (image, "type", "image");
    put (image, "data", payload["data"]);
    put (image, "mimeType", payload["mime_type"]);

    auto text = object();
    put (text, "type", "text");
    put (text, "text", "Plugin Analyzer window, " + payload["width"].toString() + " x " + payload["height"].toString() + " px");

    juce::Array<var> content;
    content.add (image);
    content.add (text);
    auto r = object();
    put (r, "content", var (content));
    put (r, "isError", false);
    return r;
}

//==============================================================================
// Resources: a short guide the client can attach to the conversation
//==============================================================================
const char* guideText = R"(# Plugin Analyzer - guide for assistants

Plugin Analyzer measures audio plugins by sending known test signals through them.

Typical workflow
1. list_plugins (filter by name) to get a plugin path, or use builtin:<id> test processors.
2. plugin_info to see the plugin's parameters (names and current values).
3. measure with a mode; read the "metrics" array (name, value, unit, chain, channel).
4. To compare two plugins or two settings, put one in chain1 and the other in chain2.

Choosing a mode
- "Is it clean / how much colour?"        -> thd (thd_percent, h2_dbc, h3_dbc), thd_vs_level
- "Does it alias?"                         -> thd at a high harmonic_freq_hz (non_harmonic_db), sweep1d
- "What does the EQ curve look like?"      -> linear (max_points > 0 for the curve)
- "Compressor ratio / threshold?"          -> ramp (max_points > 0, curve ramp_up: x = input dBFS, y = output dBFS)
- "Compressor attack / release?"           -> attack_release (attack_ms, release_ms)
- "How long is the reverb?"                -> reverb (rt60_t30_s, edt_s)
- "Does it report latency correctly?"      -> specs (impulse_peak_offset_samples should be 0)
- "Full datasheet"                          -> specs

Reading numbers
- dB is a ratio: +6 dB ~ x2, -20 dB = x0.1, -60 dB = x0.001. dBFS is relative to digital full scale.
- -200 dB / dBFS means "nothing measurable" (digital silence).
- Analog-modelling plugins are meant to distort; high THD is not automatically bad.
- describe -> "metrics" explains every metric name.

Working with the user's own window (app, or plugin inside a DAW)
- list_instances shows running windows that allow LLM control (the user enables it in Settings).
- instance_state shows their chains and the view they have open; instance_snapshot shows the window itself.
- instance_measure measures their chains without changing their view.
- instance_set_parameters / instance_load_plugin / instance_remove_plugin / instance_set_bypass / instance_set_play
  change the user's setup: tell the user what you are about to change, especially in a DAW session.
)";

var resourceList()
{
    auto r = object();
    put (r, "uri", "plugin-analyzer://guide");
    put (r, "name", "guide");
    put (r, "title", "How to use Plugin Analyzer");
    put (r, "description", "Workflow, which mode answers which question, and how to read the numbers");
    put (r, "mimeType", "text/markdown");
    juce::Array<var> list;
    list.add (r);
    auto o = object();
    put (o, "resources", var (list));
    return o;
}

} // namespace

//==============================================================================
int runServer (const juce::StringArray& options)
{
    Policy policy;
    policy.folders = api::pluginFolders();

    for (int i = 0; i < options.size(); ++i)
    {
        if (options[i] == "--allow-any-path")
            policy.allowAnyPath = true;
        else if (options[i] == "--allow-dir" && i + 1 < options.size())
            policy.folders.add (juce::File::getCurrentWorkingDirectory().getChildFile (options[++i]));
        else
        {
            std::cerr << "Unknown mcp option: " << options[i] << std::endl;
            return 2;
        }
    }

    claimStdout();
    log ("ready (protocol on stdout, logs on stderr)");

    String negotiatedVersion = supportedProtocolVersions[0];
    std::string line;

    while (std::getline (std::cin, line))
    {
        const String text = String::fromUTF8 (line.c_str()).trim();
        if (text.isEmpty())
            continue;

        var message;
        if (juce::JSON::parse (text, message).failed() || ! message.isObject())
        {
            send (errorMessage (var(), -32700, "Parse error"));
            continue;
        }

        const auto method = message["method"].toString();
        const bool isRequest = message.hasProperty ("id");
        const var id = message["id"];
        const var params = message["params"];

        if (! isRequest)
            continue;   // notifications (initialized, cancelled, ...) need no reply

        if (method == "initialize")
        {
            const auto requested = params["protocolVersion"].toString();
            negotiatedVersion = supportedProtocolVersions.contains (requested) ? requested : supportedProtocolVersions[0];

            auto caps = object();
            put (caps, "tools", object());
            put (caps, "resources", object());

            auto info = object();
            put (info, "name", "plugin-analyzer");
            put (info, "title", "Plugin Analyzer");
            put (info, "version", api::describe()["version"]);

            auto result = object();
            put (result, "protocolVersion", negotiatedVersion);
            put (result, "capabilities", caps);
            put (result, "serverInfo", info);
            put (result, "instructions", "Measures audio plugins (VST3/AU) with test signals. Start with describe, use list_plugins to find "
                                         "plugins, plugin_info for parameter names, then measure. Read the metrics array; describe explains "
                                         "each metric. To work with the user's own open window (app or plugin in a DAW) use list_instances "
                                         "and the instance_* tools; say what you change before changing it. Read the resource "
                                         "plugin-analyzer://guide for which mode answers which question.");
            send (response (id, result));
        }
        else if (method == "ping")
        {
            send (response (id, object()));
        }
        else if (method == "tools/list")
        {
            auto result = object();
            put (result, "tools", toolDefinitions());
            send (response (id, result));
        }
        else if (method == "tools/call")
        {
            const auto name = params["name"].toString();
            const auto args = params["arguments"].isObject() ? params["arguments"] : object();
            log ("tools/call " + name);
            const auto start = juce::Time::getMillisecondCounterHiRes();
            const auto payload = callTool (name, args, policy);
            log ("  done in " + String (juce::Time::getMillisecondCounterHiRes() - start, 0) + " ms"
                 + (payload.hasProperty ("error") ? " - error: " + payload["error"].toString() : String()));
            if (name == "instance_snapshot" && ! payload.hasProperty ("error"))
                send (response (id, imageResult (payload)));
            else
                send (response (id, toolResult (payload, negotiatedVersion == "2025-06-18")));
        }
        else if (method == "resources/list")
        {
            send (response (id, resourceList()));
        }
        else if (method == "resources/read")
        {
            if (params["uri"].toString() != "plugin-analyzer://guide")
            {
                send (errorMessage (id, -32002, "Resource not found: " + params["uri"].toString()));
                continue;
            }
            auto c = object();
            put (c, "uri", "plugin-analyzer://guide");
            put (c, "mimeType", "text/markdown");
            put (c, "text", guideText);
            juce::Array<var> contents;
            contents.add (c);
            auto result = object();
            put (result, "contents", var (contents));
            send (response (id, result));
        }
        else
        {
            send (errorMessage (id, -32601, "Method not found: " + method));
        }
    }

    log ("stdin closed, exiting");
    return 0;
}

} // namespace pa::mcp
