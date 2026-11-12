#include "Dsp.h"

#include <map>
#include <mutex>

namespace pa::dsp
{

int nextPow2 (int n)
{
    int p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

static const juce::dsp::FFT& getFFT (int size)
{
    // One FFT object per size, shared between threads (perform() is const and thread safe).
    static std::mutex mutex;
    static std::map<int, std::unique_ptr<juce::dsp::FFT>> cache;

    std::lock_guard<std::mutex> lock (mutex);
    auto& entry = cache[size];

    if (entry == nullptr)
    {
        int order = 0;
        while ((1 << order) < size)
            ++order;
        entry = std::make_unique<juce::dsp::FFT> (order);
    }

    return *entry;
}

std::vector<cpx> fftComplex (const std::vector<cpx>& in, bool inverse)
{
    const int size = (int) in.size();
    jassert (juce::isPowerOfTwo (size));
    std::vector<cpx> out ((size_t) size);
    getFFT (size).perform (in.data(), out.data(), inverse);
    return out;
}

std::vector<cpx> fftReal (const float* data, int numSamples, int fftSize)
{
    std::vector<cpx> in ((size_t) fftSize, cpx (0.0f, 0.0f));
    const int n = juce::jmin (numSamples, fftSize);

    for (int i = 0; i < n; ++i)
        in[(size_t) i] = cpx (data[i], 0.0f);

    return fftComplex (in, false);
}

std::vector<cpx> fftWindowed (const float* data, const float* window, int numSamples, int fftSize)
{
    std::vector<cpx> in ((size_t) fftSize, cpx (0.0f, 0.0f));
    const int n = juce::jmin (numSamples, fftSize);

    for (int i = 0; i < n; ++i)
        in[(size_t) i] = cpx (data[i] * window[i], 0.0f);

    return fftComplex (in, false);
}

std::vector<float> hannWindow (int n)
{
    std::vector<float> w ((size_t) n);

    for (int i = 0; i < n; ++i)
        w[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) n);

    return w;
}

static double besselI0 (double x)
{
    // Power series; converges quickly for the beta values used here.
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;

    for (int k = 1; k < 200; ++k)
    {
        term *= q / ((double) k * (double) k);
        sum += term;

        if (term < sum * 1.0e-17)
            break;
    }

    return sum;
}

std::vector<float> kaiserWindow (int n, double beta)
{
    std::vector<float> w ((size_t) n);
    const double denom = besselI0 (beta);

    for (int i = 0; i < n; ++i)
    {
        const double r = 2.0 * (double) i / (double) (n - 1) - 1.0;
        w[(size_t) i] = (float) (besselI0 (beta * std::sqrt (juce::jmax (0.0, 1.0 - r * r))) / denom);
    }

    return w;
}

void logReduce (const std::vector<float>& values, double binHz, double fMin, double fMax,
                int numPoints, Reduce mode, std::vector<float>& outX, std::vector<float>& outY)
{
    outX.clear();
    outY.clear();

    const int numBins = (int) values.size();

    if (numBins < 2 || fMin <= 0.0 || fMax <= fMin || numPoints < 2)
        return;

    fMax = juce::jmin (fMax, (numBins - 1) * binHz);
    const double ratio = std::pow (fMax / fMin, 1.0 / (numPoints - 1));
    const double halfStep = std::sqrt (ratio);

    outX.reserve ((size_t) numPoints);
    outY.reserve ((size_t) numPoints);

    for (int i = 0; i < numPoints; ++i)
    {
        const double f = fMin * std::pow (ratio, (double) i);
        const int lo = (int) std::ceil ((f / halfStep) / binHz);
        const int hi = juce::jmin (numBins - 1, (int) std::floor ((f * halfStep) / binHz));

        float v;

        // Every bin belongs to exactly one band, so narrow peaks are never skipped.
        if (mode != Reduce::Nearest && hi >= lo)
        {
            if (mode == Reduce::Max)
            {
                v = values[(size_t) juce::jmax (0, lo)];
                for (int k = juce::jmax (0, lo); k <= hi; ++k)
                    v = juce::jmax (v, values[(size_t) k]);
            }
            else
            {
                double sum = 0.0;
                for (int k = juce::jmax (0, lo); k <= hi; ++k)
                    sum += values[(size_t) k];
                v = (float) (sum / (double) (hi - juce::jmax (0, lo) + 1));
            }
        }
        else if (mode == Reduce::Nearest)
        {
            v = values[(size_t) juce::jlimit (0, numBins - 1, (int) std::lround (f / binHz))];
        }
        else
        {
            // Fewer than two bins in this band: interpolate linearly between neighbours.
            const double pos = f / binHz;
            const int k0 = juce::jlimit (0, numBins - 1, (int) std::floor (pos));
            const int k1 = juce::jmin (numBins - 1, k0 + 1);
            const float t = (float) (pos - (double) k0);
            v = values[(size_t) k0] + t * (values[(size_t) k1] - values[(size_t) k0]);
        }

        outX.push_back ((float) f);
        outY.push_back (v);
    }
}

void minMaxDecimate (const float* data, int n, double xStart, double xStep, int buckets,
                     std::vector<float>& outX, std::vector<float>& outY)
{
    outX.clear();
    outY.clear();

    if (n <= 0)
        return;

    if (n <= buckets * 2)
    {
        outX.reserve ((size_t) n);
        outY.reserve ((size_t) n);

        for (int i = 0; i < n; ++i)
        {
            outX.push_back ((float) (xStart + xStep * i));
            outY.push_back (data[i]);
        }
        return;
    }

    outX.reserve ((size_t) buckets * 2);
    outY.reserve ((size_t) buckets * 2);

    for (int b = 0; b < buckets; ++b)
    {
        const int s = (int) ((int64_t) n * b / buckets);
        const int e = (int) ((int64_t) n * (b + 1) / buckets);
        int iMin = s, iMax = s;

        for (int i = s; i < e; ++i)
        {
            if (data[i] < data[iMin]) iMin = i;
            if (data[i] > data[iMax]) iMax = i;
        }

        const int first = juce::jmin (iMin, iMax), second = juce::jmax (iMin, iMax);
        outX.push_back ((float) (xStart + xStep * first));
        outY.push_back (data[first]);
        outX.push_back ((float) (xStart + xStep * second));
        outY.push_back (data[second]);
    }
}

Stft stftDb (const float* data, int numSamples, int frameSize, int hop, double kaiserBeta)
{
    Stft result;

    if (numSamples < frameSize || hop <= 0)
        return result;

    const auto window = kaiserWindow (frameSize, kaiserBeta);
    double windowSum = 0.0;
    for (auto w : window)
        windowSum += w;

    // A full-scale sine produces |X| = windowSum / 2 at its bin -> 0 dB.
    const float norm = (float) (2.0 / windowSum);

    result.numBins = frameSize / 2;
    result.hop = hop;
    result.numFrames = 1 + (numSamples - frameSize) / hop;
    result.db.resize ((size_t) result.numFrames * (size_t) result.numBins);

    for (int f = 0; f < result.numFrames; ++f)
    {
        const auto spectrum = fftWindowed (data + (size_t) f * (size_t) hop, window.data(), frameSize, frameSize);
        float* dest = result.db.data() + (size_t) f * (size_t) result.numBins;

        for (int k = 0; k < result.numBins; ++k)
            dest[k] = gainToDb (std::abs (spectrum[(size_t) k]) * norm, -250.0f);
    }

    return result;
}

} // namespace pa::dsp
