#include "Measurements.h"
#include "Dsp.h"

namespace pa
{

using namespace dsp;
using juce::AudioBuffer;
using juce::String;

namespace
{

constexpr double kPi = juce::MathConstants<double>::pi;
constexpr double kTwoPi = juce::MathConstants<double>::twoPi;

//==============================================================================
struct Ctx
{
    const AnalysisSettings& s;
    Renderer& renderer;
    int chain;
    AnalysisResult& out;

    double sr() const { return s.sampleRate; }
    int defaultFlush() const { return (int) (s.sampleRate * 0.05); }

    bool render (const AudioBuffer<float>& in, AudioBuffer<float>& o, int flush)
    {
        if (! renderer.render (in, o, flush))
            return false;

        if (s.midSide && o.getNumChannels() >= 2)
        {
            auto* l = o.getWritePointer (0);
            auto* r = o.getWritePointer (1);

            for (int i = 0; i < o.getNumSamples(); ++i)
            {
                const float m = 0.5f * (l[i] + r[i]);
                const float d = 0.5f * (l[i] - r[i]);
                l[i] = m;
                r[i] = d;
            }
        }

        return true;
    }

    void addCurve (const String& kind, int channel, std::vector<float> x, std::vector<float> y)
    {
        Curve c;
        c.kind = kind;
        c.chain = chain;
        c.channel = channel;
        c.x = std::move (x);
        c.y = std::move (y);
        out.curves.push_back (std::move (c));
    }

