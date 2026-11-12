#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace pa
{

enum class Mode { Linear = 0, Harmonic, Oscilloscope, Dynamics, Hammerstein, Reverb, Performance, Specs };

enum class LinearSignal { Delta, Noise };
enum class HarmonicMode { THD, IMD, Sweep1D, Sweep2D, ThdVsLevel };
enum class Waveform { Sine, Square, Saw, Triangle };
enum class DynamicsMode { Ramp, AttackRelease };

/** Everything the analysis thread needs to run one measurement. Copied by value. */
struct AnalysisSettings
{
    Mode mode = Mode::Linear;
    double sampleRate = 48000.0;
    int quality = 0;            // analysis length = 16384 << quality
    int blockSize = 512;
    bool midSide = false;
    bool difference = false;    // show chain1 - chain2 instead of both chains
    int pauseMs = 30;           // idle time between measurements

    int fftSize() const { return 16384 << juce::jlimit (0, 2, quality); }

    // Linear
    LinearSignal linearSignal = LinearSignal::Delta;
    float linearLevelDb = 0.0f;

    // Harmonic
    HarmonicMode harmonicMode = HarmonicMode::THD;
    float harmonicLevelDb = -6.0f;
    float harmonicFreq = 1000.0f;
    int sweepPoints = 64;
    float sweep2DStart = 20.0f, sweep2DEnd = 20000.0f;
    bool sweep2DExponential = true;
    float sweep2DSeconds = 6.0f;
    float levelSweepFromDb = -60.0f, levelSweepToDb = 0.0f, levelSweepStepDb = 2.0f;

    // Oscilloscope
    Waveform oscWaveform = Waveform::Sine;
    bool oscWaveshaper = false;
    float oscFreq = 100.0f;
    float oscLevelDb = 0.0f;
    int oscDelaySamples = 0;

    // Dynamics
    DynamicsMode dynamicsMode = DynamicsMode::Ramp;
    float dynamicsFreq = 1000.0f;
    float rampStartDb = -100.0f, rampEndDb = 0.0f, rampStepDb = 1.0f;
    float rampSegmentSeconds = 0.2f;
    float arLevelsDb[3] { -30.0f, 0.0f, -30.0f };
    float arSeconds[3] { 0.3f, 0.5f, 1.0f };

    // Hammerstein (synchronized swept sine)
    int hammersteinOrder = 5;
    float hammersteinLevelDb = -6.0f;
    float hammersteinStart = 10.0f, hammersteinEnd = 20000.0f;
    float hammersteinSeconds = 4.0f;

    // Reverb
    float reverbSeconds = 5.0f;
    float reverbLevelDb = 0.0f;
};

/** A plotted line. `kind` tells the tab which view it belongs to. */
struct Curve
{
    juce::String kind;
    int chain = 0;      // 0 = chain 1, 1 = chain 2, 2 = difference (1 - 2)
    int channel = 0;    // 0 = L/Mid, 1 = R/Side (Hammerstein: order)
    std::vector<float> x, y;
};

/** A time/frequency image (values in dB), stored column-major: db[x * numY + y]. */
struct Heatmap
{
    juce::String kind;
    int chain = 0;
    int numX = 0, numY = 0;
    std::vector<float> db;
    float xMin = 0, xMax = 1;   // seconds
    float yMin = 0, yMax = 1;   // Hz
};

/** One row of the Specs table. */
struct SpecItem
{
    juce::String group, name;
    int chain = 0;
    juce::String text;      // formatted value with unit
    juce::String note;
};

/** A named numeric result (e.g. thd_percent) for scripts, the CLI and LLM tools. */
struct Metric
{
    juce::String name;
    int chain = 0;
    int channel = -1;       // -1 = whole chain, 0 = L/Mid, 1 = R/Side
    double value = 0.0;
    juce::String unit;
};

struct AnalysisResult
{
    Mode mode = Mode::Linear;
    juce::uint64 sequence = 0;
    double sampleRate = 48000.0;
    bool midSide = false;
    std::vector<Curve> curves;
    std::vector<Heatmap> heatmaps;
    std::vector<SpecItem> specs;
    std::vector<Metric> metrics;
    juce::StringArray info;
};

inline juce::String chainLabel (int chain)
{
    return chain == 0 ? "1" : chain == 1 ? "2" : "1-2";
}

inline juce::String channelLabel (bool midSide, int channel)
{
    return midSide ? (channel == 0 ? "Mid" : "Side") : (channel == 0 ? "L" : "R");
}

} // namespace pa
