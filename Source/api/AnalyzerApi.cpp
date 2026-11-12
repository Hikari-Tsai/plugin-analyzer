#include "AnalyzerApi.h"
#include "analysis/Measurements.h"
#include "host/BuiltinProcessors.h"
#include "host/PluginChain.h"

#ifndef PLUGIN_ANALYZER_VERSION_STRING
 #define PLUGIN_ANALYZER_VERSION_STRING "dev"
#endif

namespace pa::api
{

using juce::String;
using juce::var;

namespace
{

//==============================================================================
// Small JSON helpers
//==============================================================================
var object() { return var (new juce::DynamicObject()); }

void put (var& o, const juce::Identifier& key, const var& value)
{
    o.getDynamicObject()->setProperty (key, value);
}

var errorResponse (const String& message)
{
    auto o = object();
    put (o, "error", message);
    return o;
}

/** Keeps `digits` significant digits so the JSON stays short without losing tiny values
    (e.g. THD of 1.2e-7 % stays 1.2e-7 instead of becoming 0). */
double tidy (double v, int digits = 6)
{
    if (! std::isfinite (v) || juce::exactlyEqual (v, 0.0))
        return 0.0;
    const int magnitude = (int) std::ceil (std::log10 (std::abs (v)));
    const double scale = std::pow (10.0, digits - magnitude);
    const double r = std::round (v * scale) / scale;
    return juce::exactlyEqual (r, 0.0) ? 0.0 : r;
}

String chainName (int chain) { return chainLabel (chain); }

String channelName (bool midSide, int channel)
{
    return channel < 0 ? String ("all") : channelLabel (midSide, channel);
}

//==============================================================================
// Modes
//==============================================================================
struct ModeDef
{
    const char* name;
    const char* description;
    std::function<void (AnalysisSettings&)> apply;
};

const std::vector<ModeDef>& modes()
{
    static const std::vector<ModeDef> list {
        { "linear", "Frequency response, phase, group delay, impulse response and L/R crosstalk (impulse or noise excitation)",
          [] (AnalysisSettings& s) { s.mode = Mode::Linear; } },
        { "thd", "Harmonic distortion of one sine: spectrum, THD, THD+N, H2-H9",
          [] (AnalysisSettings& s) { s.mode = Mode::Harmonic; s.harmonicMode = HarmonicMode::THD; } },
        { "imd", "SMPTE intermodulation distortion (60 Hz + 7 kHz, 4:1)",
          [] (AnalysisSettings& s) { s.mode = Mode::Harmonic; s.harmonicMode = HarmonicMode::IMD; } },
        { "sweep1d", "THD, fundamental gain and non-harmonic (aliasing) content versus frequency",
          [] (AnalysisSettings& s) { s.mode = Mode::Harmonic; s.harmonicMode = HarmonicMode::Sweep1D; } },
        { "sweep2d", "Time-frequency image of a sine sweep (shows harmonics and aliasing)",
          [] (AnalysisSettings& s) { s.mode = Mode::Harmonic; s.harmonicMode = HarmonicMode::Sweep2D; } },
        { "thd_vs_level", "THD+N, THD and gain versus input level",
          [] (AnalysisSettings& s) { s.mode = Mode::Harmonic; s.harmonicMode = HarmonicMode::ThdVsLevel; } },
        { "oscilloscope", "Output waveform over time for a sine/square/saw/triangle input",
          [] (AnalysisSettings& s) { s.mode = Mode::Oscilloscope; s.oscWaveshaper = false; } },
        { "waveshaper", "Static transfer curve: output versus input for a sine",
          [] (AnalysisSettings& s) { s.mode = Mode::Oscilloscope; s.oscWaveshaper = true; } },
        { "ramp", "Dynamics transfer curve (input level up, then down)",
          [] (AnalysisSettings& s) { s.mode = Mode::Dynamics; s.dynamicsMode = DynamicsMode::Ramp; } },
        { "attack_release", "Level envelope for a quiet-loud-quiet sine; attack and release times",
          [] (AnalysisSettings& s) { s.mode = Mode::Dynamics; s.dynamicsMode = DynamicsMode::AttackRelease; } },
        { "hammerstein", "Hammerstein model (per-order filters G1..Gn) from a synchronized swept sine",
          [] (AnalysisSettings& s) { s.mode = Mode::Hammerstein; } },
        { "reverb", "Long impulse response, energy decay, RT60 (T30/T20) and EDT",
          [] (AnalysisSettings& s) { s.mode = Mode::Reverb; } },
        { "performance", "CPU use versus block size, with signal and with silence after signal",
          [] (AnalysisSettings& s) { s.mode = Mode::Performance; } },
        { "specs", "Datasheet of standard figures (AES17 / IEC 60268 style)",
          [] (AnalysisSettings& s) { s.mode = Mode::Specs; } },
    };
    return list;
}

//==============================================================================
// Settings
//==============================================================================
struct SettingDef
{
    const char* key;
    const char* type;
    const char* unit;
    const char* help;
    std::function<var (const AnalysisSettings&)> get;
    std::function<bool (AnalysisSettings&, const var&, String&)> set;
};

bool toNumber (const var& v, double& out)
{
    if (v.isInt() || v.isInt64() || v.isDouble() || v.isBool()) { out = (double) v; return true; }
    if (v.isString() && v.toString().trim().containsOnly ("0123456789.-+eE")) { out = v.toString().getDoubleValue(); return true; }
    return false;
}

template <typename T>
SettingDef number (const char* key, const char* unit, const char* help, T AnalysisSettings::* member, double lo, double hi)
{
    return { key, std::is_integral_v<T> ? "integer" : "number", unit, help,
             [member] (const AnalysisSettings& s) { return var ((double) (s.*member)); },
             [member, lo, hi, key] (AnalysisSettings& s, const var& v, String& error)
             {
                 double d;
                 if (! toNumber (v, d) || d < lo || d > hi)
                 {
                     error = String (key) + " must be a number between " + String (lo) + " and " + String (hi);
                     return false;
                 }
                 s.*member = (T) (std::is_integral_v<T> ? std::lround (d) : d);
                 return true;
             } };
}

SettingDef boolean (const char* key, const char* help, bool AnalysisSettings::* member)
{
    return { key, "boolean", "", help,
             [member] (const AnalysisSettings& s) { return var (s.*member); },
             [member] (AnalysisSettings& s, const var& v, String&) { s.*member = (bool) v; return true; } };
}

template <typename E>
SettingDef choice (const char* key, const char* help, E AnalysisSettings::* member, juce::StringArray names)
{
    return { key, "string", "", help,
             [member, names] (const AnalysisSettings& s) { return var (names[(int) (s.*member)]); },
             [member, names, key] (AnalysisSettings& s, const var& v, String& error)
             {
                 const int i = names.indexOf (v.toString().trim().toLowerCase());
                 if (i < 0) { error = String (key) + " must be one of: " + names.joinIntoString (", "); return false; }
                 s.*member = (E) i;
                 return true;
             } };
}

SettingDef triple (const char* key, const char* unit, const char* help, float (AnalysisSettings::* member)[3], double lo, double hi)
{
    return { key, "array of 3 numbers", unit, help,
             [member] (const AnalysisSettings& s) { juce::Array<var> a; for (float f : s.*member) a.add ((double) f); return var (a); },
             [member, lo, hi, key] (AnalysisSettings& s, const var& v, String& error)
             {
                 if (! v.isArray() || v.size() != 3) { error = String (key) + " must be an array of 3 numbers"; return false; }
                 for (int i = 0; i < 3; ++i)
                 {
                     double d;
                     if (! toNumber (v[i], d) || d < lo || d > hi) { error = String (key) + " values must be between " + String (lo) + " and " + String (hi); return false; }
                     (s.*member)[i] = (float) d;
                 }
                 return true;
             } };
}

const std::vector<SettingDef>& settingDefs()
{
    static const std::vector<SettingDef> list {
        number ("sample_rate", "Hz", "Sample rate used for the measurement", &AnalysisSettings::sampleRate, 8000.0, 384000.0),
        number ("quality", "", "Analysis length: 0 = 16384, 1 = 32768, 2 = 65536 samples", &AnalysisSettings::quality, 0, 2),
        number ("block_size", "samples", "Processing block size", &AnalysisSettings::blockSize, 16, PluginChain::maxBlockSize),
        boolean ("mid_side", "Analyse Mid/Side instead of Left/Right", &AnalysisSettings::midSide),

        choice ("linear_signal", "linear: excitation", &AnalysisSettings::linearSignal, { "delta", "noise" }),
        number ("linear_level_db", "dBFS", "linear: excitation level", &AnalysisSettings::linearLevelDb, -100.0, 0.0),

        number ("harmonic_level_db", "dBFS", "thd/imd/sweep1d/sweep2d: test level", &AnalysisSettings::harmonicLevelDb, -140.0, 12.0),
        number ("harmonic_freq_hz", "Hz", "thd/thd_vs_level: test frequency", &AnalysisSettings::harmonicFreq, 10.0, 96000.0),
        number ("sweep_points", "", "sweep1d: number of frequencies", &AnalysisSettings::sweepPoints, 8, 256),
        number ("sweep2d_start_hz", "Hz", "sweep2d: start frequency", &AnalysisSettings::sweep2DStart, 1.0, 96000.0),
        number ("sweep2d_end_hz", "Hz", "sweep2d: end frequency", &AnalysisSettings::sweep2DEnd, 10.0, 192000.0),
        boolean ("sweep2d_exponential", "sweep2d: exponential (true) or linear (false) sweep", &AnalysisSettings::sweep2DExponential),
        number ("sweep2d_seconds", "s", "sweep2d: sweep length", &AnalysisSettings::sweep2DSeconds, 1.0, 30.0),
        number ("level_sweep_from_db", "dBFS", "thd_vs_level: lowest level", &AnalysisSettings::levelSweepFromDb, -140.0, 12.0),
        number ("level_sweep_to_db", "dBFS", "thd_vs_level: highest level", &AnalysisSettings::levelSweepToDb, -140.0, 12.0),
        number ("level_sweep_step_db", "dB", "thd_vs_level: level step", &AnalysisSettings::levelSweepStepDb, 0.5, 12.0),

        choice ("osc_waveform", "oscilloscope: input waveform", &AnalysisSettings::oscWaveform, { "sine", "square", "saw", "triangle" }),
        number ("osc_freq_hz", "Hz", "oscilloscope/waveshaper: frequency", &AnalysisSettings::oscFreq, 1.0, 20000.0),
        number ("osc_level_db", "dBFS", "oscilloscope/waveshaper: level", &AnalysisSettings::oscLevelDb, -100.0, 12.0),
        number ("osc_delay_samples", "samples", "waveshaper: extra delay between input and output", &AnalysisSettings::oscDelaySamples, -4096, 4096),

        number ("dynamics_freq_hz", "Hz", "ramp/attack_release: sine frequency", &AnalysisSettings::dynamicsFreq, 10.0, 20000.0),
        number ("ramp_start_db", "dBFS", "ramp: lowest level", &AnalysisSettings::rampStartDb, -200.0, 12.0),
        number ("ramp_end_db", "dBFS", "ramp: highest level", &AnalysisSettings::rampEndDb, -200.0, 12.0),
        number ("ramp_step_db", "dB", "ramp: level step", &AnalysisSettings::rampStepDb, 0.1, 20.0),
        number ("ramp_step_seconds", "s", "ramp: time per level (must exceed the plugin's attack/release)", &AnalysisSettings::rampSegmentSeconds, 0.01, 5.0),
        triple ("ar_levels_db", "dBFS", "attack_release: levels of the 3 segments", &AnalysisSettings::arLevelsDb, -200.0, 12.0),
        triple ("ar_seconds", "s", "attack_release: lengths of the 3 segments", &AnalysisSettings::arSeconds, 0.01, 10.0),

        number ("hammerstein_order", "", "hammerstein: highest order (1-7)", &AnalysisSettings::hammersteinOrder, 1, 7),
        number ("hammerstein_level_db", "dBFS", "hammerstein: sweep level", &AnalysisSettings::hammersteinLevelDb, -100.0, 12.0),
        number ("hammerstein_start_hz", "Hz", "hammerstein: sweep start frequency", &AnalysisSettings::hammersteinStart, 1.0, 20000.0),
        number ("hammerstein_end_hz", "Hz", "hammerstein: sweep end frequency", &AnalysisSettings::hammersteinEnd, 100.0, 192000.0),
        number ("hammerstein_seconds", "s", "hammerstein: sweep length", &AnalysisSettings::hammersteinSeconds, 0.5, 30.0),

        number ("reverb_seconds", "s", "reverb: impulse response length", &AnalysisSettings::reverbSeconds, 1.0, 30.0),
        number ("reverb_level_db", "dBFS", "reverb: impulse level", &AnalysisSettings::reverbLevelDb, -60.0, 0.0),
    };
    return list;
}

//==============================================================================
// Curves
//==============================================================================
enum class Keep { Middle, MaxValue, MaxMagnitude };   // which point represents a bucket when reducing
struct KindInfo { const char* xUnit; const char* yUnit; const char* description; Keep keep; };

KindInfo kindInfo (const String& kind)
{
    static const std::map<String, KindInfo> table {
        { "mag",         { "Hz", "dB", "Frequency response (magnitude)", Keep::Middle } },
        { "phase",       { "Hz", "deg", "Phase response", Keep::Middle } },
        { "gd",          { "Hz", "ms", "Group delay", Keep::Middle } },
        { "ir",          { "ms", "amplitude", "Impulse response", Keep::MaxMagnitude } },
        { "xtalk",       { "Hz", "dB", "Crosstalk from this channel into the other", Keep::Middle } },
        { "spectrum",    { "Hz", "dBFS", "Output spectrum", Keep::MaxValue } },
        { "fundamental", { "Hz", "dB", "Fundamental gain versus frequency", Keep::Middle } },
        { "thd",         { "Hz", "dB", "THD versus frequency", Keep::Middle } },
        { "alias",       { "Hz", "dB", "Non-harmonic content (aliasing + noise) versus frequency", Keep::Middle } },
        { "lvl_thdn",    { "dBFS", "dB", "THD+N versus input level", Keep::Middle } },
        { "lvl_thd",     { "dBFS", "dB", "THD versus input level", Keep::Middle } },
        { "lvl_gain",    { "dBFS", "dB", "Gain versus input level", Keep::Middle } },
        { "input",       { "ms", "amplitude", "Oscilloscope input", Keep::Middle } },
        { "time",        { "ms", "amplitude", "Oscilloscope output", Keep::Middle } },
        { "shape",       { "input amplitude", "output amplitude", "Transfer curve (output versus input)", Keep::Middle } },
        { "ramp_up",     { "dBFS in", "dBFS out", "Dynamics transfer curve, level rising", Keep::Middle } },
        { "ramp_down",   { "dBFS in", "dBFS out", "Dynamics transfer curve, level falling", Keep::Middle } },
        { "env",         { "s", "dBFS", "Level envelope", Keep::Middle } },
        { "env_in",      { "s", "dBFS", "Input level envelope", Keep::Middle } },
        { "wave",        { "s", "amplitude", "Output waveform (min/max)", Keep::MaxMagnitude } },
        { "G",           { "Hz", "dB", "Hammerstein filter for order n (channel = n)", Keep::Middle } },
        { "H",           { "Hz", "dB", "Harmonic response for order n (channel = n)", Keep::Middle } },
        { "edc",         { "s", "dB", "Energy decay curve (Schroeder)", Keep::Middle } },
        { "cpu",         { "block size (samples)", "% of real time", "CPU use (channel 0 = signal, 1 = silence after signal)", Keep::Middle } },
    };

    auto it = table.find (kind);
    return it != table.end() ? it->second : KindInfo { "", "", "", Keep::Middle };
}

var curveToJson (const Curve& c, bool midSide, int maxPoints, const String& mode)
{
    const auto info = kindInfo (c.kind);
    auto o = object();
    put (o, "kind", c.kind);
    put (o, "description", info.description);
    put (o, "chain", chainName (c.chain));

    const bool orderCurve = c.kind == "G" || c.kind == "H";
    const bool cpuCurve = c.kind == "cpu";
    if (orderCurve)       put (o, "order", c.channel);
    else if (cpuCurve)    put (o, "input", c.channel == 0 ? "signal" : "silence_after_signal");
    else                  put (o, "channel", channelName (midSide, c.channel));

    // Reverb's "ir" is in seconds, the linear one in ms.
    put (o, "x_unit", (c.kind == "ir" && mode == "reverb") || (c.kind == "env" && mode == "reverb") ? "s" : info.xUnit);
    put (o, "y_unit", info.yUnit);

    const size_t n = juce::jmin (c.x.size(), c.y.size());
    if (n > 0)
    {
        size_t iMax = 0, iMin = 0;
        for (size_t i = 1; i < n; ++i)
        {
            if (c.y[i] > c.y[iMax]) iMax = i;
            if (c.y[i] < c.y[iMin]) iMin = i;
        }
        put (o, "y_max", tidy (c.y[iMax]));
        put (o, "x_at_y_max", tidy (c.x[iMax]));
        put (o, "y_min", tidy (c.y[iMin]));
        put (o, "x_at_y_min", tidy (c.x[iMin]));
    }

    if (maxPoints > 0 && n > 0)
    {
        juce::Array<var> points;
        const size_t buckets = juce::jmin (n, (size_t) maxPoints);

        for (size_t b = 0; b < buckets; ++b)
        {
            const size_t s = n * b / buckets, e = juce::jmax (s + 1, n * (b + 1) / buckets);
            size_t pick = (s + e - 1) / 2;
            // Spectra keep their highest value and waveforms their largest excursion, so peaks survive.
            for (size_t i = s; i < e; ++i)
            {
                if (info.keep == Keep::MaxValue && c.y[i] > c.y[pick]) pick = i;
                if (info.keep == Keep::MaxMagnitude && std::abs (c.y[i]) > std::abs (c.y[pick])) pick = i;
            }

            juce::Array<var> p;
            p.add (tidy (c.x[pick], 4));
            p.add (tidy (c.y[pick], 4));
            points.add (var (p));
        }

        put (o, "points", var (points));
    }

    return o;
}

//==============================================================================
// Plugins
//==============================================================================
juce::AudioPluginFormatManager& formatManager()
{
    static juce::AudioPluginFormatManager fm;
    static bool initialised = false;
    if (! initialised)
    {
        fm.addDefaultFormats();
        initialised = true;
    }
    return fm;
}

struct Loaded
{
    std::unique_ptr<juce::AudioProcessor> processor;
    var info;
};

Loaded loadProcessor (const String& spec, double sampleRate, String& error)
{
    Loaded result;
    auto info = object();

    if (spec.startsWithIgnoreCase ("builtin:"))
    {
        const auto id = spec.fromFirstOccurrenceOf (":", false, false).trim().toLowerCase();
        result.processor = createBuiltin (id);
        if (result.processor == nullptr)
        {
            juce::StringArray ids;
            for (auto& b : getBuiltinList()) ids.add ("builtin:" + b.id);
            error = "Unknown built-in processor '" + id + "'. Available: " + ids.joinIntoString (", ");
            return {};
        }
        put (info, "plugin", spec);
        put (info, "name", result.processor->getName());
        put (info, "manufacturer", "built-in");
        put (info, "format", "built-in");
        result.info = info;
        return result;
    }

    const juce::File file = juce::File::getCurrentWorkingDirectory().getChildFile (spec);
    if (! file.exists())
    {
        error = "Plugin not found: " + file.getFullPathName() + " (use a .vst3 / .component path or builtin:<id>)";
        return {};
    }

    for (auto* format : formatManager().getFormats())
    {
        if (! format->fileMightContainThisPluginType (file.getFullPathName()))
            continue;

        juce::OwnedArray<juce::PluginDescription> types;
        format->findAllTypesForFile (types, file.getFullPathName());
        if (types.isEmpty())
            continue;

        String loadError;
        auto instance = formatManager().createPluginInstance (*types[0], sampleRate, PluginChain::maxBlockSize, loadError);
        if (instance == nullptr)
        {
            error = "Could not load " + file.getFullPathName() + ": " + loadError;
            return {};
        }

        put (info, "plugin", file.getFullPathName());
        put (info, "name", types[0]->name);
        put (info, "manufacturer", types[0]->manufacturerName);
        put (info, "format", types[0]->pluginFormatName);
        put (info, "version", types[0]->version);
        result.processor = std::move (instance);
        result.info = info;
        return result;
    }

    error = "No VST3 or AU plugin found in " + file.getFullPathName();
    return {};
}

juce::AudioProcessorParameter* findParameter (juce::AudioProcessor& p, const String& key)
{
    for (auto* param : p.getParameters())
    {
        String id;
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param))
            id = ranged->getParameterID();
        else if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (param))
            id = hosted->getParameterID();

