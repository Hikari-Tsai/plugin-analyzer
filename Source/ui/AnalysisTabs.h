#pragma once

#include "PlotComponents.h"
#include "engine/AnalysisEngine.h"
#include <map>

namespace pa
{

//==============================================================================
/** Row of mutually exclusive buttons. */
class Segmented : public juce::Component,
                  public juce::SettableTooltipClient
{
public:
    explicit Segmented (const juce::StringArray& items);
    void setSelected (int index, bool notify);
    int getSelected() const { return selected; }
    std::function<void (int)> onChange;
    void resized() override;
    int getIdealWidth() const;

private:
    juce::OwnedArray<juce::TextButton> buttons;
    int selected = 0;
};

/** Label + compact bar slider (double-click the value to type). */
class ValueControl : public juce::Component
{
public:
    ValueControl (const juce::String& name, double min, double max, double interval, double initial,
                  const juce::String& suffix, double skewCentre = 0.0);
    void resized() override;
    double getValue() const { return slider.getValue(); }
    void setValue (double v) { slider.setValue (v, juce::dontSendNotification); }
    std::function<void (double)> onChange;

private:
    juce::Label label;
    juce::Slider slider { juce::Slider::LinearBar, juce::Slider::TextBoxAbove };
};

//==============================================================================
class AnalysisTab : public juce::Component
{
public:
    explicit AnalysisTab (AnalysisEngine& e) : engine (e) {}

    virtual Mode getMode() const = 0;
    virtual void showResult (const AnalysisResult& result) = 0;
    virtual void sampleRateChanged (double) {}

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panel); }

protected:
    void update (const std::function<void (AnalysisSettings&)>& f) { engine.updateSettings (f); }
    double sampleRate() const { return engine.getSettings().sampleRate; }

    /** Curves of a kind turned into plot series with chain/channel colours. */
    static std::vector<LinePlot::Series> collect (const AnalysisResult& r, const juce::String& kind,
                                                  bool dashed = false, float alpha = 1.0f);

    /** Store / Clear support keyed by a view id. */
    void storeCurrent (LinePlot& plot, int viewKey);
    void clearStored (LinePlot& plot, int viewKey);
    void applyStored (LinePlot& plot, int viewKey);

    static juce::Rectangle<int> take (juce::Rectangle<int>& row, int width)
    {
        auto r = row.removeFromLeft (width);
        row.removeFromLeft (8);
        return r;
    }

    AnalysisEngine& engine;
    std::map<int, std::vector<LinePlot::Series>> storedByView;
};

//==============================================================================
class LinearTab : public AnalysisTab
{
public:
    explicit LinearTab (AnalysisEngine&);
    Mode getMode() const override { return Mode::Linear; }
    void showResult (const AnalysisResult&) override;
    void sampleRateChanged (double) override { updateAxes(); }
    void resized() override;

private:
    void updateAxes();
    Segmented signal { { "Delta", "Noise" } };
    Segmented view { { "Magnitude", "Phase", "Group delay", "Impulse", "Crosstalk" } };
    ValueControl level { "Level", -60.0, 0.0, 0.5, 0.0, " dB" };
    juce::TextButton storeButton { "Store" }, clearButton { "Clear" };
    LinePlot plot;
};

//==============================================================================
class HarmonicTab : public AnalysisTab
{
public:
    explicit HarmonicTab (AnalysisEngine&);
    Mode getMode() const override { return Mode::Harmonic; }
    void showResult (const AnalysisResult&) override;
    void sampleRateChanged (double) override { updateAxes(); }
    void resized() override;

private:
    void updateAxes();
    void updateVisibility();
    int viewKey() const
    {
        const int m = mode.getSelected();
        return m * 10 + (m == 2 ? sweepView.getSelected() : m == 4 ? levelView.getSelected() : 0);
    }

    Segmented mode { { "THD", "IMD", "Sweep 1D", "Sweep 2D", "THD vs Level" } };
    ValueControl level { "Level", -80.0, 6.0, 0.5, -6.0, " dBFS" };
    ValueControl freq { "Freq", 10.0, 20000.0, 1.0, 1000.0, " Hz", 1000.0 };
    Segmented sweepView { { "THD", "Fundamental", "Non-harmonic" } };
    Segmented levelView { { "THD+N", "THD", "Gain" } };
    ValueControl levelFrom { "From", -140.0, 0.0, 1.0, -60.0, " dBFS" };
    ValueControl levelTo { "To", -60.0, 12.0, 1.0, 0.0, " dBFS" };
    ValueControl f1 { "f1", 1.0, 20000.0, 1.0, 20.0, " Hz", 1000.0 };
    ValueControl f2 { "f2", 10.0, 96000.0, 1.0, 20000.0, " Hz", 1000.0 };
    Segmented sweepShape { { "Exp", "Lin" } };
    Segmented freqScale { { "Lin Hz", "Log Hz" } };
    juce::ComboBox chainSelect;
    juce::TextButton storeButton { "Store" }, clearButton { "Clear" };
    LinePlot plot;
    HeatmapPlot heatmap;
    RangeBar rangeBar;
};

