#pragma once

#include "engine/AnalysisEngine.h"

namespace pa
{

/** Plugin version: sends the DAW's audio through the playback twins of one chain.

    - Audio thread: process() only ever try-locks, so it never waits for the UI. While a chain
      is being edited, that block passes through unprocessed.
    - Latency: the reported latency is the larger of the two chains' latencies; the selected
      path (chain 1, chain 2 or Off) is delayed to match, so switching between them stays
      time-aligned and the host's delay compensation never has to change. */
class PlaybackRouter
{
public:
    explicit PlaybackRouter (AnalysisEngine& e) : engine (e) {}

    /** Lock held by the message thread while slots are replaced or twins' state is set. */
    juce::CriticalSection& getLock() { return lock; }

    /** From the host's prepareToPlay: (re)prepares every playback twin. */
    void prepare (double sampleRate, int maximumBlockSize);
    bool isPrepared() const { return prepared; }

    /** Prepares one twin at the host rate (call with getLock() held). No-op before prepare(). */
    void prepareProcessor (juce::AudioProcessor& p);

    /** -1 = off (dry, latency-aligned), 0 = chain 1, 1 = chain 2. */
    void setPlayChain (int chain) { playChain = juce::jlimit (-1, 1, chain); }
    int getPlayChain() const { return playChain.load(); }

    /** Message thread: recomputes the reported latency; calls onLatencyChanged if it moved. */
    void updateLatency();
    int getReportedLatency() const { return reportedLatency.load(); }
    int getChainLatency (int chain) const;
    std::function<void (int)> onLatencyChanged;

    /** Audio thread. Processes the buffer's first one or two channels in place. */
    void process (juce::AudioBuffer<float>& buffer);

private:
    AnalysisEngine& engine;
    juce::CriticalSection lock;

    double rate = 0.0;
    int maxBlock = 0;
    bool prepared = false;

    std::atomic<int> playChain { -1 };
    std::atomic<int> reportedLatency { 0 };

    juce::AudioBuffer<float> work, scratch, delayLine;
    juce::MidiBuffer midi;
    int delayWrite = 0;
};

/** Mirrors every parameter change of one processor onto its twin (same plugin, same order). */
class ParamMirror : private juce::AudioProcessorParameter::Listener
{
public:
    ParamMirror (juce::AudioProcessor& source, juce::AudioProcessor& target);
    ~ParamMirror() override;

private:
    void parameterValueChanged (int index, float newValue) override;
    void parameterGestureChanged (int, bool) override {}

    juce::AudioProcessor& source;
    juce::AudioProcessor& target;
};

/** Copies the complete state of one processor to another. */
void copyProcessorState (juce::AudioProcessor& from, juce::AudioProcessor& to);

} // namespace pa
