#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "analysis/Types.h"

/** Headless, JSON-in / JSON-out access to the measurements.

    Used by plugin-analyzer-cli today and meant to back an MCP server later, so every
    function returns plain data (juce::var) and never throws: failures come back as
    { "error": "..." }. Call from the message thread (plugins may require it).

    Request (all keys optional except "mode"):
    {
      "mode": "thd",                       // see describe() for the list
      "settings": { "harmonic_freq_hz": 1000, "harmonic_level_db": -6 },
      "chain1": [ { "plugin": "builtin:polynomial", "params": { "a3": "0.1" }, "bypass": false } ],
      "chain2": [ { "plugin": "/Library/Audio/Plug-Ins/VST3/Foo.vst3" } ],
      "difference": false,                 // measure chain1 - chain2 instead of both
      "max_points": 200,                   // per curve (0 = no curves)
    }
    A plugin is "builtin:<id>" or a path to a .vst3 / .component file.
    Parameter values are given as text (parsed by the plugin, e.g. "6" or "6 dB"),
    or as { "normalized": 0.25 } for the raw 0..1 value. */
namespace pa::api
{

/** Runs one measurement. */
juce::var run (const juce::var& request);

/** Modes, settings keys (with defaults and units) and built-in processors. */
juce::var describe();

/** Loads one plugin and lists its identity, latency and parameters. */
juce::var pluginInfo (const juce::String& pluginSpec);

/** Converts a measurement result to JSON; curves are reduced to at most maxPoints each. */
juce::var resultToJson (const AnalysisResult& result, int maxPoints);

/** Applies a JSON object of settings; returns false with a message for unknown keys or bad values. */
bool applySettings (AnalysisSettings& settings, const juce::var& json, juce::String& error);

/** Sets the measurement mode from its API name ("thd", "specs", ...). */
bool applyMode (AnalysisSettings& settings, const juce::String& mode, juce::String& error);

juce::String toJson (const juce::var& value, bool pretty);

} // namespace pa::api