    String prefix (int channel) const
    {
        return "[" + chainLabel (chain) + " " + channelLabel (s.midSide, channel) + "]";
    }
};

AudioBuffer<float> makeStereo (int numSamples)
{
    AudioBuffer<float> b (2, juce::jmax (1, numSamples));
    b.clear();
    return b;
}

void duplicateLeft (AudioBuffer<float>& b)
{
    b.copyFrom (1, 0, b, 0, 0, b.getNumSamples());
}

/** Single-sided amplitude spectrum: a sine of amplitude A on an exact bin reads A. */
std::vector<float> amplitudes (const float* data, int n)
{
    const auto X = fftReal (data, n, n);
    std::vector<float> a ((size_t) n / 2 + 1);
    a[0] = std::abs (X[0]) / (float) n;

    for (int k = 1; k <= n / 2; ++k)
        a[(size_t) k] = 2.0f * std::abs (X[(size_t) k]) / (float) n;

    return a;
}

std::vector<float> toDb (const std::vector<float>& a, float scale = 1.0f)
{
    std::vector<float> d (a.size());
    for (size_t i = 0; i < a.size(); ++i)
        d[i] = gainToDb (a[i] * scale);
    return d;
}

/** Nearest odd FFT bin to f. With an odd bin and a power-of-two FFT, aliased harmonics
    (at N - k*m) never coincide with true harmonics (multiples of m). */
int oddBin (double f, int N, double sr)
{
    int m = juce::jlimit (1, N / 2 - 1, (int) std::lround (f * N / sr));
    if ((m & 1) == 0)
        m = (f * N / sr >= m || m + 1 >= N / 2) ? (m + 1 < N / 2 ? m + 1 : m - 1) : m - 1;
    return juce::jmax (1, m);
}

/** IEC 61672-1 A-weighting as a linear amplitude factor (0 dB at 1 kHz). */
double aWeightGain (double f)
{
    const double f2 = f * f;
    const double num = 12194.0 * 12194.0 * f2 * f2;
    const double den = (f2 + 20.6 * 20.6) * std::sqrt ((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) * (f2 + 12194.0 * 12194.0);
    return den > 0 ? num / den * 1.2589254 : 0.0;   // +2.00 dB normalisation
}

/** Peak of the band-limited (4x oversampled) reconstruction of one period-exact block. */
float truePeak (const float* x, int N)
{
    const auto X = fftReal (x, N, N);
    const int M = 4 * N;
    std::vector<cpx> Y ((size_t) M);
    for (int k = 0; k < N / 2; ++k)
        Y[(size_t) k] = X[(size_t) k];
    for (int k = 1; k < N / 2; ++k)
        Y[(size_t) (M - k)] = X[(size_t) (N - k)];
    Y[(size_t) (N / 2)] = 0.5f * X[(size_t) (N / 2)];
    Y[(size_t) (M - N / 2)] = 0.5f * X[(size_t) (N / 2)];

    const auto y = fftComplex (Y, true);
    float peak = 0.0f;
    for (auto& v : y)
        peak = juce::jmax (peak, std::abs (v.real()) * 4.0f);
    return peak;
}

String percentText (float ratio)
{
    const double pct = (double) ratio * 100.0;

    if (pct < 0.0001)
        return "<0.0001%";

    return String (pct, pct < 0.1 ? 4 : 3) + "%";
}

double dbText (float ratio) { return (double) gainToDb (ratio, -200.0f); }

//==============================================================================
// Linear analysis: impulse ("delta") or white-noise (H1 cross-spectral estimate)
//==============================================================================
void addSpectrumCurves (Ctx& c, int channel, const std::vector<cpx>& H, int fftSize)
{
    const int numBins = fftSize / 2 + 1;
    std::vector<float> mag ((size_t) numBins), phase ((size_t) numBins);

    for (int k = 0; k < numBins; ++k)
    {
        mag[(size_t) k] = gainToDb (std::abs (H[(size_t) k]));
        phase[(size_t) k] = (float) (std::arg (H[(size_t) k]) * 180.0 / kPi);
    }

    const double binHz = c.sr() / fftSize;
    const double fMin = juce::jmax (binHz, 5.0);
    std::vector<float> x, y;

    logReduce (mag, binHz, fMin, c.sr() / 2, 1600, Reduce::Mean, x, y);
    c.addCurve ("mag", channel, x, y);

    logReduce (phase, binHz, fMin, c.sr() / 2, 1600, Reduce::Nearest, x, y);
    c.addCurve ("phase", channel, x, y);

    // Group delay tau(f) = -dphi/domega, central difference; held where the response is ~silent.
    float peak = 0.0f;
    for (int k = 0; k < numBins; ++k)
        peak = juce::jmax (peak, std::abs (H[(size_t) k]));

    std::vector<float> gd ((size_t) numBins, 0.0f);
    for (int k = 1; k < numBins - 1; ++k)
    {
        if (std::abs (H[(size_t) k]) < peak * 1.0e-4f)
        {
            gd[(size_t) k] = gd[(size_t) k - 1];
            continue;
        }
        const double d = std::arg (H[(size_t) k + 1] * std::conj (H[(size_t) k - 1]));
        gd[(size_t) k] = (float) (-d / (kTwoPi * 2.0 * binHz) * 1000.0);
    }
    gd[0] = gd[1];
    gd[(size_t) numBins - 1] = gd[(size_t) numBins - 2];

    logReduce (gd, binHz, fMin, c.sr() / 2, 1600, Reduce::Mean, x, y);
    c.addCurve ("gd", channel, x, y);
}

void addImpulseCurve (Ctx& c, int channel, const float* ir, int n)
{
    const int len = juce::jmin (n, 8192);
    std::vector<float> x ((size_t) len), y ((size_t) len);

    for (int i = 0; i < len; ++i)
    {
        x[(size_t) i] = (float) (1000.0 * i / c.sr());
        y[(size_t) i] = ir[i];
    }

    c.addCurve ("ir", channel, std::move (x), std::move (y));
}

bool measureLinear (Ctx& c)
{
    const int N = c.s.fftSize();
    const float a = dbToGain (c.s.linearLevelDb);

    if (c.s.linearSignal == LinearSignal::Delta)
    {
        auto in = makeStereo (N);
        in.setSample (0, 0, a);
        in.setSample (1, 0, a);

        AudioBuffer<float> out;
        if (! c.render (in, out, c.defaultFlush()))
            return false;

        for (int ch = 0; ch < 2; ++ch)
        {
            out.applyGain (ch, 0, N, 1.0f / a);
            const auto H = fftReal (out.getReadPointer (ch), N, N);
            addSpectrumCurves (c, ch, H, N);
            addImpulseCurve (c, ch, out.getReadPointer (ch), N);
        }

        // Crosstalk: impulse on one input channel only, raw L/R (independent of M/S mode)
        for (int src = 0; src < 2; ++src)
        {
            auto one = makeStereo (N);
            one.setSample (src, 0, a);
            AudioBuffer<float> o;
            if (! c.renderer.render (one, o, c.defaultFlush()))
                return false;

            const auto Hs = fftReal (o.getReadPointer (src), N, N);
            const auto Hx = fftReal (o.getReadPointer (1 - src), N, N);
            std::vector<float> ratio ((size_t) N / 2 + 1);
            for (int k = 0; k <= N / 2; ++k)
            {
                const float direct = std::abs (Hs[(size_t) k]);
                ratio[(size_t) k] = direct > 1.0e-12f ? gainToDb (std::abs (Hx[(size_t) k]) / direct, -200.0f) : -200.0f;
            }

            std::vector<float> x, y;
            const double binHz = c.sr() / N;
            logReduce (ratio, binHz, juce::jmax (binHz, 5.0), c.sr() / 2, 1200, Reduce::Mean, x, y);
            c.addCurve ("xtalk", src, x, y);
        }

        return true;
    }

    // Noise: average K Hann-windowed frames, H = Sxy / Sxx
    constexpr int K = 8;
    const int total = N * (K + 1);
    auto in = makeStereo (total);
    juce::Random rng;

    for (int i = 0; i < total; ++i)
        in.setSample (0, i, a * (2.0f * rng.nextFloat() - 1.0f));
    duplicateLeft (in);

    AudioBuffer<float> out;
    if (! c.render (in, out, c.defaultFlush()))
        return false;

    const auto window = hannWindow (N);

    for (int ch = 0; ch < 2; ++ch)
    {
        std::vector<cpx> sxy ((size_t) N / 2 + 1);
        std::vector<double> sxx ((size_t) N / 2 + 1, 0.0);

        for (int f = 1; f <= K; ++f)   // first frame discarded as settling time
        {
            const int offset = f * N;
            const auto X = fftWindowed (in.getReadPointer (0) + offset, window.data(), N, N);
            const auto Y = fftWindowed (out.getReadPointer (ch) + offset, window.data(), N, N);

            for (int k = 0; k <= N / 2; ++k)
            {
                sxy[(size_t) k] += Y[(size_t) k] * std::conj (X[(size_t) k]);
                sxx[(size_t) k] += (double) std::norm (X[(size_t) k]);
            }
        }

        std::vector<cpx> H ((size_t) N);
        for (int k = 0; k <= N / 2; ++k)
            H[(size_t) k] = sxx[(size_t) k] > 0 ? sxy[(size_t) k] / (float) sxx[(size_t) k] : cpx();
        for (int k = 1; k < N / 2; ++k)
            H[(size_t) (N - k)] = std::conj (H[(size_t) k]);

        addSpectrumCurves (c, ch, H, N);

        const auto h = fftComplex (H, true);
        std::vector<float> ir ((size_t) N);
        for (int i = 0; i < N; ++i)
            ir[(size_t) i] = h[(size_t) i].real();

        addImpulseCurve (c, ch, ir.data(), N);
    }

    return true;
}

//==============================================================================
// Harmonic analysis
//==============================================================================
struct ThdResult
{
    float fundamental = 0, thd = 0, thdN = 0;
    float nonHarmonic = 0;   // everything except DC, fundamental and in-band harmonics (aliasing + noise)
    std::vector<std::pair<int, float>> harmonicsDbc;
};

ThdResult analyseThd (const std::vector<float>& A, int m)
{
    ThdResult r;
    const int nyquistBin = (int) A.size() - 1;
    r.fundamental = A[(size_t) m];

    if (r.fundamental <= 0.0f)
        return r;

    double harmonics = 0.0;

    for (int k = 2; k * m < nyquistBin; ++k)
    {
        const double h = A[(size_t) (k * m)];
        harmonics += h * h;

        if (k <= 9)
            r.harmonicsDbc.push_back ({ k, gainToDb ((float) h / r.fundamental, -200.0f) });
    }

    double total = 0.0;
    for (int k = 1; k < nyquistBin; ++k)
        total += (double) A[(size_t) k] * A[(size_t) k];

    const double noise = juce::jmax (0.0, total - (double) r.fundamental * r.fundamental);
    r.thd = (float) (std::sqrt (harmonics) / r.fundamental);
    r.thdN = (float) (std::sqrt (noise) / r.fundamental);
    r.nonHarmonic = (float) (std::sqrt (juce::jmax (0.0, noise - harmonics)) / r.fundamental);
    return r;
}

void addAmplitudeSpectrum (Ctx& c, int channel, const std::vector<float>& A, int N)
{
    std::vector<float> x, y;
    const double binHz = c.sr() / N;
    logReduce (toDb (A), binHz, juce::jmax (binHz, 5.0), c.sr() / 2, 2400, Reduce::Max, x, y);
    c.addCurve ("spectrum", channel, x, y);
}

/** Sine on an exact FFT bin: warm-up period of N samples, then N analysed samples. */
AudioBuffer<float> makeBinSine (int N, int bin, float amp)
{
    auto in = makeStereo (2 * N);
    auto* d = in.getWritePointer (0);

    for (int i = 0; i < 2 * N; ++i)
        d[i] = amp * (float) std::sin (kTwoPi * (double) bin * (double) (i % N) / (double) N);

    duplicateLeft (in);
    return in;
}

bool measureThd (Ctx& c)
{
    const int N = c.s.fftSize();
    const float a = dbToGain (c.s.harmonicLevelDb);
    const int m = oddBin (c.s.harmonicFreq, N, c.sr());

    auto in = makeBinSine (N, m, a);
    AudioBuffer<float> out;
    if (! c.render (in, out, c.defaultFlush()))
        return false;

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto A = amplitudes (out.getReadPointer (ch) + N, N);
        addAmplitudeSpectrum (c, ch, A, N);

        const auto r = analyseThd (A, m);
        String line = c.prefix (ch) + "  f0 " + String (m * c.sr() / N, 1) + " Hz"
                    + "   THD " + percentText (r.thd) + " (" + String (dbText (r.thd), 1) + " dB)"
                    + "   THD+N " + percentText (r.thdN) + " (" + String (dbText (r.thdN), 1) + " dB)";
        c.out.info.add (line);

        String hl = "      ";
        for (auto [k, db] : r.harmonicsDbc)
            hl << "H" << k << " " << String (db, 1) << "  ";
        if (r.harmonicsDbc.size() > 0)
            c.out.info.add (hl + "dBc");
    }

    return true;
}

bool measureImd (Ctx& c)
{
    // SMPTE-style: 60 Hz and 7 kHz, 4:1 amplitude ratio (12 dB), combined peak = level
    const int N = c.s.fftSize();
    const float a = dbToGain (c.s.harmonicLevelDb);
    const int mLow = juce::jmax (1, (int) std::lround (60.0 * N / c.sr()));
    const int mHigh = juce::jlimit (mLow * 12, N / 2 - 1, (int) std::lround (7000.0 * N / c.sr()));

    auto in = makeStereo (2 * N);
    auto* d = in.getWritePointer (0);

    for (int i = 0; i < 2 * N; ++i)
    {
        const double ph = (double) (i % N) / (double) N;
        d[i] = (float) (0.8 * a * std::sin (kTwoPi * mLow * ph) + 0.2 * a * std::sin (kTwoPi * mHigh * ph));
    }
    duplicateLeft (in);

    AudioBuffer<float> out;
    if (! c.render (in, out, c.defaultFlush()))
        return false;

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto A = amplitudes (out.getReadPointer (ch) + N, N);
        addAmplitudeSpectrum (c, ch, A, N);

        double side = 0.0;
        for (int n = 1; n <= 10; ++n)
        {
            for (int k : { mHigh - n * mLow, mHigh + n * mLow })
                if (k > 0 && k < N / 2)
                    side += (double) A[(size_t) k] * A[(size_t) k];
        }

        const float carrier = A[(size_t) mHigh];
        const float imd = carrier > 0 ? (float) (std::sqrt (side) / carrier) : 0.0f;
        c.out.info.add (c.prefix (ch) + "  IMD (" + String (mLow * c.sr() / N, 1) + " Hz + "
                        + String (mHigh * c.sr() / N, 0) + " Hz)  " + percentText (imd)
                        + " (" + String (dbText (imd), 1) + " dB)");
    }

