#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "analysis/Measurements.h"
#include <array>
#include <atomic>
#include <functional>

namespace pa
{

/** Asks for stereo in/out (falling back to mono) and enables any side-chain buses. */
void configureStereoLayout (juce::AudioProcessor& p);

/** Runs one processor on the first numSamples of a 2-channel buffer, in place.
    scratch must have enough channels for the processor and at least numSamples. */
void processInPlace (juce::AudioProcessor& p, juce::AudioBuffer<float>& io, int numSamples,
                     juce::AudioBuffer<float>& scratch, juce::MidiBuffer& midi);

/** Up to six processors in series. Not thread safe by itself: the owner serialises
    structural edits against processing with a lock. */
class PluginChain
{
public:
    static constexpr int numSlots = 6;
    static constexpr int maxBlockSize = 8192;

    struct Slot
    {
        std::unique_ptr<juce::AudioProcessor> processor;        // used by the analysis thread
        std::unique_ptr<juce::AudioProcessor> playProcessor;    // plugin version only: a twin for the DAW's audio
        juce::String name;
        std::unique_ptr<juce::XmlElement> source;   // PluginDescription XML or <BUILTIN id=".."/>
        std::atomic<bool> bypassed { false };
    };

    PluginChain();

    Slot& slot (int index) { return slots[(size_t) index]; }
    const Slot& slot (int index) const { return slots[(size_t) index]; }

    /** Installs a processor (prepares it at the current rate). Pass nullptr to clear. */
    void setSlot (int index, std::unique_ptr<juce::AudioProcessor> processor,
                  const juce::String& name, std::unique_ptr<juce::XmlElement> source,
                  std::unique_ptr<juce::AudioProcessor> playProcessor = nullptr);

    void prepare (double sampleRate);
    double getSampleRate() const { return sampleRate; }

    bool hasAnyProcessor() const;
    int getLatencySamples() const;
    void reset();

    /** Processes the first numSamples of a 2-channel buffer in place. */
    void process (juce::AudioBuffer<float>& io, int numSamples);

private:
    void prepareProcessor (juce::AudioProcessor& p);

    std::array<Slot, numSlots> slots;
    double sampleRate = 48000.0;
    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midi;
};

/** Renders test signals through a PluginChain in fixed-size blocks. */
class ChainRenderer : public Renderer
{
public:
    ChainRenderer (PluginChain& chainToUse, int blockSizeToUse, std::function<bool()> shouldAbortFn)
        : chain (chainToUse), blockSize (juce::jlimit (16, PluginChain::maxBlockSize, blockSizeToUse)),
          shouldAbort (std::move (shouldAbortFn)) {}

    bool render (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output, int flushSamples) override;
    double timeBlocks (int blockSize, int numBlocks, bool silenceAfterSignal) override;
    int reportedLatency() const override { return chain.getLatencySamples(); }

private:
    PluginChain& chain;
    int blockSize;
    std::function<bool()> shouldAbort;
};

/** chain A minus chain B (null test). */
class DifferenceRenderer : public Renderer
{
public:
    DifferenceRenderer (Renderer& a, Renderer& b) : first (a), second (b) {}

    bool render (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output, int flushSamples) override;
    double timeBlocks (int, int, bool) override { return -1.0; }

private:
    Renderer& first;
    Renderer& second;
};

} // namespace pa
