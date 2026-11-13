#pragma once

#include <juce_core/juce_core.h>

/** Live control of a running Plugin Analyzer (app or plugin instance) by local tools,
    such as the MCP server in plugin-analyzer-cli.

    Discovery: every instance with live control enabled writes
        <instancesFolder>/<instance id>.json
        { "id", "pid", "port", "token", "kind": "app" | "plugin", "host", "track", "version", "started" }
    The file is readable by the current user only and is removed when the instance closes.

    Transport: TCP on 127.0.0.1:<port>, one JSON object per line in each direction.
        request:  { "token": "...", "method": "state", "params": { ... } }
        response: { "result": { ... } }  or  { "error": "..." }
    Methods: ping, state, parameters, measure, set_parameters, load_plugin, remove_plugin,
             set_bypass, set_play, snapshot. */
namespace pa::live
{

inline juce::File instancesFolder()
{
   #if JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Application Support/HMusic/PluginAnalyzer/instances");
   #else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("HMusic/PluginAnalyzer/instances");
   #endif
}

} // namespace pa::live