    return true;
}

bool measureSweep1D (Ctx& c)
{
    const int N = c.s.fftSize();
    const float a = dbToGain (c.s.harmonicLevelDb);
    const int minBin = 10;                 // at least 10 periods in the analysis window
    const int maxBin = N / 2 - 2;
    const int points = juce::jlimit (8, 256, c.s.sweepPoints);

    std::vector<int> bins;
    for (int i = 0; i < points; ++i)
    {
        const int b = (int) std::lround (minBin * std::pow ((double) maxBin / minBin, (double) i / (points - 1))) | 1;
        if (b <= maxBin && (bins.empty() || b > bins.back()))
            bins.push_back (b);
    }

    std::vector<float> fx[2], fy[2], tx[2], ty[2], ax[2], ay[2];

    for (int m : bins)
    {
        auto in = makeBinSine (N, m, a);
        AudioBuffer<float> out;
        if (! c.render (in, out, c.defaultFlush() / 4))
            return false;

        const float f = (float) (m * c.sr() / N);

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto A = amplitudes (out.getReadPointer (ch) + N, N);
            const auto r = analyseThd (A, m);

            fx[ch].push_back (f);
            fy[ch].push_back (gainToDb (r.fundamental / a));
            ax[ch].push_back (f);
            ay[ch].push_back (gainToDb (r.nonHarmonic, -200.0f));

            if (2 * m < N / 2)    // at least one harmonic below Nyquist
            {
                tx[ch].push_back (f);
                ty[ch].push_back (gainToDb (r.thd, -200.0f));
            }
        }
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        c.addCurve ("fundamental", ch, fx[ch], fy[ch]);
        c.addCurve ("thd", ch, tx[ch], ty[ch]);
        c.addCurve ("alias", ch, ax[ch], ay[ch]);
    }

    return true;
}

bool measureThdVsLevel (Ctx& c)
{
    const int N = c.s.fftSize();
    const int m = oddBin (c.s.harmonicFreq, N, c.sr());
    const float lo = juce::jmax (-140.0f, juce::jmin (c.s.levelSweepFromDb, c.s.levelSweepToDb));
    const float hi = juce::jmax (c.s.levelSweepFromDb, c.s.levelSweepToDb);
    const float step = juce::jlimit (0.5f, 12.0f, c.s.levelSweepStepDb);

    std::vector<float> x, thdn[2], thd[2], gain[2];

    for (float level = lo; level <= hi + 1.0e-3f; level += step)
    {
        const float a = dbToGain (level);
        auto in = makeBinSine (N, m, a);
        AudioBuffer<float> out;
        if (! c.render (in, out, c.defaultFlush() / 4))
            return false;

        x.push_back (level);
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto r = analyseThd (amplitudes (out.getReadPointer (ch) + N, N), m);
            thdn[ch].push_back (gainToDb (r.thdN, -200.0f));
            thd[ch].push_back (gainToDb (r.thd, -200.0f));
            gain[ch].push_back (gainToDb (r.fundamental / a, -200.0f));
        }
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        c.addCurve ("lvl_thdn", ch, x, thdn[ch]);
        c.addCurve ("lvl_thd", ch, x, thd[ch]);
        c.addCurve ("lvl_gain", ch, x, gain[ch]);
    }

    c.out.info.add ("[" + chainLabel (c.chain) + "]  f0 " + String (m * c.sr() / N, 1) + " Hz, "
                    + String (x.size()) + " levels from " + String (lo, 1) + " to " + String (hi, 1) + " dBFS");
    return true;
}

bool measureSweep2D (Ctx& c)
{
    const double sr = c.sr();
    const float a = dbToGain (c.s.harmonicLevelDb);
    const double f1 = juce::jlimit (1.0, sr / 2 - 1, (double) c.s.sweep2DStart);
    const double f2 = juce::jlimit (f1 + 1, sr / 2, (double) c.s.sweep2DEnd);
    const double T = juce::jlimit (1.0, 30.0, (double) c.s.sweep2DSeconds);
    const int len = (int) (T * sr);

    auto in = makeStereo (len);
    auto* d = in.getWritePointer (0);
    const double k = std::log (f2 / f1);

    for (int i = 0; i < len; ++i)
    {
        const double t = i / sr;
        const double phase = c.s.sweep2DExponential
                                 ? kTwoPi * f1 * T / k * (std::exp (t * k / T) - 1.0)
                                 : kTwoPi * (f1 * t + (f2 - f1) * t * t / (2.0 * T));
        d[i] = a * (float) std::sin (phase);
    }
    duplicateLeft (in);

    AudioBuffer<float> out;
    if (! c.render (in, out, c.defaultFlush()))
        return false;

    const int frame = 4096;
    const int hop = juce::jmax (128, len / 700);
    const auto st = stftDb (out.getReadPointer (0), len, frame, hop, 16.0);

    Heatmap h;
    h.kind = "sweep2d";
    h.chain = c.chain;
    h.numX = st.numFrames;
    h.numY = st.numBins;
    h.db = st.db;
    h.xMin = (float) (frame * 0.5 / sr);
    h.xMax = (float) ((frame * 0.5 + (double) hop * juce::jmax (0, st.numFrames - 1)) / sr);
    h.yMin = 0.0f;
    h.yMax = (float) (sr / 2);
    c.out.heatmaps.push_back (std::move (h));
    return true;
}

//==============================================================================
// Oscilloscope
//==============================================================================
float waveSample (Waveform w, double phase01)
{
    switch (w)
    {
        case Waveform::Square:   return phase01 < 0.5 ? 1.0f : -1.0f;
        case Waveform::Saw:      return (float) (2.0 * phase01 - 1.0);
        case Waveform::Triangle: return (float) (phase01 < 0.5 ? 4.0 * phase01 - 1.0 : 3.0 - 4.0 * phase01);
        case Waveform::Sine:
        default:                 return (float) std::sin (kTwoPi * phase01);
    }
}

