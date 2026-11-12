#include "AnalysisTabs.h"

namespace pa
{

using juce::String;

//==============================================================================
Segmented::Segmented (const juce::StringArray& items)
{
    static int groupCounter = 1000;
    const int group = ++groupCounter;

    for (int i = 0; i < items.size(); ++i)
    {
        auto* b = buttons.add (new juce::TextButton (items[i]));
        b->setClickingTogglesState (true);
        b->setRadioGroupId (group);
        b->setColour (juce::TextButton::buttonColourId, Theme::plotBg);
        b->setColour (juce::TextButton::buttonOnColourId, Theme::accent.withAlpha (0.85f));
        b->setColour (juce::TextButton::textColourOffId, Theme::textDim);
        b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);

        int edges = 0;
        if (i > 0) edges |= juce::Button::ConnectedOnLeft;
        if (i < items.size() - 1) edges |= juce::Button::ConnectedOnRight;
        b->setConnectedEdges (edges);

        b->onClick = [this, i]
        {
            if (buttons[i]->getToggleState() && selected != i)
            {
                selected = i;
                if (onChange)
                    onChange (i);
            }
        };
        addAndMakeVisible (b);
    }

    buttons[0]->setToggleState (true, juce::dontSendNotification);
}

void Segmented::setSelected (int index, bool notify)
{
    index = juce::jlimit (0, buttons.size() - 1, index);
    selected = index;
    buttons[index]->setToggleState (true, juce::dontSendNotification);
    if (notify && onChange)
        onChange (index);
}

int Segmented::getIdealWidth() const
{
    int w = 0;
    for (auto* b : buttons)
        w += juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (13.0f), b->getButtonText()) + 22;
    return w;
}

void Segmented::resized()
{
    const int total = getIdealWidth();
    auto r = getLocalBounds();
    for (auto* b : buttons)
    {
        const int ideal = juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (13.0f), b->getButtonText()) + 22;
        b->setBounds (r.removeFromLeft (getWidth() * ideal / juce::jmax (1, total)));
    }
}

//==============================================================================
ValueControl::ValueControl (const String& name, double min, double max, double interval, double initial,
                            const String& suffix, double skewCentre)
{
    label.setText (name, juce::dontSendNotification);
    label.setColour (juce::Label::textColourId, Theme::textDim);
    label.setFont (juce::FontOptions (12.5f));
    label.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (label);

    slider.setRange (min, max, interval);
    if (skewCentre > min && skewCentre < max)
        slider.setSkewFactorFromMidPoint (skewCentre);
    slider.setValue (initial, juce::dontSendNotification);
    slider.setTextValueSuffix (suffix);
    slider.setNumDecimalPlacesToDisplay (interval >= 1.0 ? 0 : interval >= 0.1 ? 1 : 2);
    slider.setColour (juce::Slider::trackColourId, Theme::accent.withAlpha (0.35f));
    slider.setColour (juce::Slider::backgroundColourId, Theme::plotBg);
    slider.setColour (juce::Slider::textBoxTextColourId, Theme::text);
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    slider.setDoubleClickReturnValue (true, initial, juce::ModifierKeys::altModifier);
    slider.onValueChange = [this]
    {
        if (onChange)
            onChange (slider.getValue());
    };
    addAndMakeVisible (slider);
}

void ValueControl::resized()
{
    auto r = getLocalBounds();
    const int lw = juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (12.5f), label.getText()) + 10;
    label.setBounds (r.removeFromLeft (lw));
    slider.setBounds (r);
}

//==============================================================================
std::vector<LinePlot::Series> AnalysisTab::collect (const AnalysisResult& r, const String& kind, bool dashed, float alpha)
{
    std::vector<LinePlot::Series> out;

    for (auto& c : r.curves)
    {
        if (c.kind != kind)
            continue;

        LinePlot::Series s;
        s.x = c.x;
        s.y = c.y;
        s.colour = Theme::curveColour (c.chain, c.channel).withAlpha (alpha);
        s.label = chainLabel (c.chain) + " " + channelLabel (r.midSide, c.channel);
        s.dashed = dashed;
        out.push_back (std::move (s));
    }

    return out;
}

void AnalysisTab::storeCurrent (LinePlot& plot, int viewKey)
{
    auto s = plot.getSeries();
    for (auto& e : s)
    {
        e.colour = e.colour.withMultipliedBrightness (0.65f).withAlpha (0.75f);
        e.dashed = true;
        e.thickness = 1.3f;
        e.label = "stored " + e.label;
    }
    storedByView[viewKey] = s;
    plot.setStored (s);
}

void AnalysisTab::clearStored (LinePlot& plot, int viewKey)
{
    storedByView.erase (viewKey);
    plot.setStored ({});
}

void AnalysisTab::applyStored (LinePlot& plot, int viewKey)
{
    auto it = storedByView.find (viewKey);
    plot.setStored (it != storedByView.end() ? it->second : std::vector<LinePlot::Series>());
}

