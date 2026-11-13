// plugin-analyzer-cli: headless measurements with JSON output.
// See docs/CLI.md for the full reference.

#include <juce_audio_processors/juce_audio_processors.h>
#include "api/AnalyzerApi.h"
#include "McpServer.h"
#include <iostream>
#include <iterator>

using juce::String;
using juce::var;

namespace
{

const char* usage = R"(plugin-analyzer-cli - measure audio plugins, output JSON

Usage:
  plugin-analyzer-cli describe
      List measurement modes, settings (with defaults and units) and built-in processors.

  plugin-analyzer-cli plugin-info <plugin>
      Load one plugin and list its latency, channels and parameters.

  plugin-analyzer-cli measure <mode> [options]
      --plugin <plugin>          add a plugin to chain 1 (repeatable, in order)
      --chain2 <plugin>          add a plugin to chain 2 (repeatable)
      --param C.S.NAME=VALUE     set a parameter: chain C, slot S (1-based), name or id
                                 e.g. --param 1.1.Gain=6   --param "1.2.Drive Level=12 dB"
      --set KEY=VALUE            measurement setting, e.g. --set harmonic_freq_hz=1000
                                 (arrays: --set ar_levels_db=-30,0,-30)
      --difference               measure chain 1 minus chain 2
      --points N                 points per curve (default 200, 0 = numbers only)

  plugin-analyzer-cli run <request.json | ->
      Run a JSON request from a file or stdin (the same format the MCP "measure" tool takes).

  plugin-analyzer-cli mcp [--allow-dir <folder>]... [--allow-any-path]
      Run as an MCP (Model Context Protocol) server on stdin/stdout, for LLM clients.
      By default only built-ins and plugins in the standard plugin folders can be loaded.

Common options:
  --pretty        indented JSON
  --out <file>    write JSON to a file instead of stdout

<plugin> is builtin:<id> (e.g. builtin:compressor) or a path to a .vst3 / .component.
Exit code: 0 = success, 1 = error (the JSON then has an "error" field), 2 = bad usage.
)";

var parseValue (const String& text)
{
    const auto t = text.trim();
    if (t.equalsIgnoreCase ("true"))  return true;
    if (t.equalsIgnoreCase ("false")) return false;
    if (t.containsChar (','))
    {
        juce::Array<var> items;
        for (auto& part : juce::StringArray::fromTokens (t, ",", ""))
            items.add (parseValue (part));
        return var (items);
    }
    if (t.isNotEmpty() && t.containsOnly ("0123456789.-+eE") && t.containsAnyOf ("0123456789"))
        return t.getDoubleValue();
    return t;
}

var newObject() { return var (new juce::DynamicObject()); }

int finish (const var& response, bool pretty, const String& outFile)
{
    const auto text = pa::api::toJson (response, pretty);

    if (outFile.isNotEmpty())
    {
        juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile (outFile);
        if (! f.replaceWithText (text + "\n"))
        {
            std::cerr << "Could not write " << f.getFullPathName() << std::endl;
            return 1;
        }
    }
    else
    {
        std::cout << text << std::endl;
    }

    return response.hasProperty ("error") ? 1 : 0;
}