bool measureOscilloscope (Ctx& c)
{
    const double sr = c.sr();
    const double f = juce::jlimit (1.0, sr / 4, (double) c.s.oscFreq);
    const float a = dbToGain (c.s.oscLevelDb);
    const double period = sr / f;
    const bool shaper = c.s.oscWaveshaper;
    const auto wave = shaper ? Waveform::Sine : c.s.oscWaveform;

    const int warm = (int) juce::jmax (sr * 0.1, 4.0 * period);
    const int show = (int) juce::jmin (sr * 0.5, std::ceil ((shaper ? 2.0 : 3.0) * period));
    const int extra = std::abs (c.s.oscDelaySamples);
    const int len = warm + show + extra;

    auto in = makeStereo (len);
    auto* d = in.getWritePointer (0);
    for (int i = 0; i < len; ++i)
    {
        const double ph = std::fmod ((double) i * f / sr, 1.0);
        d[i] = a * waveSample (wave, ph);
    }
    duplicateLeft (in);

    AudioBuffer<float> out;
    if (! c.render (in, out, c.defaultFlush()))
        return false;

    if (! shaper)
    {
        if (c.chain == 0 || c.chain == 2)
        {
            std::vector<float> x ((size_t) show), y ((size_t) show);
            for (int i = 0; i < show; ++i)
            {
                x[(size_t) i] = (float) (1000.0 * i / sr);
                y[(size_t) i] = d[warm + i];
            }
            c.addCurve ("input", 0, std::move (x), std::move (y));
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            std::vector<float> x ((size_t) show), y ((size_t) show);
            const auto* o = out.getReadPointer (ch);
            for (int i = 0; i < show; ++i)
            {
                x[(size_t) i] = (float) (1000.0 * i / sr);
                y[(size_t) i] = o[warm + i];
            }
            c.addCurve ("time", ch, std::move (x), std::move (y));
        }
    }
    else
    {
        const int delay = c.s.oscDelaySamples;

        for (int ch = 0; ch < 2; ++ch)
        {
            std::vector<float> x, y;
            const auto* o = out.getReadPointer (ch);

            for (int i = 0; i < show; ++i)
            {
                const int j = warm + i + delay;
                if (j >= 0 && j < len)
                {
                    x.push_back (d[warm + i]);
                    y.push_back (o[j]);
                }
            }
            c.addCurve ("shape", ch, std::move (x), std::move (y));
        }
    }

    return true;
}

//==============================================================================
// Dynamics
//==============================================================================
bool measureRamp (Ctx& c)
{
    const double sr = c.sr();
    const double f = juce::jlimit (10.0, sr / 4, (double) c.s.dynamicsFreq);
    const float step = juce::jlimit (0.1f, 20.0f, c.s.rampStepDb);
    float lo = juce::jmin (c.s.rampStartDb, c.s.rampEndDb);
    float hi = juce::jmax (c.s.rampStartDb, c.s.rampEndDb);
    lo = juce::jmax (lo, -200.0f);

    std::vector<float> levels;
    for (float v = lo; v <= hi + 1.0e-3f; v += step)
        levels.push_back (v);

    if (levels.size() < 2)
        return true;

    // Up then back down (shows hysteresis); the top level is shared.
    std::vector<float> sequence = levels;
    for (int i = (int) levels.size() - 2; i >= 0; --i)
        sequence.push_back (levels[(size_t) i]);

    const int seg = (int) juce::jmax (sr * (double) c.s.rampSegmentSeconds, 4.0 * sr / f);
    const int len = seg * (int) sequence.size();

    auto in = makeStereo (len);
    auto* d = in.getWritePointer (0);
    for (int i = 0; i < len; ++i)
        d[i] = dbToGain (sequence[(size_t) (i / seg)]) * (float) std::sin (kTwoPi * f * i / sr);
    duplicateLeft (in);

    AudioBuffer<float> out;
    if (! c.render (in, out, c.defaultFlush()))
        return false;

    const int upCount = (int) levels.size();

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto* o = out.getReadPointer (ch);
        std::vector<float> ux, uy, dx, dy;

        for (size_t s = 0; s < sequence.size(); ++s)
        {
            // Steady-state peak: last quarter of the segment
            const int start = (int) s * seg + (seg * 3) / 4;
            const int end = ((int) s + 1) * seg;
            float peak = 0.0f;
            for (int i = start; i < end; ++i)
                peak = juce::jmax (peak, std::abs (o[i]));

            if ((int) s < upCount)
            {
                ux.push_back (sequence[s]);
                uy.push_back (gainToDb (peak, -200.0f));
            }
            else
            {
                dx.push_back (sequence[s]);
                dy.push_back (gainToDb (peak, -200.0f));
            }
        }

        if (! dx.empty())  // include the shared top point so the down curve connects
        {
            dx.insert (dx.begin(), ux.back());
            dy.insert (dy.begin(), uy.back());
        }

        c.addCurve ("ramp_up", ch, std::move (ux), std::move (uy));
        c.addCurve ("ramp_down", ch, std::move (dx), std::move (dy));
    }

    return true;
}

bool measureAttackRelease (Ctx& c)
{
    const double sr = c.sr();
    const double f = juce::jlimit (10.0, sr / 4, (double) c.s.dynamicsFreq);
    int segLen[3], segStart[3];
    int len = 0;

    for (int s = 0; s < 3; ++s)
    {
        segStart[s] = len;
        segLen[s] = (int) (juce::jlimit (0.01f, 10.0f, c.s.arSeconds[s]) * sr);
        len += segLen[s];
    }

    auto in = makeStereo (len);
    auto* d = in.getWritePointer (0);
    for (int s = 0; s < 3; ++s)
    {
        const float amp = dbToGain (c.s.arLevelsDb[s]);
        for (int i = segStart[s]; i < segStart[s] + segLen[s]; ++i)
            d[i] = amp * (float) std::sin (kTwoPi * f * i / sr);
    }
    duplicateLeft (in);

    AudioBuffer<float> out;
    if (! c.render (in, out, c.defaultFlush()))
        return false;

    // Peak envelope over windows of two periods
    const int win = juce::jmax (8, (int) std::ceil (2.0 * sr / f));
    const int numWin = len / win;

    auto envelope = [&] (const float* data, std::vector<float>& x, std::vector<float>& y)
    {
        x.resize ((size_t) numWin);
        y.resize ((size_t) numWin);
        for (int w = 0; w < numWin; ++w)
        {
            float peak = 0.0f;
            for (int i = w * win; i < (w + 1) * win; ++i)
                peak = juce::jmax (peak, std::abs (data[i]));
            x[(size_t) w] = (float) ((w + 0.5) * win / sr);
            y[(size_t) w] = gainToDb (peak, -200.0f);
        }
    };

    if (c.chain == 0 || c.chain == 2)
    {
        std::vector<float> x, y;
        envelope (d, x, y);
        c.addCurve ("env_in", 0, std::move (x), std::move (y));
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        std::vector<float> ex, ey;
        envelope (out.getReadPointer (ch), ex, ey);

        // Estimate attack / release as the time to cover 63% of the gain change (in dB)
        auto gainAt = [&] (int w, int seg) { return ey[(size_t) w] - c.s.arLevelsDb[seg]; };
        auto timeConstant = [&] (int seg) -> double
        {
            const int w0 = segStart[seg] / win + 1;
            const int w1 = (segStart[seg] + segLen[seg]) / win - 1;
            if (w1 - w0 < 4)
                return -1.0;

            const float g0 = gainAt (w0, seg);
            float gEnd = 0.0f;
            const int tailCount = juce::jmax (1, (w1 - w0) / 10);
            for (int w = w1 - tailCount + 1; w <= w1; ++w)
                gEnd += gainAt (w, seg);
            gEnd /= (float) tailCount;

            if (std::abs (gEnd - g0) < 0.5f)
                return -1.0;

            const float target = g0 + 0.632f * (gEnd - g0);
            for (int w = w0; w <= w1; ++w)
                if ((gEnd < g0 && gainAt (w, seg) <= target) || (gEnd > g0 && gainAt (w, seg) >= target))
                    return (w - w0 + 1) * (double) win / sr;

            return -1.0;
        };

        const double att = timeConstant (1), rel = timeConstant (2);
        String line = c.prefix (ch);
        line << "  attack " << (att > 0 ? "~" + String (att * 1000.0, 1) + " ms" : String ("n/a"))
             << "   release " << (rel > 0 ? "~" + String (rel * 1000.0, 1) + " ms" : String ("n/a"))
             << "   (63% of gain change)";
        c.out.info.add (line);

        c.addCurve ("env", ch, std::move (ex), std::move (ey));

        std::vector<float> wx, wy;
        minMaxDecimate (out.getReadPointer (ch), len, 0.0, 1.0 / sr, 3000, wx, wy);
        c.addCurve ("wave", ch, std::move (wx), std::move (wy));
    }

    return true;
}