static void setupChainSelect (juce::ComboBox& box)
{
    box.addItem ("Chain 1", 1);
    box.addItem ("Chain 2", 2);
    box.addItem ("1-2 (difference)", 3);
    box.setSelectedId (1, juce::dontSendNotification);
}

static const Heatmap* findHeatmap (const AnalysisResult& r, const String& kind, int wantedChain)
{
    const Heatmap* fallback = nullptr;
    for (auto& h : r.heatmaps)
    {
        if (h.kind != kind)
            continue;
        if (h.chain == wantedChain)
            return &h;
        if (fallback == nullptr)
            fallback = &h;
    }
    return fallback;
}

static LinePlot::Series referenceSeries (std::vector<float> x, std::vector<float> y, const String& label)
{
    LinePlot::Series s;
    s.x = std::move (x);
    s.y = std::move (y);
    s.colour = Theme::textDim.withAlpha (0.7f);
    s.label = label;
    s.dashed = true;
    s.thickness = 1.2f;
    return s;
}

//==============================================================================
// Linear
//==============================================================================
LinearTab::LinearTab (AnalysisEngine& e) : AnalysisTab (e)
{
    for (auto* c : std::initializer_list<juce::Component*> { &signal, &view, &level, &storeButton, &clearButton, &plot })
        addAndMakeVisible (c);

    signal.onChange = [this] (int i) { update ([i] (AnalysisSettings& s) { s.linearSignal = (LinearSignal) i; }); };
    level.onChange = [this] (double v) { update ([v] (AnalysisSettings& s) { s.linearLevelDb = (float) v; }); };
    view.onChange = [this] (int)
    {
        updateAxes();
        applyStored (plot, view.getSelected());
        if (auto r = engine.getLatestResult(); r != nullptr && r->mode == getMode())
            showResult (*r);
    };
    storeButton.onClick = [this] { storeCurrent (plot, view.getSelected()); };
    clearButton.onClick = [this] { clearStored (plot, view.getSelected()); };
    updateAxes();
}

void LinearTab::updateAxes()
{
    const double sr = sampleRate();
    const Axis freqAxis { 10.0, sr / 2, true, AxisFormat::Hz };

    switch (view.getSelected())
    {
        case 0:  plot.setAxes (freqAxis, { -30.0, 30.0, false, AxisFormat::dB }); break;
        case 1:  plot.setAxes (freqAxis, { -200.0, 200.0, false, AxisFormat::Degrees }); break;
        case 2:  plot.setAxes (freqAxis, { -2.0, 10.0, false, AxisFormat::Ms }); break;
        case 3:  plot.setAxes ({ -0.5, 20.0, false, AxisFormat::Ms }, { -1.1, 1.1, false, AxisFormat::Plain }); break;
        default: plot.setAxes (freqAxis, { -160.0, 10.0, false, AxisFormat::dB }); break;
    }
}

void LinearTab::showResult (const AnalysisResult& r)
{
    static const char* kinds[] = { "mag", "phase", "gd", "ir", "xtalk" };
    auto series = collect (r, kinds[view.getSelected()]);

    juce::StringArray info = r.info;
    if (view.getSelected() == 4)
    {
        for (auto& s : series)    // crosstalk curves are tagged by source channel
            s.label = s.label.upToFirstOccurrenceOf (" ", false, false) + (s.label.endsWith ("L") || s.label.endsWith ("Mid") ? "  L -> R" : "  R -> L");
        if (series.empty())
            info.add ("Crosstalk needs the Delta signal (one channel is excited at a time; always raw L/R).");
    }
    else if (view.getSelected() == 2)
    {
        info.add ("Group delay after latency compensation (a pure delay that is not reported shows as a constant).");
    }

    plot.setSeries (std::move (series));
    plot.setInfo (info);
}

void LinearTab::resized()
{
    auto r = getLocalBounds();
    auto row = r.removeFromTop (40).reduced (8, 7);
    signal.setBounds (take (row, signal.getIdealWidth()));
    view.setBounds (take (row, view.getIdealWidth()));
    level.setBounds (take (row, 190));
    storeButton.setBounds (take (row, 60));
    clearButton.setBounds (take (row, 60));
    plot.setBounds (r.reduced (4));
}