int badUsage (const String& message)
{
    std::cerr << "Error: " << message << "\n\n" << usage;
    return 2;
}

} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // plugins expect a message thread

    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (String::fromUTF8 (argv[i]));

    if (args.isEmpty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help")
    {
        std::cout << usage;
        return args.isEmpty() ? 2 : 0;
    }
    if (args[0] == "--version")
    {
        std::cout << pa::api::describe()["version"].toString() << std::endl;
        return 0;
    }

    const String command = args[0];

    if (command == "mcp")
    {
        juce::StringArray options (args);
        options.remove (0);
        return pa::mcp::runServer (options);
    }

    bool pretty = false;
    String outFile;

    // Pull out the common options first
    for (int i = 1; i < args.size(); ++i)
    {
        if (args[i] == "--pretty") { pretty = true; args.remove (i--); }
        else if (args[i] == "--out")
        {
            if (i + 1 >= args.size()) return badUsage ("--out needs a file name");
            outFile = args[i + 1];
            args.removeRange (i--, 2);
        }
    }

    if (command == "describe")
        return finish (pa::api::describe(), pretty, outFile);

    if (command == "plugin-info")
    {
        if (args.size() < 2) return badUsage ("plugin-info needs a plugin");
        return finish (pa::api::pluginInfo (args[1]), pretty, outFile);
    }

    if (command == "run")
    {
        if (args.size() < 2) return badUsage ("run needs a request file (or - for stdin)");
        String text;
        if (args[1] == "-")
            text = String (std::string (std::istreambuf_iterator<char> (std::cin), std::istreambuf_iterator<char>()));
        else
            text = juce::File::getCurrentWorkingDirectory().getChildFile (args[1]).loadFileAsString();

        var request;
        const auto parsed = juce::JSON::parse (text, request);
        if (parsed.failed())
            return finish ([&] { auto o = newObject(); o.getDynamicObject()->setProperty ("error", "Invalid JSON: " + parsed.getErrorMessage()); return o; }(),
                           pretty, outFile);
        return finish (pa::api::run (request), pretty, outFile);
    }

    if (command == "measure")
    {
        if (args.size() < 2 || args[1].startsWith ("-")) return badUsage ("measure needs a mode (see 'describe')");

        auto request = newObject();
        auto* req = request.getDynamicObject();
        req->setProperty ("mode", args[1]);

        juce::Array<var> chains[2];
        auto settings = newObject();

        for (int i = 2; i < args.size(); ++i)
        {
            const auto& a = args[i];
            auto next = [&] () -> String
            {
                if (i + 1 >= args.size()) return {};
                return args[++i];
            };

            if (a == "--plugin" || a == "--chain1" || a == "--chain2")
            {
                const auto spec = next();
                if (spec.isEmpty()) return badUsage (a + " needs a plugin");
                auto slot = newObject();
                slot.getDynamicObject()->setProperty ("plugin", spec);
                chains[a == "--chain2" ? 1 : 0].add (slot);
            }
            else if (a == "--param")
            {
                const auto spec = next();
                const auto lhs = spec.upToFirstOccurrenceOf ("=", false, false);
                const auto value = spec.fromFirstOccurrenceOf ("=", false, false);
                const int chain = lhs.upToFirstOccurrenceOf (".", false, false).getIntValue();
                const auto rest = lhs.fromFirstOccurrenceOf (".", false, false);
                const int slot = rest.upToFirstOccurrenceOf (".", false, false).getIntValue();
                const auto name = rest.fromFirstOccurrenceOf (".", false, false);

                if (! spec.containsChar ('=') || chain < 1 || chain > 2 || slot < 1 || name.isEmpty())
                    return badUsage ("--param expects CHAIN.SLOT.NAME=VALUE, e.g. --param 1.1.Gain=6");
                if (slot > chains[chain - 1].size())
                    return badUsage ("--param " + spec + ": chain " + String (chain) + " has no slot " + String (slot)
                                     + " (add the plugin first)");

                auto& slotVar = chains[chain - 1].getReference (slot - 1);
                if (! slotVar.hasProperty ("params"))
                    slotVar.getDynamicObject()->setProperty ("params", newObject());
                slotVar["params"].getDynamicObject()->setProperty (name, value);
            }
            else if (a == "--set")
            {
                const auto spec = next();
                if (! spec.containsChar ('=')) return badUsage ("--set expects KEY=VALUE");
                settings.getDynamicObject()->setProperty (spec.upToFirstOccurrenceOf ("=", false, false).trim(),
                                                          parseValue (spec.fromFirstOccurrenceOf ("=", false, false)));
            }
            else if (a == "--difference")
            {
                req->setProperty ("difference", true);
            }
            else if (a == "--points")
            {
                const auto n = next();
                if (n.isEmpty()) return badUsage ("--points needs a number");
                req->setProperty ("max_points", n.getIntValue());
            }
            else
            {
                return badUsage ("Unknown option: " + a);
            }
        }

        if (! chains[0].isEmpty()) req->setProperty ("chain1", var (chains[0]));
        if (! chains[1].isEmpty()) req->setProperty ("chain2", var (chains[1]));
        req->setProperty ("settings", settings);
        return finish (pa::api::run (request), pretty, outFile);
    }

    return badUsage ("Unknown command: " + command);
}
