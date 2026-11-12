// Headless checks of the measurement maths against processors with known behaviour.
// Run: ./PluginAnalyzerTests   (exit code 0 = all passed)

#include "analysis/Measurements.h"
#include "host/BuiltinProcessors.h"
#include "host/PluginChain.h"
#include "core/PlaybackRouter.h"

#include <juce_events/juce_events.h>
#include <iostream>

using namespace pa;

namespace
{

int failures = 0;

void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (! ok)
        ++failures;
}

bool near (double v, double target, double tol) { return std::abs (v - target) <= tol; }

/** Value of curve (kind, chain 0, channel) at the x closest to `x`. */
float valueAt (const AnalysisResult& r, const juce::String& kind, int channel, float x)
{
    for (auto& c : r.curves)
    {
        if (c.kind != kind || c.channel != channel || c.x.empty())
            continue;

        size_t best = 0;
        for (size_t i = 0; i < c.x.size(); ++i)
            if (std::abs (c.x[i] - x) < std::abs (c.x[best] - x))
                best = i;
        return c.y[best];
    }
    return std::numeric_limits<float>::quiet_NaN();
}

struct Rig
{
    PluginChain chain;
    AnalysisSettings settings;

    Rig() { chain.prepare (settings.sampleRate); }

    juce::AudioProcessor& add (int slot, const juce::String& id)
    {
        chain.setSlot (slot, createBuiltin (id), id, nullptr);
        return *chain.slot (slot).processor;
    }

    AnalysisResult run()
    {
        ChainRenderer renderer (chain, settings.blockSize, [] { return false; });
        AnalysisResult r;
        r.mode = settings.mode;
        const bool ok = runMeasurement (settings, renderer, 0, r);
        check (ok, "measurement completed");
        return r;
    }
};

/** Impulse through the router in 512-sample host blocks; returns (peak index, peak value). */
std::pair<int, float> routeImpulse (PlaybackRouter& router, int length = 4096)
{
    juce::AudioBuffer<float> buffer (2, length);
    buffer.clear();
    buffer.setSample (0, 0, 1.0f);
    buffer.setSample (1, 0, 1.0f);

    for (int pos = 0; pos < length; pos += 512)
    {
        juce::AudioBuffer<float> block (buffer.getArrayOfWritePointers(), 2, pos, juce::jmin (512, length - pos));
        router.process (block);
    }

    int peak = 0;
    for (int i = 1; i < length; ++i)
        if (std::abs (buffer.getSample (0, i)) > std::abs (buffer.getSample (0, peak)))
            peak = i;
    return { peak, buffer.getSample (0, peak) };
}

juce::String specText (const AnalysisResult& r, const juce::String& nameContains)
{
    for (auto& sp : r.specs)
        if (sp.name.contains (nameContains))
            return sp.text;
    return "<missing>";
}

void printSpecs (const AnalysisResult& r)
{
    for (auto& sp : r.specs)
        std::cout << "        " << sp.name.paddedRight (' ', 46) << sp.text << std::endl;
}