//==============================================================================
// Harmonic
//==============================================================================
HarmonicTab::HarmonicTab (AnalysisEngine& e) : AnalysisTab (e)
{
    for (auto* c : std::initializer_list<juce::Component*> { &levelView, &levelFrom, &levelTo })
        addAndMakeVisible (c);

    for (auto* c : std::initializer_list<juce::Component*> { &mode, &level, &freq, &sweepView, &f1, &f2, &sweepShape,
                                                             &freqScale, &chainSelect, &storeButton, &clearButton,
                                                             &plot, &heatmap, &rangeBar })
        addAndMakeVisible (c);

    setupChainSelect (chainSelect);
    heatmap.setRange (-160.0f, 0.0f);

    mode.onChange = [this] (int i)
    {
        update ([i] (AnalysisSettings& s) { s.harmonicMode = (HarmonicMode) i; });
        updateVisibility();
        updateAxes();
        applyStored (plot, viewKey());
    };
    level.onChange = [this] (double v) { update ([v] (AnalysisSettings& s) { s.harmonicLevelDb = (float) v; }); };
    freq.onChange = [this] (double v) { update ([v] (AnalysisSettings& s) { s.harmonicFreq = (float) v; }); };
    sweepView.onChange = [this] (int)
    {
        updateAxes();
        applyStored (plot, viewKey());
        if (auto r = engine.getLatestResult(); r != nullptr && r->mode == getMode())
            showResult (*r);
    };
    levelView.onChange = sweepView.onChange;
    levelFrom.onChange = [this] (double v) { update ([v] (AnalysisSettings& s) { s.levelSweepFromDb = (float) v; }); updateAxes(); };
    levelTo.onChange = [this] (double v) { update ([v] (AnalysisSettings& s) { s.levelSweepToDb = (float) v; }); updateAxes(); };
    f1.onChange = [this] (double v) { update ([v] (AnalysisSettings& s) { s.sweep2DStart = (float) v; }); };
    f2.onChange = [this] (double v) { update ([v] (AnalysisSettings& s) { s.sweep2DEnd = (float) v; }); };
    sweepShape.onChange = [this] (int i) { update ([i] (AnalysisSettings& s) { s.sweep2DExponential = i == 0; }); };
    freqScale.onChange = [this] (int i) { heatmap.setLogFrequency (i == 1); };
    chainSelect.onChange = [this]
    {
        if (auto r = engine.getLatestResult(); r != nullptr && r->mode == getMode())
            showResult (*r);
    };
    rangeBar.onChange = [this] (float lo, float hi) { heatmap.setRange (lo, hi); };
    storeButton.onClick = [this] { storeCurrent (plot, viewKey()); };
    clearButton.onClick = [this] { clearStored (plot, viewKey()); };

    updateVisibility();
    updateAxes();
}

void HarmonicTab::updateVisibility()
{
    const int m = mode.getSelected();
    freq.setVisible (m == 0 || m == 4);
    level.setVisible (m != 4);
    sweepView.setVisible (m == 2);
    for (auto* c : std::initializer_list<juce::Component*> { &levelView, &levelFrom, &levelTo })
        c->setVisible (m == 4);
    for (auto* c : std::initializer_list<juce::Component*> { &f1, &f2, &sweepShape, &freqScale, &chainSelect, &heatmap, &rangeBar })
        c->setVisible (m == 3);
    plot.setVisible (m != 3);
    storeButton.setVisible (m != 3);
    clearButton.setVisible (m != 3);
    resized();
}

void HarmonicTab::updateAxes()
{
    const auto s = engine.getSettings();
    const double sr = s.sampleRate;
    const Axis freqAxis { 10.0, sr / 2, true, AxisFormat::Hz };

    switch (mode.getSelected())
    {
        case 0:
        case 1:  plot.setAxes (freqAxis, { -180.0, 10.0, false, AxisFormat::dB }); break;
        case 2:
            plot.setAxes (freqAxis, sweepView.getSelected() == 1 ? Axis { -30.0, 30.0, false, AxisFormat::dB }
                                                                 : Axis { -160.0, 0.0, false, AxisFormat::dB });
            break;
        case 4:
        {
            const Axis x { juce::jmin (levelFrom.getValue(), levelTo.getValue()),
                           juce::jmax (levelFrom.getValue(), levelTo.getValue()), false, AxisFormat::dB };
            plot.setAxes (x, levelView.getSelected() == 2 ? Axis { -30.0, 30.0, false, AxisFormat::dB }
                                                           : Axis { -160.0, 0.0, false, AxisFormat::dB });
            break;
        }
        default:
            heatmap.setAxes ({ 0.0, s.sweep2DSeconds, false, AxisFormat::Seconds },
                             { freqScale.getSelected() == 1 ? 20.0 : 0.0, sr / 2, freqScale.getSelected() == 1, AxisFormat::Hz });
            break;
    }
}

void HarmonicTab::showResult (const AnalysisResult& r)
{
    const int m = mode.getSelected();

    if (m == 3)
    {
        heatmap.setData (findHeatmap (r, "sweep2d", chainSelect.getSelectedId() - 1));
        heatmap.setInfo ({});
        return;
    }

    static const char* sweepKinds[] = { "thd", "fundamental", "alias" };
    static const char* levelKinds[] = { "lvl_thdn", "lvl_thd", "lvl_gain" };
    const String kind = m < 2 ? String ("spectrum")
                      : m == 2 ? String (sweepKinds[sweepView.getSelected()])
                               : String (levelKinds[levelView.getSelected()]);
    plot.setSeries (collect (r, kind));
    plot.setInfo (r.info);
}

