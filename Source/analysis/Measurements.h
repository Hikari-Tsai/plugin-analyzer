#pragma once

#include "Types.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace pa
{

/** The system under test, as seen by the measurement code. */
class Renderer
{
public:
    virtual ~Renderer() = default;

    /** Resets the system, feeds `flushSamples` of silence, then feeds `input` (2 channels).
        `output` receives 2 channels of the same length, already compensated for the
        reported latency. Returns false if the measurement was aborted. */
    virtual bool render (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output, int flushSamples) = 0;

    /** Processes numBlocks blocks of blockSize samples and returns the elapsed seconds
        spent in processing, or a negative value if aborted/unsupported.
        With silenceAfterSignal, noise is fed first (untimed) and then silence is timed:
        decaying tails expose plugins that slow down on denormal numbers. */
    virtual double timeBlocks (int blockSize, int numBlocks, bool silenceAfterSignal = false) = 0;

    /** Latency reported by the system under test, or -1 if not meaningful. */
    virtual int reportedLatency() const { return -1; }
};

/** Runs the measurement selected by settings.mode and appends curves, heatmaps and
    info lines to `result`, tagged with `chainTag` (0, 1, or 2 = difference). */
bool runMeasurement (const AnalysisSettings& settings, Renderer& renderer, int chainTag, AnalysisResult& result);

} // namespace pa
