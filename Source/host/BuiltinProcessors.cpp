#include "BuiltinProcessors.h"

namespace pa
{

namespace
{

using juce::AudioBuffer;
using juce::AudioParameterFloat;
using juce::String;

class BuiltinProcessor : public juce::AudioProcessor
{
public:
    explicit BuiltinProcessor (String nameToUse)
        : AudioProcessor (BusesProperties()
                              .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          processorName (std::move (nameToUse))
    {
    }

    const String getName() const override { return processorName; }

    void prepareToPlay (double sr, int) override
    {
        rate = sr;
        prepare();
    }

    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& layout) const override
    {
        const auto& in = layout.getMainInputChannelSet();
        return ! in.isDisabled() && in == layout.getMainOutputChannelSet() && in.size() <= 2;
    }

    void processBlock (AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int channels = juce::jmin (2, getMainBusNumInputChannels());
        for (int ch = 0; ch < channels; ++ch)
            processChannel (ch, buffer.getWritePointer (ch), buffer.getNumSamples());
        postBlock (buffer, channels);
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor (*this); }
    bool hasEditor() const override { return true; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const String getProgramName (int) override { return {}; }
    void changeProgramName (int, const String&) override {}

    void getStateInformation (juce::MemoryBlock& data) override
    {
        juce::XmlElement xml ("STATE");
        for (auto* p : getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                xml.setAttribute (rp->getParameterID(), (double) rp->getValue());
        copyXmlToBinary (xml, data);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
            for (auto* p : getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                    if (xml->hasAttribute (rp->getParameterID()))
                        rp->setValueNotifyingHost ((float) xml->getDoubleAttribute (rp->getParameterID()));
    }

protected:
    virtual void prepare() {}
    virtual void processChannel (int channel, float* data, int numSamples) = 0;
    virtual void postBlock (AudioBuffer<float>&, int) {}

    AudioParameterFloat* addFloat (const String& id, const String& name, float min, float max,
                                   float def, float skewCentre = 0.0f, const String& unit = {})
    {
        juce::NormalisableRange<float> range (min, max);
        if (skewCentre > min && skewCentre < max)
            range.setSkewForCentre (skewCentre);

        auto* p = new AudioParameterFloat (juce::ParameterID { id, 1 }, name, range, def,
                                           juce::AudioParameterFloatAttributes().withLabel (unit));
        addParameter (p);
        return p;
    }

    static float db (float v) { return juce::Decibels::decibelsToGain (v, -200.0f); }

    double rate = 48000.0;
    String processorName;
};

//==============================================================================
class GainProcessor final : public BuiltinProcessor
{
public:
    GainProcessor() : BuiltinProcessor ("Gain") { gain = addFloat ("gain", "Gain", -24.0f, 24.0f, 0.0f, 0.0f, "dB"); }

    void processChannel (int, float* d, int n) override
    {
        const float g = db (gain->get());
        for (int i = 0; i < n; ++i) d[i] *= g;
    }

private:
    AudioParameterFloat* gain;
};

//==============================================================================
class SaturatorProcessor final : public BuiltinProcessor
{
public:
    SaturatorProcessor() : BuiltinProcessor ("Tanh Saturator")
    {
        drive = addFloat ("drive", "Drive", 0.0f, 36.0f, 12.0f, 0.0f, "dB");
        bias = addFloat ("bias", "Bias (even harmonics)", 0.0f, 0.5f, 0.0f);
        output = addFloat ("output", "Output", -36.0f, 12.0f, -12.0f, 0.0f, "dB");
    }

    void processChannel (int, float* d, int n) override
    {
        const float g = db (drive->get()), b = bias->get(), o = db (output->get());
        const float offset = std::tanh (g * b);
        for (int i = 0; i < n; ++i)
            d[i] = o * (std::tanh (g * (d[i] + b)) - offset);
    }

private:
    AudioParameterFloat *drive, *bias, *output;
};

//==============================================================================
class PolynomialProcessor final : public BuiltinProcessor
{
public:
    PolynomialProcessor() : BuiltinProcessor ("Polynomial")
    {
        a1 = addFloat ("a1", "a1 (x)", -2.0f, 2.0f, 1.0f);
        a2 = addFloat ("a2", "a2 (x^2)", -1.0f, 1.0f, 0.1f);
        a3 = addFloat ("a3", "a3 (x^3)", -1.0f, 1.0f, 0.05f);
    }

    void processChannel (int, float* d, int n) override
    {
        const float c1 = a1->get(), c2 = a2->get(), c3 = a3->get();
        for (int i = 0; i < n; ++i)
        {
            const float x = d[i];
            d[i] = x * (c1 + x * (c2 + x * c3));
        }
    }

private:
    AudioParameterFloat *a1, *a2, *a3;
};

//==============================================================================
class PeakEqProcessor final : public BuiltinProcessor
{
public:
    PeakEqProcessor() : BuiltinProcessor ("Peak EQ")
    {
        freq = addFloat ("freq", "Frequency", 20.0f, 20000.0f, 1000.0f, 1000.0f, "Hz");
        gain = addFloat ("gain", "Gain", -24.0f, 24.0f, 6.0f, 0.0f, "dB");
        q = addFloat ("q", "Q", 0.1f, 10.0f, 1.0f, 1.0f);
    }

    void prepare() override
    {
        for (auto& s : state) s = {};
    }

    void processChannel (int ch, float* d, int n) override
    {
        // RBJ cookbook peaking EQ, computed in double precision
        const double A = std::pow (10.0, gain->get() / 40.0);
        const double w0 = juce::MathConstants<double>::twoPi * juce::jmin ((double) freq->get(), rate * 0.49) / rate;
        const double alpha = std::sin (w0) / (2.0 * q->get());
        const double cw = std::cos (w0);
        const double a0 = 1.0 + alpha / A;
        const double b0 = (1.0 + alpha * A) / a0, b1 = -2.0 * cw / a0, b2 = (1.0 - alpha * A) / a0;
        const double a1 = -2.0 * cw / a0, a2 = (1.0 - alpha / A) / a0;

        auto& s = state[(size_t) ch];
        for (int i = 0; i < n; ++i)
        {
            const double x = d[i];
            const double y = b0 * x + s.z1;
            s.z1 = b1 * x - a1 * y + s.z2;
            s.z2 = b2 * x - a2 * y;
            d[i] = (float) y;
        }
    }

    void reset() override { prepare(); }

private:
    struct State { double z1 = 0, z2 = 0; };
    std::array<State, 2> state;
    AudioParameterFloat *freq, *gain, *q;
};

//==============================================================================
class CompressorProcessor final : public BuiltinProcessor
{
public:
    CompressorProcessor() : BuiltinProcessor ("Compressor")
    {
        threshold = addFloat ("threshold", "Threshold", -60.0f, 0.0f, -20.0f, 0.0f, "dB");
        ratio = addFloat ("ratio", "Ratio", 1.0f, 20.0f, 4.0f, 4.0f);
        attack = addFloat ("attack", "Attack", 0.1f, 200.0f, 10.0f, 10.0f, "ms");
        release = addFloat ("release", "Release", 5.0f, 2000.0f, 100.0f, 100.0f, "ms");
        makeup = addFloat ("makeup", "Makeup", 0.0f, 24.0f, 0.0f, 0.0f, "dB");
    }

    void prepare() override { envDb = { 0.0f, 0.0f }; }
    void reset() override { prepare(); }

    void processChannel (int, float*, int) override {}

    void postBlock (AudioBuffer<float>& buffer, int channels) override
    {
        // Stereo-linked peak detector, smoothing applied to gain reduction in dB
        const float th = threshold->get(), r = ratio->get(), mk = makeup->get();
        const float aCoef = std::exp (-1.0f / (0.001f * attack->get() * (float) rate));
        const float rCoef = std::exp (-1.0f / (0.001f * release->get() * (float) rate));
        const int n = buffer.getNumSamples();

        for (int i = 0; i < n; ++i)
        {
            float peak = 0.0f;
            for (int ch = 0; ch < channels; ++ch)
                peak = juce::jmax (peak, std::abs (buffer.getSample (ch, i)));

            // Peak follower (instant rise, release decay) so a steady sine reads its peak
            float& follower = envDb[1];
            follower = juce::jmax (peak, follower * rCoef);

            const float levelDb = juce::Decibels::gainToDecibels (follower, -200.0f);
            const float over = levelDb - th;
            const float targetGr = over > 0.0f ? over * (1.0f - 1.0f / r) : 0.0f;
            float& gr = envDb[0];
            const float coef = targetGr > gr ? aCoef : rCoef;
            gr = targetGr + coef * (gr - targetGr);

            const float g = db (mk - gr);
            for (int ch = 0; ch < channels; ++ch)
                buffer.setSample (ch, i, buffer.getSample (ch, i) * g);
        }
    }

private:
    std::array<float, 2> envDb {};
    AudioParameterFloat *threshold, *ratio, *attack, *release, *makeup;
};

//==============================================================================
class HardClipProcessor final : public BuiltinProcessor
{
public:
    HardClipProcessor() : BuiltinProcessor ("Hard Clipper")
    {
        drive = addFloat ("drive", "Drive", 0.0f, 24.0f, 6.0f, 0.0f, "dB");
        ceiling = addFloat ("ceiling", "Ceiling", -24.0f, 0.0f, -6.0f, 0.0f, "dB");
    }

    void processChannel (int, float* d, int n) override
    {
        const float g = db (drive->get()), c = db (ceiling->get());
        for (int i = 0; i < n; ++i)
            d[i] = juce::jlimit (-c, c, d[i] * g);
    }

private:
    AudioParameterFloat *drive, *ceiling;
};

//==============================================================================
class LatencyProcessor final : public BuiltinProcessor
{
public:
    LatencyProcessor() : BuiltinProcessor ("Latency (delay)")
    {
        delay = addFloat ("delay", "Delay", 0.0f, 4096.0f, 256.0f, 0.0f, "samples");
        report = addFloat ("report", "Report latency (0/1)", 0.0f, 1.0f, 1.0f);
    }

    void prepare() override
    {
        line.setSize (2, 8192);
        line.clear();
        writePos = 0;
        updateLatency();
    }

    void reset() override { prepare(); }

    void processBlock (AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        updateLatency();
        const int d = (int) delay->get();
        const int size = line.getNumSamples();
        const int channels = juce::jmin (2, buffer.getNumChannels());
        const int n = buffer.getNumSamples();

        for (int i = 0; i < n; ++i)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                line.setSample (ch, writePos, buffer.getSample (ch, i));
                buffer.setSample (ch, i, line.getSample (ch, (writePos - d + size) % size));
            }
            writePos = (writePos + 1) % size;
        }
    }

    void processChannel (int, float*, int) override {}

private:
    void updateLatency()
    {
        const int want = report->get() > 0.5f ? (int) delay->get() : 0;
        if (want != getLatencySamples())
            setLatencySamples (want);
    }

    AudioBuffer<float> line;
    int writePos = 0;
    AudioParameterFloat *delay, *report;
};

//==============================================================================
class ReverbProcessor final : public BuiltinProcessor
{
public:
    ReverbProcessor() : BuiltinProcessor ("Simple Reverb")
    {
        size = addFloat ("size", "Room size", 0.0f, 1.0f, 0.7f);
        damping = addFloat ("damping", "Damping", 0.0f, 1.0f, 0.4f);
        wet = addFloat ("wet", "Wet", 0.0f, 1.0f, 0.33f);
        dry = addFloat ("dry", "Dry", 0.0f, 1.0f, 0.7f);
    }