void HarmonicTab::resized()
{
    auto r = getLocalBounds();
    auto row = r.removeFromTop (40).reduced (8, 7);
    mode.setBounds (take (row, mode.getIdealWidth()));
    if (level.isVisible()) level.setBounds (take (row, 200));
    if (freq.isVisible()) freq.setBounds (take (row, 190));
    if (sweepView.isVisible()) sweepView.setBounds (take (row, sweepView.getIdealWidth()));
    if (levelView.isVisible())
    {
        levelView.setBounds (take (row, levelView.getIdealWidth()));
        levelFrom.setBounds (take (row, 170));
        levelTo.setBounds (take (row, 150));
    }
    if (storeButton.isVisible())
    {
        storeButton.setBounds (take (row, 60));
        clearButton.setBounds (take (row, 60));
    }

    if (mode.getSelected() == 3)
    {
        auto row2 = r.removeFromTop (34).reduced (8, 4);
        f1.setBounds (take (row2, 160));
        f2.setBounds (take (row2, 160));
        sweepShape.setBounds (take (row2, sweepShape.getIdealWidth()));
        freqScale.setBounds (take (row2, freqScale.getIdealWidth()));
        chainSelect.setBounds (take (row2, 150));

        auto area = r.reduced (4);
        rangeBar.setBounds (area.removeFromRight (56).reduced (0, 4));
        heatmap.setBounds (area);
    }
    else
    {
        plot.setBounds (r.reduced (4));
    }
}

//==============================================================================
// Oscilloscope
//==============================================================================
OscilloscopeTab::OscilloscopeTab (AnalysisEngine& e) : AnalysisTab (e)
{
    for (auto* c : std::initializer_list<juce::Component*> { &waveform, &display, &freq, &level, &delay, &plot })
        addAndMakeVisible (c);

    waveform.onChange = [this] (int i) { update ([i] (AnalysisSettings& s) { s.oscWaveform = (Waveform) i; }); };
    display.onChange = [this] (int i)
    {
        update ([i] (AnalysisSettings& s) { s.oscWaveshaper = i == 1; });
        waveform.setEnabled (i == 0);
        delay.setVisible (i == 1);
        updateAxes();
        resized();
    };
    freq.onChange = [this] (double v)
    {
        update ([v] (AnalysisSettings& s) { s.oscFreq = (float) v; });
        updateAxes();
    };
    level.onChange = [this] (double v) { update ([v] (AnalysisSettings& s) { s.oscLevelDb = (float) v; }); };
    delay.onChange = [this] (double v) { update ([v] (AnalysisSettings& s) { s.oscDelaySamples = (int) v; }); };
    delay.setVisible (false);
    updateAxes();
}

void OscilloscopeTab::updateAxes()
{
    if (display.getSelected() == 0)
        plot.setAxes ({ 0.0, 3000.0 / freq.getValue(), false, AxisFormat::Ms }, { -1.25, 1.25, false, AxisFormat::Plain });
    else
        plot.setAxes ({ -1.25, 1.25, false, AxisFormat::Plain }, { -1.25, 1.25, false, AxisFormat::Plain });
}

void OscilloscopeTab::showResult (const AnalysisResult& r)
{
    std::vector<LinePlot::Series> series;

    if (display.getSelected() == 0)
    {
        for (auto& c : r.curves)
            if (c.kind == "input")
                series.push_back (referenceSeries (c.x, c.y, "input"));

        for (auto& s : collect (r, "time"))
            series.push_back (std::move (s));
    }
    else
    {
        series.push_back (referenceSeries ({ -1.25f, 1.25f }, { -1.25f, 1.25f }, "linear"));
        for (auto& s : collect (r, "shape"))
            series.push_back (std::move (s));
    }

    plot.setSeries (std::move (series));
}

void OscilloscopeTab::resized()
{
    auto r = getLocalBounds();
    auto row = r.removeFromTop (40).reduced (8, 7);
    display.setBounds (take (row, display.getIdealWidth()));
    waveform.setBounds (take (row, waveform.getIdealWidth()));
    freq.setBounds (take (row, 180));
    level.setBounds (take (row, 190));
    if (delay.isVisible())
        delay.setBounds (take (row, 170));
    plot.setBounds (r.reduced (4));
}