        if (id.equalsIgnoreCase (key) || param->getName (256).equalsIgnoreCase (key))
            return param;
    }
    return nullptr;
}

String parameterId (juce::AudioProcessorParameter& param)
{
    if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (&param))
        return ranged->getParameterID();
    if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (&param))
        return hosted->getParameterID();
    return String (param.getParameterIndex());
}

String parameterText (juce::AudioProcessorParameter& param)
{
    const auto label = param.getLabel().trim();
    return param.getCurrentValueAsText().trim() + (label.isNotEmpty() ? " " + label : String());
}

/** Applies { "name": value } to a processor; returns what each parameter ended up at. */
bool applyParams (juce::AudioProcessor& p, const var& params, var& applied, String& error)
{
    applied = object();
    if (params.isVoid())
        return true;

    auto* obj = params.getDynamicObject();
    if (obj == nullptr)
    {
        error = "\"params\" must be an object of { \"parameter name or id\": value }";
        return false;
    }

    for (auto& prop : obj->getProperties())
    {
        auto* param = findParameter (p, prop.name.toString());
        if (param == nullptr)
        {
            juce::StringArray names;
            for (auto* q : p.getParameters()) names.add (q->getName (64));
            error = "Unknown parameter '" + prop.name.toString() + "' for " + p.getName() + ". Parameters: " + names.joinIntoString (", ");
            return false;
        }

        float normalized;
        if (auto* o = prop.value.getDynamicObject(); o != nullptr && o->hasProperty ("normalized"))
            normalized = juce::jlimit (0.0f, 1.0f, (float) (double) o->getProperty ("normalized"));
        else
            normalized = param->getValueForText (prop.value.toString());

        param->setValueNotifyingHost (normalized);
        put (applied, prop.name, parameterText (*param));
    }

    return true;
}

