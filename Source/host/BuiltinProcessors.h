#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace pa
{

/** Small reference processors with known behaviour. Useful to sanity-check the analyzer
    and to learn what each analysis view shows. */
struct BuiltinInfo
{
    juce::String id;
    juce::String name;
};

const std::vector<BuiltinInfo>& getBuiltinList();
std::unique_ptr<juce::AudioProcessor> createBuiltin (const juce::String& id);

/** Sets a built-in processor's parameter by its ID (no-op if not found). Used in tests. */
void setBuiltinParameter (juce::AudioProcessor& p, const juce::String& paramId, float plainValue);

} // namespace pa