//==============================================================================
// Dynamics
//==============================================================================
DynamicsTab::DynamicsTab (AnalysisEngine& e) : AnalysisTab (e)
{
    const auto s = engine.getSettings();

    for (int i = 0; i < 3; ++i)
    {
        arLevel[i] = std::make_unique<ValueControl> ("L" + String (i + 1), -100.0, 12.0, 0.5, s.arLevelsDb[i], " dB");
        arTime[i] = std::make_unique<ValueControl> ("T" + String (i + 1), 0.01, 5.0, 0.01, s.arSeconds[i], " s");
        addAndMakeVisible (*arLevel[i]);
        addAndMakeVisible (*arTime[i]);

        arLevel[i]->onChange = [this, i] (double v) { update ([i, v] (AnalysisSettings& st) { st.arLevelsDb[i] = (float) v; }); };
        arTime[i]->onChange = [this, i] (double v)
        {
            update ([i, v] (AnalysisSettings& st) { st.arSeconds[i] = (float) v; });
            updateAxes();
        };
    }

    for (auto* c : std::initializer_list<juce::Component*> { &mode, &freq, &rampStart, &rampEnd, &rampStep, &rampTime,
                                                             &arView, &storeButton, &clearButton, &plot })
        addAndMakeVisible (c);

    mode.onChange = [this] (int i)
    {
        update ([i] (AnalysisSettings& st) { st.dynamicsMode = (DynamicsMode) i; });
        updateVisibility();
        updateAxes();
        applyStored (plot, i * 10 + (i == 1 ? arView.getSelected() : 0));
    };
    freq.onChange = [this] (double v) { update ([v] (AnalysisSettings& st) { st.dynamicsFreq = (float) v; }); };
    rampStart.onChange = [this] (double v) { update ([v] (AnalysisSettings& st) { st.rampStartDb = (float) v; }); updateAxes(); };
    rampEnd.onChange = [this] (double v) { update ([v] (AnalysisSettings& st) { st.rampEndDb = (float) v; }); updateAxes(); };
    rampStep.onChange = [this] (double v) { update ([v] (AnalysisSettings& st) { st.rampStepDb = (float) v; }); };
    rampTime.onChange = [this] (double v) { update ([v] (AnalysisSettings& st) { st.rampSegmentSeconds = (float) v; }); };
    arView.onChange = [this] (int)
    {
        updateAxes();
        applyStored (plot, 10 + arView.getSelected());
        if (auto r = engine.getLatestResult(); r != nullptr && r->mode == getMode())
            showResult (*r);
    };
    storeButton.onClick = [this] { storeCurrent (plot, mode.getSelected() * 10 + (mode.getSelected() == 1 ? arView.getSelected() : 0)); };
    clearButton.onClick = [this] { clearStored (plot, mode.getSelected() * 10 + (mode.getSelected() == 1 ? arView.getSelected() : 0)); };

    updateVisibility();
    updateAxes();
}

void DynamicsTab::updateVisibility()
{
    const bool ramp = mode.getSelected() == 0;
    for (auto* c : std::initializer_list<juce::Component*> { &rampStart, &rampEnd, &rampStep, &rampTime })
        c->setVisible (ramp);
    for (int i = 0; i < 3; ++i)
    {
        arLevel[i]->setVisible (! ramp);
        arTime[i]->setVisible (! ramp);
    }
    arView.setVisible (! ramp);
    resized();
}

void DynamicsTab::updateAxes()
{
    if (mode.getSelected() == 0)
    {
        const double lo = juce::jmin (rampStart.getValue(), rampEnd.getValue());
        const double hi = juce::jmax (rampStart.getValue(), rampEnd.getValue());
        plot.setAxes ({ lo, hi, false, AxisFormat::dB }, { lo, hi + 12.0, false, AxisFormat::dB });
    }
    else
    {
        double total = 0.0;
        for (auto& t : arTime)
            total += t->getValue();

        plot.setAxes ({ 0.0, total, false, AxisFormat::Seconds },
                      arView.getSelected() == 0 ? Axis { -80.0, 6.0, false, AxisFormat::dB }
                                                : Axis { -1.25, 1.25, false, AxisFormat::Plain });
    }
}

void DynamicsTab::showResult (const AnalysisResult& r)
{
    std::vector<LinePlot::Series> series;

    if (mode.getSelected() == 0)
    {
        const float lo = (float) juce::jmin (rampStart.getValue(), rampEnd.getValue());
        const float hi = (float) juce::jmax (rampStart.getValue(), rampEnd.getValue());
        series.push_back (referenceSeries ({ lo, hi }, { lo, hi }, "unity"));

        for (auto& s : collect (r, "ramp_up"))
            series.push_back (std::move (s));
        for (auto& s : collect (r, "ramp_down", true, 0.8f))
        {
            s.label << " (down)";
            series.push_back (std::move (s));
        }
        plot.setInfo ({});
    }
    else if (arView.getSelected() == 0)
    {
        for (auto& c : r.curves)
            if (c.kind == "env_in")
                series.push_back (referenceSeries (c.x, c.y, "input"));
        for (auto& s : collect (r, "env"))
            series.push_back (std::move (s));
        plot.setInfo (r.info);
    }
    else
    {
        series = collect (r, "wave");
        plot.setInfo (r.info);
    }

    plot.setSeries (std::move (series));
}

