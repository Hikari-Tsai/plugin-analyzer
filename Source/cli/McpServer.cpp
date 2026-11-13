#include "McpServer.h"
#include "api/AnalyzerApi.h"

#include <cstdio>
#include <iostream>
#include <string>

#if JUCE_WINDOWS
 #include <fcntl.h>
 #include <io.h>
#else
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
      }
    ])";

    return parseJson (tools.replace ("MODES", modeEnum()).replace ("SLOT", slotSchema).toRawUTF8());
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

    return fail ("Unknown tool: " + name);
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
                                         "each metric. Read the resource plugin-analyzer://guide for which mode answers which question.");
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