//==============================================================================
// Hammerstein model from a synchronized exponential swept sine (Novak et al.)
//==============================================================================
double binomial (int n, int k)
{
    double r = 1.0;
    for (int i = 1; i <= k; ++i)
        r = r * (n - k + i) / i;
    return r;
}

/** Complex weight of sin(m*phi) inside sin^n(phi), relative to a unit sin(m*phi). */
std::complex<double> powerToHarmonic (int n, int m)
{
    if (m > n || ((n - m) & 1) != 0)
        return {};

    const std::complex<double> twoJ (0.0, 2.0);
    const double sign = (((n - m) / 2) & 1) ? -1.0 : 1.0;
    return twoJ * std::pow (twoJ, -n) * binomial (n, (n + m) / 2) * sign;
}

bool measureHammerstein (Ctx& c)
{
    const double sr = c.sr();
    const int order = juce::jlimit (1, 7, c.s.hammersteinOrder);
    const float a = dbToGain (c.s.hammersteinLevelDb);
    const double f1 = juce::jlimit (1.0, sr / 8, (double) c.s.hammersteinStart);
    const double f2 = juce::jlimit (f1 * 2, sr / 2, (double) c.s.hammersteinEnd);
    const double T0 = juce::jlimit (0.5, 30.0, (double) c.s.hammersteinSeconds);

    // Synchronisation: f1 * L must be an integer so that sin(m*phi(t)) == sin(phi(t + L ln m)).
    const double L = juce::jmax (1.0, std::round (f1 * T0 / std::log (f2 / f1))) / f1;
    const double T = L * std::log (f2 / f1);
    const int len = (int) (T * sr);
    const int tail = (int) (0.5 * sr);
    const int total = len + tail;

    auto in = makeStereo (total);
    auto* d = in.getWritePointer (0);
    const int fade = juce::jmin (len / 20, (int) (0.01 * sr));
    // Fade in over the first octave (f1 -> 2 f1). A hard start gives every harmonic a brick-wall
    // edge at m * f1, whose time-domain ringing leaks into the neighbouring harmonic windows.
    // The analysed band therefore starts at 2 * order * f1.
    const int fadeIn = juce::jmin (len / 4, (int) (L * std::log (2.0) * sr));

    for (int i = 0; i < len; ++i)
    {
        const double t = i / sr;
        float g = a;
        if (i < fadeIn)
            g *= 0.5f - 0.5f * std::cos ((float) kPi * (float) i / (float) fadeIn);
        if (i >= len - fade)
            g *= 0.5f * (1.0f + std::cos ((float) kPi * (float) (i - (len - fade)) / (float) fade));
        d[i] = g * (float) std::sin (kTwoPi * f1 * L * (std::exp (t / L) - 1.0));
    }
    duplicateLeft (in);

    AudioBuffer<float> out;
    if (! c.render (in, out, c.defaultFlush()))
        return false;

    const int P = nextPow2 (2 * total);
    const auto X = fftReal (d, total, P);
    const auto Y = fftReal (out.getReadPointer (0), total, P);

    float maxX = 0.0f;
    for (int k = 0; k <= P / 2; ++k)
        maxX = juce::jmax (maxX, std::norm (X[(size_t) k]));
    const float eps = maxX * 1.0e-9f;

    std::vector<cpx> H ((size_t) P);
    const int kLo = (int) std::ceil (f1 * P / sr), kHi = (int) std::floor (f2 * P / sr);
    // Smooth (raised-cosine) band edges: a brick-wall edge would make the strong linear
    // response ring for a long time, also at negative times where the harmonic responses sit.
    auto edgeWeight = [&] (double f)
    {
        const double lowEnd = 2.0 * f1, highStart = f2 / 1.25;
        if (f <= f1 || f >= f2) return 0.0;
        if (f < lowEnd)    return 0.5 - 0.5 * std::cos (kPi * std::log (f / f1) / std::log (lowEnd / f1));
        if (f > highStart) return 0.5 + 0.5 * std::cos (kPi * std::log (f / highStart) / std::log (f2 / highStart));
        return 1.0;
    };

    for (int k = juce::jmax (1, kLo); k <= juce::jmin (P / 2 - 1, kHi); ++k)
    {
        const float w = (float) edgeWeight (k * sr / P);
        H[(size_t) k] = w * Y[(size_t) k] * std::conj (X[(size_t) k]) / (std::norm (X[(size_t) k]) + eps);
        H[(size_t) (P - k)] = std::conj (H[(size_t) k]);
    }

    const auto hc = fftComplex (H, true);

    // Cut out each harmonic impulse response. Harmonic m sits at t = -L ln(m).
    auto sepSamples = [&] (int m) { return L * std::log ((m + 1.0) / m) * sr; };
    const int Q = nextPow2 ((int) std::ceil (sepSamples (1)));
    const int numBins = Q / 2 + 1;
    std::vector<std::vector<cpx>> Hm ((size_t) order + 1);

    for (int m = 1; m <= order; ++m)
    {
        const double centre = m == 1 ? 0.0 : (double) P - L * std::log ((double) m) * sr;
        const double sep = sepSamples (m);
        const int pre = juce::jmin (Q / 4, (int) (0.1 * sep));
        const int post = juce::jmin (Q - pre, (int) (0.9 * sep));
        const int r = (int) std::lround (centre);
        const double frac = centre - r;

        std::vector<cpx> seg ((size_t) Q);
        for (int i = -pre; i < post; ++i)
        {
            float w = 1.0f;
            const int fadePre = juce::jmax (1, pre / 2), fadePost = juce::jmax (1, post / 5);
            if (i < -pre + fadePre)
                w = 0.5f - 0.5f * std::cos ((float) kPi * (float) (i + pre) / (float) fadePre);
            else if (i > post - fadePost)
                w = 0.5f - 0.5f * std::cos ((float) kPi * (float) (post - i) / (float) fadePost);

            const int idx = ((r + i) % P + P) % P;
            seg[(size_t) ((i + Q) % Q)] = cpx (hc[(size_t) idx].real() * w, 0.0f);
        }

        auto spec = fftComplex (seg, false);
        Hm[(size_t) m].resize ((size_t) numBins);
        for (int k = 0; k < numBins; ++k)
        {
            const double ang = kTwoPi * k * frac / Q;   // remove the fractional delay
            Hm[(size_t) m][(size_t) k] = spec[(size_t) k] * cpx ((float) std::cos (ang), (float) std::sin (ang));
        }
    }

    // H_m = sum_{n>=m} powerToHarmonic(n,m) * a^(n-1) * G_n  -> back-substitute for G_n
    std::vector<std::vector<std::complex<double>>> G ((size_t) order + 1,
                                                      std::vector<std::complex<double>> ((size_t) numBins));
    for (int k = 0; k < numBins; ++k)
    {
        for (int m = order; m >= 1; --m)
        {
            std::complex<double> acc (Hm[(size_t) m][(size_t) k].real(), Hm[(size_t) m][(size_t) k].imag());
            for (int n = m + 1; n <= order; ++n)
                acc -= powerToHarmonic (n, m) * std::pow ((double) a, n - 1) * G[(size_t) n][(size_t) k];

            G[(size_t) m][(size_t) k] = acc / (powerToHarmonic (m, m) * std::pow ((double) a, m - 1));
        }
    }

    const double binHz = sr / Q;
    String gLine = "[" + chainLabel (c.chain) + "]  Hammerstein |G_n| @1 kHz:";

    for (int n = 1; n <= order; ++n)
    {
        std::vector<float> gDb ((size_t) numBins), hDb ((size_t) numBins);
        for (int k = 0; k < numBins; ++k)
        {
            gDb[(size_t) k] = gainToDb ((float) std::abs (G[(size_t) n][(size_t) k]), -200.0f);
            hDb[(size_t) k] = gainToDb (std::abs (Hm[(size_t) n][(size_t) k]), -200.0f);
        }

        std::vector<float> x, y;
        logReduce (gDb, binHz, juce::jmax (2.0 * f1 * order, binHz), f2, 600, Reduce::Mean, x, y);
        c.addCurve ("G", n, x, y);

        logReduce (hDb, binHz, juce::jmax (2.0 * f1 * n, binHz), f2, 600, Reduce::Mean, x, y);
        c.addCurve ("H", n, x, y);

        const int k1k = juce::jlimit (0, numBins - 1, (int) std::lround (1000.0 / binHz));
        gLine << "  G" << n << " " << String (gDb[(size_t) k1k], 1);
    }

    c.out.info.add (gLine + " dB");
    return true;
}