void DynamicsTab::resized()
{
    auto r = getLocalBounds();
    auto row = r.removeFromTop (40).reduced (8, 7);
    mode.setBounds (take (row, mode.getIdealWidth()));
    freq.setBounds (take (row, 170));

    if (mode.getSelected() == 0)
    {
        rampStart.setBounds (take (row, 150));
        rampEnd.setBounds (take (row, 130));
        rampStep.setBounds (take (row, 130));
        rampTime.setBounds (take (row, 170));
        storeButton.setBounds (take (row, 60));
        clearButton.setBounds (take (row, 60));
    }
    else
    {
        arView.setBounds (take (row, arView.getIdealWidth()));
        storeButton.setBounds (take (row, 60));
        clearButton.setBounds (take (row, 60));

        auto row2 = r.removeFromTop (34).reduced (8, 4);
        for (int i = 0; i < 3; ++i)
        {
            arLevel[i]->setBounds (take (row2, 140));
            arTime[i]->setBounds (take (row2, 130));
        }
    }

    plot.setBounds (r.reduced (4));
}

//==============================================================================
// Hammerstein
//==============================================================================
HammersteinTab::HammersteinTab (AnalysisEngine& e) : AnalysisTab (e)
{
    for (int i = 1; i <= 7; ++i)
        order.addItem ("Order " + String (i), i);
    order.setSelectedId (engine.getSettings().hammersteinOrder, juce::dontSendNotification);
    setupChainSelect (chainSelect);

    for (auto* c : std::initializer_list<juce::Component*> { &order, &level, &view, &chainSelect, &plot })
        addAndMakeVisible (c);

    order.onChange = [this]
    {
        const int n = order.getSelectedId();
        update ([n] (AnalysisSettings& s) { s.hammersteinOrder = n; });
    };
    level.onChange = [this] (double v) { update ([v] (AnalysisSettings& s) { s.hammersteinLevelDb = (float) v; }); };

    auto refresh = [this]
    {
        if (auto r = engine.getLatestResult(); r != nullptr && r->mode == getMode())
            showResult (*r);
    };
    view.onChange = [refresh] (int) { refresh(); };
    chainSelect.onChange = refresh;
    updateAxes();
}

void HammersteinTab::updateAxes()
{
    plot.setAxes ({ 10.0, sampleRate() / 2, true, AxisFormat::Hz }, { -140.0, 12.0, false, AxisFormat::dB });
}

void HammersteinTab::showResult (const AnalysisResult& r)
{
    const String kind = view.getSelected() == 0 ? "G" : "H";
    int chain = chainSelect.getSelectedId() - 1;

    bool found = false;
    for (auto& c : r.curves)
        found = found || (c.kind == kind && c.chain == chain);
    if (! found && ! r.curves.empty())
        chain = r.curves.front().chain;

    std::vector<LinePlot::Series> series;
    for (auto& c : r.curves)
    {
        if (c.kind != kind || c.chain != chain)
            continue;

        LinePlot::Series s;
        s.x = c.x;
        s.y = c.y;
        s.colour = Theme::orderColour (c.channel);
        s.label = kind == "G" ? "G" + String (c.channel) + "  (x^" + String (c.channel) + " branch)"
                              : "H" + String (c.channel) + "  (harmonic " + String (c.channel) + ")";
        series.push_back (std::move (s));
    }

    plot.setSeries (std::move (series));
    plot.setInfo (r.info);
}

void HammersteinTab::resized()
{
    auto r = getLocalBounds();
    auto row = r.removeFromTop (40).reduced (8, 7);
    order.setBounds (take (row, 100));
    level.setBounds (take (row, 200));
    view.setBounds (take (row, view.getIdealWidth()));
    chainSelect.setBounds (take (row, 150));
    plot.setBounds (r.reduced (4));
}

//==============================================================================
// Reverb
//==============================================================================
ReverbTab::ReverbTab (AnalysisEngine& e) : AnalysisTab (e)
{
    for (auto* c : std::initializer_list<juce::Component*> { &length, &level, &dims, &view1D, &freqScale, &chainSelect,
                                                             &plot, &heatmap, &rangeBar })
        addAndMakeVisible (c);

    setupChainSelect (chainSelect);
    freqScale.setSelected (1, false);
    heatmap.setRange (-140.0f, 0.0f);
    rangeBar.setRange (-140.0f, 0.0f);

    auto refresh = [this]
    {
        if (auto r = engine.getLatestResult(); r != nullptr && r->mode == getMode())
            showResult (*r);
    };

    length.onChange = [this] (double v)
    {
        update ([v] (AnalysisSettings& s) { s.reverbSeconds = (float) v; });
        updateAxes();
    };
    level.onChange = [this] (double v) { update ([v] (AnalysisSettings& s) { s.reverbLevelDb = (float) v; }); };
    dims.onChange = [this, refresh] (int) { updateVisibility(); updateAxes(); refresh(); };
    view1D.onChange = [this, refresh] (int) { updateAxes(); refresh(); };
    freqScale.onChange = [this] (int i) { heatmap.setLogFrequency (i == 1); };
    chainSelect.onChange = refresh;
    rangeBar.onChange = [this] (float lo, float hi) { heatmap.setRange (lo, hi); };

    updateVisibility();
    updateAxes();
}