//==============================================================================
class OscilloscopeTab : public AnalysisTab
{
public:
    explicit OscilloscopeTab (AnalysisEngine&);
    Mode getMode() const override { return Mode::Oscilloscope; }
    void showResult (const AnalysisResult&) override;
    void resized() override;

private:
    void updateAxes();
    Segmented waveform { { "Sine", "Square", "Saw", "Triangle" } };
    Segmented display { { "Time", "Waveshaper" } };
    ValueControl freq { "Freq", 10.0, 5000.0, 1.0, 100.0, " Hz", 200.0 };
    ValueControl level { "Level", -60.0, 12.0, 0.5, 0.0, " dBFS" };
    ValueControl delay { "Delay", -256.0, 256.0, 1.0, 0.0, " smp" };
    LinePlot plot;
};

//==============================================================================
class DynamicsTab : public AnalysisTab
{
public:
    explicit DynamicsTab (AnalysisEngine&);
    Mode getMode() const override { return Mode::Dynamics; }
    void showResult (const AnalysisResult&) override;
    void resized() override;

private:
    void updateAxes();
    void updateVisibility();
    Segmented mode { { "Ramp", "Attack / Release" } };
    ValueControl freq { "Freq", 20.0, 10000.0, 1.0, 1000.0, " Hz", 1000.0 };
    ValueControl rampStart { "From", -120.0, 0.0, 1.0, -100.0, " dB" };
    ValueControl rampEnd { "To", -60.0, 12.0, 1.0, 0.0, " dB" };
    ValueControl rampStep { "Step", 0.25, 6.0, 0.25, 1.0, " dB" };
    ValueControl rampTime { "Time/step", 0.05, 1.5, 0.05, 0.2, " s" };
    std::unique_ptr<ValueControl> arLevel[3], arTime[3];
    Segmented arView { { "Envelope", "Waveform" } };
    juce::TextButton storeButton { "Store" }, clearButton { "Clear" };
    LinePlot plot;
};

//==============================================================================
class HammersteinTab : public AnalysisTab
{
public:
    explicit HammersteinTab (AnalysisEngine&);
    Mode getMode() const override { return Mode::Hammerstein; }
    void showResult (const AnalysisResult&) override;
    void sampleRateChanged (double) override { updateAxes(); }
    void resized() override;

private:
    void updateAxes();
    juce::ComboBox order;
    ValueControl level { "Level", -60.0, 6.0, 0.5, -6.0, " dBFS" };
    Segmented view { { "Hammerstein filters G(n)", "Harmonic responses H(n)" } };
    juce::ComboBox chainSelect;
    LinePlot plot;
};

//==============================================================================
class ReverbTab : public AnalysisTab
{
public:
    explicit ReverbTab (AnalysisEngine&);
    Mode getMode() const override { return Mode::Reverb; }
    void showResult (const AnalysisResult&) override;
    void sampleRateChanged (double) override { updateAxes(); }
    void resized() override;

private:
    void updateAxes();
    void updateVisibility();
    ValueControl length { "Length", 1.0, 20.0, 0.5, 5.0, " s" };
    ValueControl level { "Level", -40.0, 0.0, 0.5, 0.0, " dB" };
    Segmented dims { { "1D", "2D" } };
    Segmented view1D { { "Impulse", "Decay (dB)" } };
    Segmented freqScale { { "Lin Hz", "Log Hz" } };
    juce::ComboBox chainSelect;
    LinePlot plot;
    HeatmapPlot heatmap;
    RangeBar rangeBar;
};

//==============================================================================
class SpecsTab : public AnalysisTab
{
public:
    explicit SpecsTab (AnalysisEngine&);
    Mode getMode() const override { return Mode::Specs; }
    void showResult (const AnalysisResult&) override;
    void resized() override;

private:
    class Table : public juce::Component
    {
    public:
        void setData (std::vector<SpecItem> items);
        void paint (juce::Graphics&) override;
        juce::String toTsv() const;
        int getIdealHeight() const;

    private:
        struct Row { juce::String group, name, note; std::map<int, juce::String> values; };
        std::vector<Row> rows;
        std::vector<int> chains;
    };

    juce::Label note;
    juce::TextButton copyButton { "Copy table" };
    juce::Viewport viewport;
    Table table;
};

//==============================================================================
class PerformanceTab : public AnalysisTab
{
public:
    explicit PerformanceTab (AnalysisEngine&);
    Mode getMode() const override { return Mode::Performance; }
    void showResult (const AnalysisResult&) override;
    void resized() override;

private:
    juce::Label note;
    LinePlot plot;
    float lastMax = -1.0f;
};

} // namespace pa