var parameterList (juce::AudioProcessor& p)
{
    juce::Array<var> list;
    for (auto* param : p.getParameters())
    {
        auto o = object();
        put (o, "index", param->getParameterIndex());
        put (o, "id", parameterId (*param));
        put (o, "name", param->getName (256));
        put (o, "value", parameterText (*param));
        put (o, "normalized", tidy (param->getValue(), 4));
        if (param->getNumSteps() > 0 && param->getNumSteps() < 64)
            put (o, "steps", param->getNumSteps());
        list.add (o);
    }
    return list;
}

/** Runs a few silent blocks so processors that publish their latency while processing do so. */
void warmUp (PluginChain& chain)
{
    juce::AudioBuffer<float> block (2, 512);
    for (int i = 0; i < 4; ++i)
    {
        block.clear();
        chain.process (block, 512);
    }
}

} // namespace

//==============================================================================
String toJson (const var& value, bool pretty)
{
    return juce::JSON::toString (value, ! pretty, 6);
}

bool applyMode (AnalysisSettings& settings, const String& mode, String& error)
{
    for (auto& m : modes())
    {
        if (mode.trim().equalsIgnoreCase (m.name))
        {
            m.apply (settings);
            return true;
        }
    }

    juce::StringArray names;
    for (auto& m : modes()) names.add (m.name);
    error = "Unknown mode '" + mode + "'. Modes: " + names.joinIntoString (", ");
    return false;
}

