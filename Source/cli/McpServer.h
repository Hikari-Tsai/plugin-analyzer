#pragma once

#include <juce_core/juce_core.h>

namespace pa::mcp
{

/** Runs a Model Context Protocol server on stdin/stdout until stdin closes.
    Options (after "mcp"):
      --allow-dir <folder>   also allow plugins from this folder (repeatable)
      --allow-any-path       allow plugins from any path
    Returns the process exit code. */
int runServer (const juce::StringArray& options);

} // namespace pa::mcp