void ReverbTab::updateVisibility()
{
    const bool twoD = dims.getSelected() == 1;
    view1D.setVisible (! twoD);
    plot.setVisible (! twoD);
    for (auto* c : std::initializer_list<juce::Component*> { &freqScale, &chainSelect, &heatmap, &rangeBar })
        c->setVisible (twoD);
    resized();
}

void ReverbTab::updateAxes()
{
    const double len = length.getValue();
    const double sr = sampleRate();

    if (view1D.getSelected() == 0)
        plot.setAxes ({ 0.0, len, false, AxisFormat::Seconds }, { -1.1, 1.1, false, AxisFormat::Plain });
    else
        plot.setAxes ({ 0.0, len, false, AxisFormat::Seconds }, { -140.0, 6.0, false, AxisFormat::dB });

    const bool logF = freqScale.getSelected() == 1;
    heatmap.setAxes ({ 0.0, len, false, AxisFormat::Seconds }, { logF ? 20.0 : 0.0, sr / 2, logF, AxisFormat::Hz });
}

void ReverbTab::showResult (const AnalysisResult& r)
{
    if (dims.getSelected() == 1)
    {
        heatmap.setData (findHeatmap (r, "reverb2d", chainSelect.getSelectedId() - 1));
        heatmap.setInfo (r.info);
        return;
    }

    if (view1D.getSelected() == 0)
    {
        plot.setSeries (collect (r, "ir"));
    }
    else
    {
        auto series = collect (r, "env", false, 0.85f);
        for (auto& s : collect (r, "edc", true))
        {
            s.label << " EDC";
            s.thickness = 2.0f;
            series.push_back (std::move (s));
        }
        plot.setSeries (std::move (series));
    }

    plot.setInfo (r.info);
}

void ReverbTab::resized()
{
    auto r = getLocalBounds();
    auto row = r.removeFromTop (40).reduced (8, 7);
    length.setBounds (take (row, 170));
    level.setBounds (take (row, 170));
    dims.setBounds (take (row, dims.getIdealWidth()));

    if (dims.getSelected() == 0)
    {
        view1D.setBounds (take (row, view1D.getIdealWidth()));
        plot.setBounds (r.reduced (4));
    }
    else
    {
        freqScale.setBounds (take (row, freqScale.getIdealWidth()));
        chainSelect.setBounds (take (row, 150));
        auto area = r.reduced (4);
        rangeBar.setBounds (area.removeFromRight (56).reduced (0, 4));
        heatmap.setBounds (area);
    }
}

//==============================================================================
// Specs
//==============================================================================
SpecsTab::SpecsTab (AnalysisEngine& e) : AnalysisTab (e)
{
    note.setText ("Standard figures measured on each chain (AES17 / IEC 60268 style). Left channel unless noted; "
                  "always raw L/R. Refreshes about once a second.", juce::dontSendNotification);
    note.setColour (juce::Label::textColourId, Theme::textDim);
    note.setFont (juce::FontOptions (12.5f));
    addAndMakeVisible (note);

    copyButton.onClick = [this] { juce::SystemClipboard::copyTextToClipboard (table.toTsv()); };
    addAndMakeVisible (copyButton);

    viewport.setViewedComponent (&table, false);
    viewport.setScrollBarsShown (true, false);
    addAndMakeVisible (viewport);
}

void SpecsTab::showResult (const AnalysisResult& r)
{
    table.setData (r.specs);
    table.setSize (viewport.getMaximumVisibleWidth(), table.getIdealHeight());
}

void SpecsTab::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (40).reduced (10, 6);
    copyButton.setBounds (top.removeFromRight (110));
    note.setBounds (top);
    viewport.setBounds (r.reduced (6));
    table.setSize (viewport.getMaximumVisibleWidth(), table.getIdealHeight());
}

void SpecsTab::Table::setData (std::vector<SpecItem> items)
{
    rows.clear();
    chains.clear();

    for (auto& it : items)
    {
        if (std::find (chains.begin(), chains.end(), it.chain) == chains.end())
            chains.push_back (it.chain);

        auto found = std::find_if (rows.begin(), rows.end(), [&] (const Row& row) { return row.group == it.group && row.name == it.name; });
        if (found == rows.end())
        {
            rows.push_back ({ it.group, it.name, it.note, {} });
            found = rows.end() - 1;
        }
        found->values[it.chain] = it.text;
    }

    std::sort (chains.begin(), chains.end());
    repaint();
}

int SpecsTab::Table::getIdealHeight() const
{
    int groups = 0;
    String last;
    for (auto& r : rows)
        if (r.group != last) { ++groups; last = r.group; }
    return 34 + (int) rows.size() * 24 + groups * 30 + 20;
}