bool applySettings (AnalysisSettings& settings, const var& json, String& error)
{
    if (json.isVoid())
        return true;

    auto* obj = json.getDynamicObject();
    if (obj == nullptr)
    {
        error = "\"settings\" must be an object";
        return false;
    }

    for (auto& prop : obj->getProperties())
    {
        const auto key = prop.name.toString();
        auto def = std::find_if (settingDefs().begin(), settingDefs().end(), [&] (const SettingDef& d) { return key == d.key; });
        if (def == settingDefs().end())
        {
            error = "Unknown setting '" + key + "' (see 'describe' for the list)";
            return false;
        }
        if (! def->set (settings, prop.value, error))
            return false;
    }

    return true;
}

var resultToJson (const AnalysisResult& r, int maxPoints)
{
    auto o = object();

    juce::Array<var> metrics;
    for (auto& m : r.metrics)
    {
        auto mo = object();
        put (mo, "name", m.name);
        put (mo, "chain", chainName (m.chain));
        put (mo, "channel", channelName (r.midSide, m.channel));
        put (mo, "value", tidy (m.value));
        put (mo, "unit", m.unit);
        metrics.add (mo);
    }
    put (o, "metrics", var (metrics));

    juce::Array<var> specs;
    for (auto& s : r.specs)
    {
        auto so = object();
        put (so, "group", s.group);
        put (so, "name", s.name);
        put (so, "chain", chainName (s.chain));
        put (so, "value", s.text);
        if (s.note.isNotEmpty()) put (so, "note", s.note);
        specs.add (so);
    }
    if (! specs.isEmpty())
        put (o, "specs", var (specs));

    juce::StringArray info;
    for (auto& line : r.info) info.add (line.trim());
    if (! info.isEmpty())
        put (o, "info", var (info));

    if (maxPoints > 0)
    {
        juce::Array<var> curves;
        for (auto& c : r.curves)
            curves.add (curveToJson (c, r.midSide, maxPoints, r.mode == Mode::Reverb ? "reverb" : ""));
        put (o, "curves", var (curves));
    }

    juce::Array<var> maps;
    for (auto& h : r.heatmaps)
    {
        auto ho = object();
        put (ho, "kind", h.kind);
        put (ho, "chain", chainName (h.chain));
        put (ho, "frames", h.numX);
        put (ho, "bins", h.numY);
        put (ho, "time_s", tidy (h.xMax, 3));
        put (ho, "max_frequency_hz", tidy (h.yMax, 1));
        put (ho, "note", "Image data is not included; view it in the app.");
        maps.add (ho);
    }
    if (! maps.isEmpty())
        put (o, "heatmaps", var (maps));

    return o;
}