//==============================================================================
// Reverb (long impulse response)
//==============================================================================
bool measureReverb (Ctx& c)
{
    const double sr = c.sr();
    const int len = (int) (juce::jlimit (1.0f, 30.0f, c.s.reverbSeconds) * sr);
    const float a = dbToGain (c.s.reverbLevelDb);

    auto in = makeStereo (len);
    in.setSample (0, 0, a);
    in.setSample (1, 0, a);

    AudioBuffer<float> out;
    if (! c.render (in, out, (int) sr))
        return false;

    for (int ch = 0; ch < 2; ++ch)
    {
        out.applyGain (ch, 0, len, 1.0f / a);
        const auto* ir = out.getReadPointer (ch);

        std::vector<float> x, y;
        minMaxDecimate (ir, len, 0.0, 1.0 / sr, 4000, x, y);
        c.addCurve ("ir", ch, std::move (x), std::move (y));

        // Peak envelope in dB (5 ms windows)
        const int win = juce::jmax (1, (int) (0.005 * sr));
        std::vector<float> ex, ey;
        for (int w = 0; (w + 1) * win <= len; ++w)
        {
            float peak = 0.0f;
            for (int i = w * win; i < (w + 1) * win; ++i)
                peak = juce::jmax (peak, std::abs (ir[i]));
            ex.push_back ((float) ((w + 0.5) * win / sr));
            ey.push_back (gainToDb (peak, -200.0f));
        }
        c.addCurve ("env", ch, std::move (ex), std::move (ey));

        // Schroeder backward integration -> energy decay curve
        std::vector<double> edc ((size_t) len);
        double acc = 0.0;
        for (int i = len - 1; i >= 0; --i)
        {
            acc += (double) ir[i] * ir[i];
            edc[(size_t) i] = acc;
        }

        std::vector<float> cx, cy;
        const double e0 = edc[0] > 0 ? edc[0] : 1.0;
        const int step = juce::jmax (1, len / 3000);
        for (int i = 0; i < len; i += step)
        {
            cx.push_back ((float) (i / sr));
            cy.push_back ((float) juce::jmax (-200.0, 10.0 * std::log10 (juce::jmax (1.0e-30, edc[(size_t) i] / e0))));
        }

        // Linear fit of the EDC between two levels -> decay rate
        auto decayTime = [&] (double fromDb, double toDb) -> double
        {
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            int n = 0;
            for (size_t i = 0; i < cx.size(); ++i)
            {
                if (cy[i] <= fromDb && cy[i] >= toDb)
                {
                    sx += cx[i]; sy += cy[i]; sxx += (double) cx[i] * cx[i]; sxy += (double) cx[i] * cy[i]; ++n;
                }
            }
            if (n < 8)
                return -1.0;
            const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
            return slope < 0 ? -60.0 / slope : -1.0;
        };

        const double t30 = decayTime (-5.0, -35.0), t20 = decayTime (-5.0, -25.0), edt = decayTime (0.0, -10.0);
        auto fmt = [] (double v) { return v > 0 ? String (v, 2) + " s" : String ("n/a"); };
        c.out.info.add (c.prefix (ch) + "  RT60 (T30) " + fmt (t30) + "   RT60 (T20) " + fmt (t20) + "   EDT " + fmt (edt));

        c.addCurve ("edc", ch, std::move (cx), std::move (cy));

        if (ch == 0)
        {
            const int frame = 2048;
            const int hop = juce::jmax (64, len / 700);
            const auto st = stftDb (ir, len, frame, hop, 12.0);

            Heatmap h;
            h.kind = "reverb2d";
            h.chain = c.chain;
            h.numX = st.numFrames;
            h.numY = st.numBins;
            h.db = st.db;
            h.xMin = (float) (frame * 0.5 / sr);
            h.xMax = (float) ((frame * 0.5 + (double) hop * juce::jmax (0, st.numFrames - 1)) / sr);
            h.yMin = 0.0f;
            h.yMax = (float) (sr / 2);
            c.out.heatmaps.push_back (std::move (h));
        }
    }

    return true;
}

//==============================================================================
// Performance
//==============================================================================
bool measurePerformance (Ctx& c)
{
    std::vector<float> x, y, ys;

    for (int bs = 16; bs <= 4096; bs *= 2)
    {
        const int blocks = juce::jmax (8, (int) (0.25 * c.sr() / bs));

        if (c.renderer.timeBlocks (bs, 4) < 0)       // warm-up
            return false;

        const double t = c.renderer.timeBlocks (bs, blocks);
        const double ts = c.renderer.timeBlocks (bs, blocks, true);
        if (t < 0 || ts < 0)
            return false;

        const double audioSeconds = (double) blocks * bs / c.sr();
        x.push_back ((float) bs);
        y.push_back ((float) (100.0 * t / audioSeconds));
        ys.push_back ((float) (100.0 * ts / audioSeconds));
    }

    auto line = [&] (const String& what, const std::vector<float>& v)
    {
        String l = "[" + chainLabel (c.chain) + "] " + what;
        for (size_t i = 0; i < x.size(); ++i)
            l << "  " << (int) x[i] << ": " << String (v[i], v[i] < 1.0f ? 3 : 2);
        c.out.info.add (l);
    };

    if (! y.empty())
    {
        line ("CPU % (signal): ", y);
        line ("CPU % (silence):", ys);
    }

    c.addCurve ("cpu", 0, x, std::move (y));
    c.addCurve ("cpu", 1, std::move (x), std::move (ys));
    return true;
}