    void prepare() override
    {
        reverb.setSampleRate (rate);
        reverb.reset();
    }

    void reset() override { reverb.reset(); }

    void processChannel (int, float*, int) override {}

    void postBlock (AudioBuffer<float>& buffer, int channels) override
    {
        juce::Reverb::Parameters p;
        p.roomSize = size->get();
        p.damping = damping->get();
        p.wetLevel = wet->get();
        p.dryLevel = dry->get();
        p.width = 1.0f;
        reverb.setParameters (p);

        if (channels >= 2)
            reverb.processStereo (buffer.getWritePointer (0), buffer.getWritePointer (1), buffer.getNumSamples());
        else
            reverb.processMono (buffer.getWritePointer (0), buffer.getNumSamples());
    }

private:
    juce::Reverb reverb;
    AudioParameterFloat *size, *damping, *wet, *dry;
};

} // namespace

//==============================================================================
const std::vector<BuiltinInfo>& getBuiltinList()
{
    static const std::vector<BuiltinInfo> list {
        { "gain", "Gain" },
        { "peakeq", "Peak EQ" },
        { "saturator", "Tanh Saturator" },
        { "polynomial", "Polynomial" },
        { "hardclip", "Hard Clipper" },
        { "compressor", "Compressor" },
        { "latency", "Latency (delay)" },
        { "reverb", "Simple Reverb" },
    };
    return list;
}

std::unique_ptr<juce::AudioProcessor> createBuiltin (const juce::String& id)
{
    if (id == "gain")       return std::make_unique<GainProcessor>();
    if (id == "peakeq")     return std::make_unique<PeakEqProcessor>();
    if (id == "saturator")  return std::make_unique<SaturatorProcessor>();
    if (id == "polynomial") return std::make_unique<PolynomialProcessor>();
    if (id == "hardclip")   return std::make_unique<HardClipProcessor>();
    if (id == "compressor") return std::make_unique<CompressorProcessor>();
    if (id == "latency")    return std::make_unique<LatencyProcessor>();
    if (id == "reverb")     return std::make_unique<ReverbProcessor>();
    return nullptr;
}

void setBuiltinParameter (juce::AudioProcessor& p, const juce::String& paramId, float plainValue)
{
    for (auto* param : p.getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (param))
            if (rp->getParameterID() == paramId)
                rp->setValueNotifyingHost (rp->convertTo0to1 (plainValue));
}

} // namespace pa