void printInfo (const AnalysisResult& r)
{
    for (auto& line : r.info)
        std::cout << "        " << line << std::endl;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::cout << "Linear: empty chain is flat" << std::endl;
    {
        Rig rig;
        auto r = rig.run();
        check (near (valueAt (r, "mag", 0, 100.0f), 0.0, 0.01), "0 dB at 100 Hz");
        check (near (valueAt (r, "mag", 0, 15000.0f), 0.0, 0.01), "0 dB at 15 kHz");
        check (near (valueAt (r, "phase", 0, 5000.0f), 0.0, 0.1), "0 deg phase at 5 kHz");
    }

    std::cout << "Linear: latency is compensated" << std::endl;
    {
        Rig rig;
        rig.add (0, "latency");
        rig.chain.prepare (rig.settings.sampleRate);
        auto r = rig.run();
        check (near (valueAt (r, "phase", 0, 5000.0f), 0.0, 0.1), "phase stays 0 with 256 samples latency");
    }

    std::cout << "Linear: peak EQ +6 dB @ 1 kHz (delta and noise)" << std::endl;
    {
        Rig rig;
        rig.add (0, "peakeq");
        auto r = rig.run();
        check (near (valueAt (r, "mag", 0, 1000.0f), 6.0, 0.05), "delta: +6 dB at 1 kHz");
        check (near (valueAt (r, "mag", 0, 50.0f), 0.0, 0.2), "delta: ~0 dB at 50 Hz");

        rig.settings.linearSignal = LinearSignal::Noise;
        rig.settings.linearLevelDb = -6.0f;
        r = rig.run();
        check (near (valueAt (r, "mag", 0, 1000.0f), 6.0, 0.1), "noise: +6 dB at 1 kHz");
    }

    std::cout << "Harmonic: THD of y = x + 0.1 x^3 at amplitude 0.5" << std::endl;
    {
        Rig rig;
        auto& p = rig.add (0, "polynomial");
        setBuiltinParameter (p, "a2", 0.0f);
        setBuiltinParameter (p, "a3", 0.1f);
        rig.settings.mode = Mode::Harmonic;
        rig.settings.harmonicMode = HarmonicMode::THD;
        rig.settings.harmonicLevelDb = juce::Decibels::gainToDecibels (0.5f);
        auto r = rig.run();
        printInfo (r);

        // H3 = a3 A^3 / 4, H1 = A + 3 a3 A^3 / 4
        const double h3 = 0.1 * 0.125 / 4.0, h1 = 0.5 + 3.0 * 0.1 * 0.125 / 4.0;
        const double expectedPct = 100.0 * h3 / h1;
        const auto thdText = r.info[0].fromFirstOccurrenceOf ("THD ", false, false).upToFirstOccurrenceOf ("%", false, false);
        check (near (thdText.getDoubleValue(), expectedPct, 0.001),
               "THD = " + thdText + "% (expected " + juce::String (expectedPct, 4) + "%)");
    }

    std::cout << "Harmonic: IMD of a linear gain is negligible" << std::endl;
    {
        Rig rig;
        rig.add (0, "gain");
        rig.settings.mode = Mode::Harmonic;
        rig.settings.harmonicMode = HarmonicMode::IMD;
        auto r = rig.run();
        printInfo (r);
        const auto dbText = r.info[0].fromLastOccurrenceOf ("(", false, false).upToFirstOccurrenceOf (" dB", false, false);
        check (dbText.getDoubleValue() < -100.0, "IMD below -100 dB");
    }

    std::cout << "Hammerstein: y = x + 0.2 x^2 + 0.1 x^3" << std::endl;
    {
        Rig rig;
        auto& p = rig.add (0, "polynomial");
        setBuiltinParameter (p, "a2", 0.2f);
        setBuiltinParameter (p, "a3", 0.1f);
        rig.settings.mode = Mode::Hammerstein;
        rig.settings.hammersteinOrder = 3;
        auto r = rig.run();
        printInfo (r);
        for (float f : { 500.0f, 2000.0f })
        {
            check (near (valueAt (r, "G", 1, f), 0.0, 0.3), "G1 ~ 0 dB @ " + juce::String (f));
            check (near (valueAt (r, "G", 2, f), 20.0 * std::log10 (0.2), 0.5), "G2 ~ -14 dB @ " + juce::String (f));
            check (near (valueAt (r, "G", 3, f), 20.0 * std::log10 (0.1), 0.5), "G3 ~ -20 dB @ " + juce::String (f));
        }
    }

    std::cout << "Hammerstein: polynomial -> peak EQ, order 5 (unused orders stay low)" << std::endl;
    {
        Rig rig;
        auto& p = rig.add (0, "polynomial");
        setBuiltinParameter (p, "a2", 0.2f);
        setBuiltinParameter (p, "a3", 0.1f);
        rig.add (1, "peakeq");
        rig.settings.mode = Mode::Hammerstein;
        rig.settings.hammersteinOrder = 5;
        auto r = rig.run();
        printInfo (r);
        for (float f : { 150.0f, 300.0f, 1000.0f, 5000.0f })
        {
            const float g4 = valueAt (r, "G", 4, f), g5 = valueAt (r, "G", 5, f);
            std::cout << "        f=" << f << "  G1 " << valueAt (r, "G", 1, f) << "  G3 " << valueAt (r, "G", 3, f)
                      << "  G4 " << g4 << "  G5 " << g5
                      << "   | H3 " << valueAt (r, "H", 3, f) << "  H4 " << valueAt (r, "H", 4, f) << "  H5 " << valueAt (r, "H", 5, f) << std::endl;
            check (g4 < -60.0f && g5 < -60.0f, "G4/G5 below -60 dB @ " + juce::String (f));
        }
        check (near (valueAt (r, "G", 1, 1000.0f), 6.0, 0.3), "G1 shows the EQ peak (+6 dB @ 1 kHz)");
    }

    std::cout << "Dynamics: compressor ramp (threshold -20, ratio 4)" << std::endl;
    {
        Rig rig;
        rig.add (0, "compressor");
        rig.settings.mode = Mode::Dynamics;
        rig.settings.dynamicsMode = DynamicsMode::Ramp;
        rig.settings.rampStartDb = -60.0f;
        rig.settings.rampStepDb = 2.0f;
        auto r = rig.run();
        check (near (valueAt (r, "ramp_up", 0, -40.0f), -40.0, 0.2), "unity below threshold");
        check (near (valueAt (r, "ramp_up", 0, 0.0f), -15.0, 0.5), "0 dB in -> -15 dB out");
    }

    std::cout << "Reverb: built-in reverb produces a decay" << std::endl;
    {
        Rig rig;
        rig.add (0, "reverb");
        rig.settings.mode = Mode::Reverb;
        rig.settings.reverbSeconds = 3.0f;
        auto r = rig.run();
        printInfo (r);
        check (r.info.size() > 0 && r.info[0].contains ("RT60 (T30)") && ! r.info[0].contains ("T30) n/a"), "RT60 estimated");
        check (! r.heatmaps.empty(), "waterfall data present");
    }

    std::cout << "Performance" << std::endl;
    {
        Rig rig;
        rig.add (0, "saturator");
        rig.settings.mode = Mode::Performance;
        auto r = rig.run();
        printInfo (r);
        check (! r.curves.empty() && r.curves[0].x.size() == 9, "9 block sizes timed");
    }

    std::cout << "Linear: group delay and crosstalk" << std::endl;
    {
        Rig rig;
        auto& p = rig.add (0, "latency");
        setBuiltinParameter (p, "report", 0.0f);      // 256 samples delay, NOT reported
        rig.run();                                    // first pass lets the processor publish its new latency
        auto r = rig.run();
        const double expectedMs = 1000.0 * 256.0 / rig.settings.sampleRate;
        check (near (valueAt (r, "gd", 0, 1000.0f), expectedMs, 0.01), "group delay = unreported 256 samples ("
               + juce::String (valueAt (r, "gd", 0, 1000.0f), 3) + " ms)");
        check (valueAt (r, "xtalk", 0, 1000.0f) <= -199.0f, "no crosstalk L->R in an independent-channel processor");
    }

    std::cout << "Harmonic: THD vs level (y = x + 0.1 x^3)" << std::endl;
    {
        Rig rig;
        auto& p = rig.add (0, "polynomial");
        setBuiltinParameter (p, "a2", 0.0f);
        setBuiltinParameter (p, "a3", 0.1f);
        rig.settings.mode = Mode::Harmonic;
        rig.settings.harmonicMode = HarmonicMode::ThdVsLevel;
        auto r = rig.run();
        const double expected = 20.0 * std::log10 ((0.1 * 0.125 / 4.0) / (0.5 + 3.0 * 0.1 * 0.125 / 4.0));
        const float got = valueAt (r, "lvl_thd", 0, -6.0f);
        check (near (got, expected, 0.1), "THD at -6 dBFS = " + juce::String (got, 2) + " dB (expected " + juce::String (expected, 2) + ")");
        check (valueAt (r, "lvl_thd", 0, -60.0f) < got - 80.0f, "THD falls ~2 dB per dB of level (cubic term)");
    }

    std::cout << "Harmonic: non-harmonic (aliasing) in sweep 1D" << std::endl;
    {
        Rig rig;
        rig.add (0, "hardclip");
        rig.settings.mode = Mode::Harmonic;
        rig.settings.harmonicMode = HarmonicMode::Sweep1D;
        rig.settings.sweepPoints = 24;
        auto r = rig.run();
        check (valueAt (r, "alias", 0, 8000.0f) > -90.0f, "hard clipper aliases at 8 kHz ("
               + juce::String (valueAt (r, "alias", 0, 8000.0f), 1) + " dB)");
    }

    std::cout << "Specs: empty chain" << std::endl;
    {
        Rig rig;
        rig.settings.mode = Mode::Specs;
        auto r = rig.run();
        printSpecs (r);
        check (specText (r, "Gain @").startsWith ("0.00 dB"), "gain 0.00 dB (never \"-0.00\"): " + specText (r, "Gain @"));
        check (specText (r, "Impulse peak").startsWith ("0 samples"), "latency check passes");
        check (specText (r, "Deviation").startsWith ("0.00 dB"), "flat response");
        check (specText (r, "Crosstalk L->R").startsWith ("< -200"), "no crosstalk");
    }

    std::cout << "Specs: misreported latency, saturator, reverb" << std::endl;
    {
        Rig rig;
        auto& p = rig.add (0, "latency");
        setBuiltinParameter (p, "report", 0.0f);
        rig.settings.mode = Mode::Specs;
        auto r = rig.run();
        check (specText (r, "Impulse peak").startsWith ("+256 samples"), "detects 256 samples of unreported latency: " + specText (r, "Impulse peak"));

        Rig sat;
        sat.add (0, "saturator");
        sat.settings.mode = Mode::Specs;
        r = sat.run();
        printSpecs (r);
        check (! specText (r, "THD @").startsWith ("<"), "saturator has measurable THD");

        Rig rev;
        rev.add (0, "reverb");
        rev.settings.mode = Mode::Specs;
        r = rev.run();
        check (! specText (r, "Crosstalk L->R").startsWith ("< -200"), "stereo reverb shows crosstalk: " + specText (r, "Crosstalk L->R"));
    }

    std::cout << "Performance: silence-after-signal curve present" << std::endl;
    {
        Rig rig;
        rig.add (0, "reverb");
        rig.settings.mode = Mode::Performance;
        auto r = rig.run();
        int cpuCurves = 0;
        for (auto& cv : r.curves) cpuCurves += cv.kind == "cpu" ? 1 : 0;
        check (cpuCurves == 2, "signal and silence curves");
    }

    std::cout << "Plugin playback: routing and latency alignment" << std::endl;
    for (int play : { -1, 0, 1 })
    {
        AnalysisEngine engine;   // not started: only its chains are used
        // chain 1: 256-sample latency (reported) + gain +6 dB;  chain 2: empty
        auto twinDelay = createBuiltin ("latency");
        auto twinGain = createBuiltin ("gain");
        setBuiltinParameter (*twinGain, "gain", 6.0f);
        engine.getChain (0).setSlot (0, createBuiltin ("latency"), "latency", nullptr, std::move (twinDelay));
        engine.getChain (0).setSlot (1, createBuiltin ("gain"), "gain", nullptr, std::move (twinGain));

        PlaybackRouter router (engine);
        router.prepare (48000.0, 512);
        router.updateLatency();
        // the latency processor publishes its latency during its first block: run once, then measure
        router.setPlayChain (0);
        routeImpulse (router);
        router.updateLatency();
        router.setPlayChain (play);

        const auto [index, value] = routeImpulse (router);
        const juce::String name = play < 0 ? "Off" : "Play " + juce::String (play + 1);
        check (router.getReportedLatency() == 256, name + ": reported latency 256 (got " + juce::String (router.getReportedLatency()) + ")");
        check (index == 256, name + ": output aligned at 256 samples (got " + juce::String (index) + ")");
        const double expected = play == 0 ? juce::Decibels::decibelsToGain (6.0) : 1.0;
        check (near (value, expected, 1.0e-4), name + ": gain " + juce::String (value, 4) + " (expected " + juce::String (expected, 4) + ")");
    }

    std::cout << "Plugin playback: parameter mirror and state copy" << std::endl;
    {
        auto source = createBuiltin ("peakeq");
        auto target = createBuiltin ("peakeq");
        {
            ParamMirror mirror (*source, *target);
            setBuiltinParameter (*source, "gain", -9.0f);
            setBuiltinParameter (*source, "freq", 3000.0f);
        }
        auto value = [] (juce::AudioProcessor& p, const juce::String& id)
        {
            for (auto* prm : p.getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
                    if (rp->getParameterID() == id)
                        return rp->convertFrom0to1 (rp->getValue());
            return -999.0f;
        };
        check (near (value (*target, "gain"), -9.0, 0.01), "gain mirrored to twin");
        check (near (value (*target, "freq"), 3000.0, 1.0), "frequency mirrored to twin");

        setBuiltinParameter (*source, "q", 4.0f);   // mirror is gone: only a state copy can sync this
        copyProcessorState (*source, *target);
        check (near (value (*target, "q"), 4.0, 0.01), "full state copy");
    }

    std::cout << (failures == 0 ? "\nAll checks passed." : "\nSome checks FAILED.") << std::endl;
    return failures == 0 ? 0 : 1;
}