void SpecsTab::Table::paint (juce::Graphics& g)
{
    g.fillAll (Theme::plotBg);

    if (rows.empty())
    {
        g.setColour (Theme::textDim);
        g.drawText ("Measuring...", getLocalBounds(), juce::Justification::centred);
        return;
    }

    const int nameW = juce::jmin (380, getWidth() / 3);
    const int noteW = juce::jmin (330, getWidth() / 4);
    const int valueW = (getWidth() - nameW - noteW - 24) / juce::jmax (1, (int) chains.size());
    int y = 6;

    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.setColour (Theme::textDim);
    g.drawText ("Metric", 12, y, nameW, 22, juce::Justification::centredLeft);
    for (size_t i = 0; i < chains.size(); ++i)
    {
        g.setColour (Theme::curveColour (chains[i], 0));
        g.drawText ("Chain " + chainLabel (chains[i]), 12 + nameW + (int) i * valueW, y, valueW, 22, juce::Justification::centredLeft);
    }
    g.setColour (Theme::textDim);
    g.drawText ("Note", getWidth() - noteW - 12, y, noteW, 22, juce::Justification::centredLeft);
    y += 28;

    String lastGroup;
    const auto mono = juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain);

    for (auto& r : rows)
    {
        if (r.group != lastGroup)
        {
            lastGroup = r.group;
            y += 6;
            g.setColour (Theme::accent);
            g.setFont (juce::FontOptions (13.5f, juce::Font::bold));
            g.drawText (r.group, 12, y, getWidth() - 24, 22, juce::Justification::centredLeft);
            y += 24;
            g.setColour (Theme::grid);
            g.drawHorizontalLine (y - 2, 12.0f, (float) getWidth() - 12.0f);
        }

        g.setFont (juce::FontOptions (13.0f));
        g.setColour (Theme::text);
        g.drawText (r.name, 24, y, nameW - 12, 22, juce::Justification::centredLeft, true);

        g.setFont (mono);
        for (size_t i = 0; i < chains.size(); ++i)
        {
            auto it = r.values.find (chains[i]);
            g.drawText (it != r.values.end() ? it->second : String ("-"),
                        12 + nameW + (int) i * valueW, y, valueW - 8, 22, juce::Justification::centredLeft, true);
        }

        g.setFont (juce::FontOptions (12.0f));
        g.setColour (Theme::textDim);
        g.drawText (r.note, getWidth() - noteW - 12, y, noteW, 22, juce::Justification::centredLeft, true);
        y += 24;
    }
}

String SpecsTab::Table::toTsv() const
{
    String out = "Group\tMetric";
    for (auto c : chains)
        out << "\tChain " << chainLabel (c);
    out << "\tNote\n";

    for (auto& r : rows)
    {
        out << r.group << "\t" << r.name;
        for (auto c : chains)
        {
            auto it = r.values.find (c);
            out << "\t" << (it != r.values.end() ? it->second : String());
        }
        out << "\t" << r.note << "\n";
    }
    return out;
}

//==============================================================================
// Performance
//==============================================================================
PerformanceTab::PerformanceTab (AnalysisEngine& e) : AnalysisTab (e)
{
    note.setText ("Time in each chain's process call, as % of real time, for block sizes 16-4096. Solid: noise input. "
                  "Dashed: silence right after signal - if it is much slower, the plugin likely suffers from denormals.",
                  juce::dontSendNotification);
    note.setColour (juce::Label::textColourId, Theme::textDim);
    note.setFont (juce::FontOptions (12.5f));
    addAndMakeVisible (note);
    addAndMakeVisible (plot);
    plot.setAxes ({ 16.0, 4096.0, true, AxisFormat::Samples }, { 0.0, 1.0, false, AxisFormat::Percent });
}

void PerformanceTab::showResult (const AnalysisResult& r)
{
    std::vector<LinePlot::Series> series;
    float maxY = 0.0f;

    for (auto& c : r.curves)
    {
        if (c.kind != "cpu")
            continue;

        LinePlot::Series s;
        s.x = c.x;
        s.y = c.y;
        s.colour = Theme::curveColour (c.chain, 0).withAlpha (c.channel == 0 ? 1.0f : 0.8f);
        s.dashed = c.channel == 1;
        s.label = "Chain " + chainLabel (c.chain) + (c.channel == 0 ? "  signal" : "  silence after signal");
        for (auto v : s.y)
            maxY = juce::jmax (maxY, v);
        series.push_back (std::move (s));
    }

    const float top = juce::jmax (0.05f, maxY * 1.3f);
    if (lastMax < 0.0f || top > lastMax * 1.05f || top < lastMax * 0.5f)
    {
        lastMax = top;
        plot.setAxesKeepZoom ({ 16.0, 4096.0, true, AxisFormat::Samples }, { 0.0, top, false, AxisFormat::Percent });
    }

    plot.setSeries (std::move (series));
    plot.setInfo (r.info);
}

void PerformanceTab::resized()
{
    auto r = getLocalBounds();
    note.setBounds (r.removeFromTop (40).reduced (10, 4));
    plot.setBounds (r.reduced (4));
}

} // namespace pa