var describe()
{
    auto o = object();
    put (o, "name", "plugin-analyzer");
    put (o, "version", PLUGIN_ANALYZER_VERSION_STRING);

    juce::Array<var> modeList;
    for (auto& m : modes())
    {
        auto mo = object();
        put (mo, "name", m.name);
        put (mo, "description", m.description);
        modeList.add (mo);
    }
    put (o, "modes", var (modeList));

    const AnalysisSettings defaults;
    juce::Array<var> settingList;
    for (auto& d : settingDefs())
    {
        auto so = object();
        put (so, "key", d.key);
        put (so, "type", d.type);
        if (*d.unit) put (so, "unit", d.unit);
        put (so, "default", d.get (defaults));
        put (so, "description", d.help);
        settingList.add (so);
    }
    put (o, "settings", var (settingList));

    juce::Array<var> builtins;
    for (auto& b : getBuiltinList())
    {
        auto bo = object();
        put (bo, "plugin", "builtin:" + b.id);
        put (bo, "name", b.name);
        builtins.add (bo);
    }
    put (o, "builtins", var (builtins));
    put (o, "plugin_spec", "\"builtin:<id>\" or a path to a .vst3 / .component file");
    return o;
}

var pluginInfo (const String& spec)
{
    String error;
    auto loaded = loadProcessor (spec, 48000.0, error);
    if (loaded.processor == nullptr)
        return errorResponse (error);

    PluginChain chain;
    chain.prepare (48000.0);
    auto* p = loaded.processor.get();
    chain.setSlot (0, std::move (loaded.processor), {}, nullptr);
    warmUp (chain);

    auto o = loaded.info;
    put (o, "latency_samples", p->getLatencySamples());
    put (o, "input_channels", p->getMainBusNumInputChannels());
    put (o, "output_channels", p->getMainBusNumOutputChannels());
    put (o, "tail_seconds", tidy (p->getTailLengthSeconds(), 3));
    put (o, "parameters", parameterList (*p));
    return o;
}