//==============================================================================
// Specs: a datasheet of standard figures (AES17 / IEC 60268-style)
//==============================================================================
String dbOrFloor (double db, int decimals, const String& unit = " dB")
{
    return db <= -199.0 ? String ("< -200") + unit : String (db, decimals) + unit;
}

String ratioText (float ratio)
{
    return percentText (ratio) + "  (" + dbOrFloor (gainToDb (ratio, -200.0f), 1) + ")";
}

bool measureSpecs (Ctx& c)
{
    const int N = c.s.fftSize();
    const double sr = c.sr(), binHz = sr / N;
    const int m997 = oddBin (997.0, N, sr);
    const String f997 = String (m997 * binHz, 1) + " Hz";

    struct Row { String group, name, text, note; };
    std::vector<Row> rows;
    auto add = [&] (const String& g, const String& n, const String& t, const String& note = {}) { rows.push_back ({ g, n, t, note }); };
    auto raw = [&] (const AudioBuffer<float>& in, AudioBuffer<float>& out) { return c.renderer.render (in, out, c.defaultFlush()); };
    AudioBuffer<float> out;

    // ---- Tone at -20 dBFS: gain, L/R balance, THD+N
    const float a20 = dbToGain (-20.0f);
    if (! raw (makeBinSine (N, m997, a20), out)) return false;
    const auto AL20 = amplitudes (out.getReadPointer (0) + N, N);
    const auto AR20 = amplitudes (out.getReadPointer (1) + N, N);
    const auto thd20 = analyseThd (AL20, m997);
    const double gain20 = gainToDb (AL20[(size_t) m997] / a20, -200.0f);
    const double balance = gainToDb (AR20[(size_t) m997], -200.0f) - gainToDb (AL20[(size_t) m997], -200.0f);

    // ---- Impulse at -20 dBFS: latency check, frequency response
    auto imp = makeStereo (N);
    imp.setSample (0, 0, a20);
    imp.setSample (1, 0, a20);
    if (! raw (imp, out)) return false;

    const float* ir = out.getReadPointer (0);
    int peakIdx = 0;
    for (int i = 1; i < N; ++i)
        if (std::abs (ir[i]) > std::abs (ir[peakIdx]))
            peakIdx = i;
    const int peakOffset = peakIdx > N / 2 ? peakIdx - N : peakIdx;

    const auto H = fftReal (ir, N, N);
    std::vector<float> mag ((size_t) N / 2 + 1);
    for (int k = 0; k <= N / 2; ++k)
        mag[(size_t) k] = gainToDb (std::abs (H[(size_t) k]) / a20, -200.0f);

    const int k1k = juce::jlimit (1, N / 2, (int) std::lround (1000.0 / binHz));
    const int k20 = juce::jmax (1, (int) std::lround (20.0 / binHz));
    const int k20k = juce::jmin (N / 2, (int) std::lround (juce::jmin (20000.0, 0.4535 * sr) / binHz));
    float fMin = mag[(size_t) k20], fMax = fMin;
    for (int k = k20; k <= k20k; ++k)
    {
        fMin = juce::jmin (fMin, mag[(size_t) k]);
        fMax = juce::jmax (fMax, mag[(size_t) k]);
    }

    const float ref = mag[(size_t) k1k];
    int kLow = 0, kHigh = N / 2 + 1;
    for (int k = k1k; k >= 1; --k)
        if (mag[(size_t) k] < ref - 3.0f) { kLow = k; break; }
    for (int k = k1k; k <= N / 2; ++k)
        if (mag[(size_t) k] < ref - 3.0f) { kHigh = k; break; }

    auto hz = [] (double f) { return f >= 1000.0 ? String (f / 1000.0, 2) + " kHz" : String (f, 1) + " Hz"; };
    const String lowText = kLow == 0 ? String ("< ") + hz (binHz) : hz (kLow * binHz);
    const String highText = kHigh > N / 2 ? String ("> ") + hz (sr / 2 - binHz) : hz (kHigh * binHz);

    // ---- Silence in: DC offset and idle noise (unweighted / A-weighted)
    if (! raw (makeStereo (2 * N), out)) return false;
    const float* idle = out.getReadPointer (0) + N;
    double mean = 0.0;
    for (int i = 0; i < N; ++i)
        mean += idle[i];
    mean /= N;

    const auto window = hannWindow (N);
    double sumW2 = 0.0;
    for (auto w : window) sumW2 += (double) w * w;
    const auto X = fftWindowed (idle, window.data(), N, N);
    double ms = 0.0, msA = 0.0;
    for (int k = 1; k < N / 2; ++k)
    {
        const double p = std::norm (X[(size_t) k]);
        const double wA = aWeightGain (k * binHz);
        ms += p;
        msA += p * wA * wA;
    }
    ms *= 2.0 / (N * sumW2);
    msA *= 2.0 / (N * sumW2);
    auto dbfsFromMs = [] (double v) { return v > 0 ? juce::jmax (-200.0, 10.0 * std::log10 (2.0 * v)) : -200.0; };

    // ---- Tone at -1 dBFS: THD, THD+N, true peak
    const float a1 = dbToGain (-1.0f);
    if (! raw (makeBinSine (N, m997, a1), out)) return false;
    const auto thd1 = analyseThd (amplitudes (out.getReadPointer (0) + N, N), m997);
    float samplePeak = 0.0f;
    for (int i = 0; i < N; ++i)
        samplePeak = juce::jmax (samplePeak, std::abs (out.getSample (0, N + i)));
    const float tp = truePeak (out.getReadPointer (0) + N, N);

    // ---- Tone at -60 dBFS: AES17 dynamic range (A-weighted THD+N ratio + 60 dB)
    if (! raw (makeBinSine (N, m997, dbToGain (-60.0f)), out)) return false;
    const auto A60 = amplitudes (out.getReadPointer (0) + N, N);
    double residualA = 0.0;
    for (int k = 1; k < N / 2; ++k)
        if (k != m997)
        {
            const double wA = aWeightGain (k * binHz);
            residualA += (double) A60[(size_t) k] * A60[(size_t) k] * wA * wA;
        }
    const double fund60 = A60[(size_t) m997] * aWeightGain (m997 * binHz);
    const double dynamicRange = residualA > 0 && fund60 > 0 ? 60.0 + 20.0 * std::log10 (fund60 / std::sqrt (residualA)) : 999.0;

    // ---- SMPTE IMD at -6 dBFS combined peak
    const int mLow = juce::jmax (1, (int) std::lround (60.0 * N / sr));
    const int mHigh = juce::jlimit (mLow * 12, N / 2 - 1, (int) std::lround (7000.0 * N / sr));
    {
        auto in = makeStereo (2 * N);
        const float a = dbToGain (-6.0f);
        for (int i = 0; i < 2 * N; ++i)
        {
            const double ph = (double) (i % N) / (double) N;
            in.setSample (0, i, (float) (0.8 * a * std::sin (kTwoPi * mLow * ph) + 0.2 * a * std::sin (kTwoPi * mHigh * ph)));
        }
        duplicateLeft (in);
        if (! raw (in, out)) return false;
    }
    const auto Aimd = amplitudes (out.getReadPointer (0) + N, N);
    double side = 0.0;
    for (int n = 1; n <= 10; ++n)
        for (int k : { mHigh - n * mLow, mHigh + n * mLow })
            if (k > 0 && k < N / 2)
                side += (double) Aimd[(size_t) k] * Aimd[(size_t) k];
    const float imd = Aimd[(size_t) mHigh] > 0 ? (float) (std::sqrt (side) / Aimd[(size_t) mHigh]) : 0.0f;

    // ---- High-frequency tones: non-harmonic content (aliasing + noise)
    float alias[2] {};
    const double aliasFreq[2] { 5000.0, juce::jmin (10000.0, sr * 0.2) };
    for (int i = 0; i < 2; ++i)
    {
        const int m = oddBin (aliasFreq[i], N, sr);
        if (! raw (makeBinSine (N, m, dbToGain (-6.0f)), out)) return false;
        alias[i] = analyseThd (amplitudes (out.getReadPointer (0) + N, N), m).nonHarmonic;
    }

    // ---- Crosstalk: impulse on one channel only
    double xtalk[2][2] {};   // [source][0 = 1 kHz, 1 = 10 kHz]
    for (int src = 0; src < 2; ++src)
    {
        auto one = makeStereo (N);
        one.setSample (src, 0, a20);
        if (! raw (one, out)) return false;
        const auto Hs = fftReal (out.getReadPointer (src), N, N);
        const auto Hx = fftReal (out.getReadPointer (1 - src), N, N);
        int idx = 0;
        for (double f : { 1000.0, juce::jmin (10000.0, sr * 0.4) })
        {
            const int k = (int) std::lround (f / binHz);
            const float d = std::abs (Hs[(size_t) k]);
            xtalk[src][idx++] = d > 1.0e-12f ? gainToDb (std::abs (Hx[(size_t) k]) / d, -200.0f) : -200.0;
        }
    }

    // ---- CPU at 512 samples: signal vs. silence after signal (denormal check)
    const int blocks = juce::jmax (8, (int) (0.5 * sr / 512));
    const double tSig = c.renderer.timeBlocks (512, blocks);
    const double tSil = c.renderer.timeBlocks (512, blocks, true);
    const double audioSec = (double) blocks * 512.0 / sr;

    // ---- Rows (order = display order)
    const int reported = c.renderer.reportedLatency();
    add ("Level & latency", "Gain @ " + f997 + ", -20 dBFS", dbOrFloor (gain20, 2));
    add ("Level & latency", "Reported latency", reported >= 0 ? String (reported) + " samples (" + String (1000.0 * reported / sr, 2) + " ms)" : String ("n/a"));
    add ("Level & latency", "Impulse peak after compensation",
         peakOffset == 0 ? String ("0 samples (latency report matches)") : String (peakOffset > 0 ? "+" : "") + String (peakOffset) + " samples",
         "Non-zero: latency misreported, or a linear-phase / look-ahead design");
    add ("Level & latency", "DC offset at output (silent input)", dbOrFloor (gainToDb ((float) std::abs (mean), -200.0f), 1, " dBFS"));

    add ("Frequency response", "Deviation 20 Hz - 20 kHz (-20 dBFS)", String (fMax - fMin, 2) + " dB  (" + String (fMin, 2) + " to " + String (fMax, 2) + " dB)");
    add ("Frequency response", "-3 dB points (re 1 kHz)", lowText + "  /  " + highText);

    add ("Distortion", "THD @ " + f997 + ", -1 dBFS", ratioText (thd1.thd));
    add ("Distortion", "THD+N @ " + f997 + ", -1 dBFS", ratioText (thd1.thdN));
    add ("Distortion", "THD+N @ " + f997 + ", -20 dBFS", ratioText (thd20.thdN));
    add ("Distortion", "IMD (SMPTE 60 Hz + 7 kHz 4:1, -6 dBFS)", ratioText (imd));
    add ("Distortion", "Non-harmonic @ " + hz (oddBin (aliasFreq[0], N, sr) * binHz) + ", -6 dBFS", dbOrFloor (gainToDb (alias[0], -200.0f), 1) + " re fundamental", "Aliasing + noise");
    add ("Distortion", "Non-harmonic @ " + hz (oddBin (aliasFreq[1], N, sr) * binHz) + ", -6 dBFS", dbOrFloor (gainToDb (alias[1], -200.0f), 1) + " re fundamental", "Aliasing + noise");

    add ("Noise", "Idle output noise (unweighted)", dbOrFloor (dbfsFromMs (ms), 1, " dBFS"));
    add ("Noise", "Idle output noise (A-weighted)", dbOrFloor (dbfsFromMs (msA), 1, " dBFS(A)"));
    add ("Noise", "Dynamic range (AES17, -60 dBFS, A-wtd)", dynamicRange > 300 ? String ("> 300 dB (no residual)") : String (dynamicRange, 1) + " dB");

    add ("Stereo", "L/R gain mismatch @ " + f997, String (balance, 3) + " dB");
    add ("Stereo", "Crosstalk L->R @ 1 kHz / 10 kHz", dbOrFloor (xtalk[0][0], 1) + "  /  " + dbOrFloor (xtalk[0][1], 1));
    add ("Stereo", "Crosstalk R->L @ 1 kHz / 10 kHz", dbOrFloor (xtalk[1][0], 1) + "  /  " + dbOrFloor (xtalk[1][1], 1));

    add ("Peak", "True peak @ " + f997 + ", -1 dBFS in", String (gainToDb (tp, -200.0f), 2) + " dBTP");
    add ("Peak", "Inter-sample overshoot (true - sample peak)", String (gainToDb (tp, -200.0f) - gainToDb (samplePeak, -200.0f), 2) + " dB", "4x band-limited reconstruction");

    if (tSig >= 0 && tSil >= 0)
    {
        const double pSig = 100.0 * tSig / audioSec, pSil = 100.0 * tSil / audioSec;
        add ("CPU (512-sample blocks)", "Signal", String (pSig, 3) + " % of real time");
        add ("CPU (512-sample blocks)", "Silence after signal", String (pSil, 3) + " %  (x" + String (pSig > 0 ? pSil / pSig : 0.0, 2) + ")",
             "Much higher than signal: likely denormal slowdown");
    }
    else
    {
        add ("CPU (512-sample blocks)", "Signal / silence", "n/a");
    }

    for (auto& r : rows)
    {
        SpecItem item;
        item.group = r.group;
        item.name = r.name;
        item.chain = c.chain;
        item.text = r.text;
        item.note = r.note;
        c.out.specs.push_back (std::move (item));
    }

    return true;
}

} // namespace

//==============================================================================
bool runMeasurement (const AnalysisSettings& settings, Renderer& renderer, int chainTag, AnalysisResult& result)
{
    Ctx c { settings, renderer, chainTag, result };

    switch (settings.mode)
    {
        case Mode::Linear:       return measureLinear (c);
        case Mode::Harmonic:
            switch (settings.harmonicMode)
            {
                case HarmonicMode::THD:     return measureThd (c);
                case HarmonicMode::IMD:     return measureImd (c);
                case HarmonicMode::Sweep1D: return measureSweep1D (c);
                case HarmonicMode::Sweep2D: return measureSweep2D (c);
                case HarmonicMode::ThdVsLevel: return measureThdVsLevel (c);
            }
            return false;
        case Mode::Oscilloscope: return measureOscilloscope (c);
        case Mode::Dynamics:     return settings.dynamicsMode == DynamicsMode::Ramp ? measureRamp (c)
                                                                                    : measureAttackRelease (c);
        case Mode::Hammerstein:  return measureHammerstein (c);
        case Mode::Reverb:       return measureReverb (c);
        case Mode::Performance:  return measurePerformance (c);
        case Mode::Specs:        return measureSpecs (c);
    }

    return false;
}

} // namespace pa
