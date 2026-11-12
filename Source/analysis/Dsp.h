#pragma once

#include <juce_dsp/juce_dsp.h>
#include <complex>
#include <vector>

namespace pa::dsp
{

using cpx = std::complex<float>;

inline float dbToGain (float db) { return std::pow (10.0f, db / 20.0f); }

inline float gainToDb (float g, float floorDb = -240.0f)
{
    return g > 0.0f ? juce::jmax (floorDb, 20.0f * std::log10 (g)) : floorDb;
}

int nextPow2 (int n);

/** Forward FFT of real data, zero padded (or truncated) to fftSize (power of two). Unscaled. */
std::vector<cpx> fftReal (const float* data, int numSamples, int fftSize);

/** Forward FFT of real data multiplied by a window of the same length as numSamples. */
std::vector<cpx> fftWindowed (const float* data, const float* window, int numSamples, int fftSize);

/** Complex FFT (forward or inverse). Inverse is scaled by 1/N. Size must be a power of two. */
std::vector<cpx> fftComplex (const std::vector<cpx>& in, bool inverse);

std::vector<float> hannWindow (int n);
std::vector<float> kaiserWindow (int n, double beta);

enum class Reduce { Nearest, Max, Mean };

/** Resamples per-bin values (bin k = k * binHz) onto log-spaced frequencies in [fMin, fMax]. */
void logReduce (const std::vector<float>& values, double binHz, double fMin, double fMax,
                int numPoints, Reduce mode, std::vector<float>& outX, std::vector<float>& outY);

/** Min/max envelope decimation for long time signals (keeps peaks visible). */
void minMaxDecimate (const float* data, int n, double xStart, double xStep, int buckets,
                     std::vector<float>& outX, std::vector<float>& outY);

struct Stft
{
    int numFrames = 0, numBins = 0;
    int hop = 0;
    std::vector<float> db;   // [frame * numBins + bin], dBFS for a full-scale sine
};

Stft stftDb (const float* data, int numSamples, int frameSize, int hop, double kaiserBeta);

} // namespace pa::dsp