var run (const var& request)
{
    const auto start = juce::Time::getMillisecondCounterHiRes();

    if (! request.isObject())
        return errorResponse ("The request must be a JSON object");

    AnalysisSettings settings;
    String error;
    const String mode = request["mode"].toString();
    if (mode.isEmpty())
        return errorResponse ("\"mode\" is required (see 'describe')");
    if (! applyMode (settings, mode, error) || ! applySettings (settings, request["settings"], error))
        return errorResponse (error);

    const int maxPoints = request.hasProperty ("max_points") ? juce::jlimit (0, 5000, (int) request["max_points"]) : 200;

    // Build the chains
    std::array<PluginChain, 2> chains;
    juce::Array<var> chainInfo;

    for (int c = 0; c < 2; ++c)
    {
        chains[(size_t) c].prepare (settings.sampleRate);
        const var slots = request[c == 0 ? "chain1" : "chain2"];
        if (slots.isVoid())
            continue;
        if (! slots.isArray())
            return errorResponse (String (c == 0 ? "chain1" : "chain2") + " must be an array of slots");
        if (slots.size() > PluginChain::numSlots)
            return errorResponse ("A chain holds at most " + String (PluginChain::numSlots) + " plugins");

        juce::Array<var> slotInfo;
        for (int s = 0; s < slots.size(); ++s)
        {
            const var slot = slots[s].isString() ? var() : slots[s];
            const String spec = slots[s].isString() ? slots[s].toString() : slot["plugin"].toString();
            if (spec.isEmpty())
                return errorResponse ("Each slot needs a \"plugin\"");

            auto loaded = loadProcessor (spec, settings.sampleRate, error);
            if (loaded.processor == nullptr)
                return errorResponse (error);

            auto* p = loaded.processor.get();
            auto& chain = chains[(size_t) c];
            chain.setSlot (s, std::move (loaded.processor), {}, nullptr);

            var applied;
            if (! applyParams (*p, slot["params"], applied, error))
                return errorResponse (error);

            if ((bool) slot["bypass"])
                chain.slot (s).bypassed = true;

            auto info = loaded.info;
            put (info, "slot", s + 1);
            if (applied.getDynamicObject()->getProperties().size() > 0)
                put (info, "params", applied);
            if ((bool) slot["bypass"])
                put (info, "bypassed", true);
            slotInfo.add (info);
        }

        warmUp (chains[(size_t) c]);

        for (int s = 0; s < slotInfo.size(); ++s)
            if (auto* p = chains[(size_t) c].slot (s).processor.get())
                put (slotInfo.getReference (s), "latency_samples", p->getLatencySamples());

        auto co = object();
        put (co, "chain", c + 1);
        put (co, "latency_samples", chains[(size_t) c].getLatencySamples());
        put (co, "slots", var (slotInfo));
        chainInfo.add (co);
    }

    // Measure
    AnalysisResult result;
    result.mode = settings.mode;
    result.sampleRate = settings.sampleRate;
    result.midSide = settings.midSide;

    auto noAbort = [] { return false; };
    ChainRenderer first (chains[0], settings.blockSize, noAbort);
    ChainRenderer second (chains[1], settings.blockSize, noAbort);
    const bool hasSecond = chains[1].hasAnyProcessor();
    const bool difference = (bool) request["difference"];

    bool ok;
    if (difference)
    {
        if (! hasSecond)
            return errorResponse ("\"difference\" needs plugins in chain2");
        DifferenceRenderer diff (first, second);
        ok = runMeasurement (settings, diff, 2, result);
    }
    else
    {
        ok = runMeasurement (settings, first, 0, result);
        if (ok && hasSecond)
            ok = runMeasurement (settings, second, 1, result);
    }

    if (! ok)
        return errorResponse ("The measurement did not complete");

    auto response = object();
    auto tool = object();
    put (tool, "name", "plugin-analyzer");
    put (tool, "version", PLUGIN_ANALYZER_VERSION_STRING);
    put (response, "tool", tool);
    put (response, "mode", mode.trim().toLowerCase());
    put (response, "sample_rate", settings.sampleRate);
    put (response, "channels", settings.midSide ? "mid/side" : "left/right");
    if (difference) put (response, "difference", "chain1 - chain2");
    put (response, "chains", var (chainInfo));

    auto body = resultToJson (result, maxPoints);
    for (auto& prop : body.getDynamicObject()->getProperties())
        put (response, prop.name, prop.value);

    put (response, "elapsed_ms", tidy (juce::Time::getMillisecondCounterHiRes() - start, 1));
    return response;
}

} // namespace pa::api
