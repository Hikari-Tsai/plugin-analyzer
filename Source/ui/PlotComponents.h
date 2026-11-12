#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "analysis/Types.h"

namespace pa
{

namespace Theme
{
    const juce::Colour background   { 0xff15171c };
    const juce::Colour panel        { 0xff1e2128 };
    const juce::Colour plotBg       { 0xff101216 };
    const juce::Colour grid         { 0xff2a2e37 };
    const juce::Colour gridMajor    { 0xff3a3f4b };
    const juce::Colour text         { 0xffd8dce4 };
    const juce::Colour textDim      { 0xff8a91a0 };
    const juce::Colour accent       { 0xffff9f1c };

    juce::Colour curveColour (int chain, int channel);
    juce::Colour orderColour (int order);

    /** Sets the app-wide colours on the default LookAndFeel (call once at startup / editor creation). */
    void applyToDefaultLookAndFeel();
}

enum class AxisFormat { Hz, dB, Degrees, Ms, Seconds, Percent, Plain, Samples };

struct Axis
{
    double min = 0.0, max = 1.0;
    bool log = false;
    AxisFormat format = AxisFormat::Plain;
};

juce::String formatAxisValue (double v, AxisFormat f, bool withUnit);

/** Shared axis/zoom handling for the line plot and the heatmap. */
class PlotBase : public juce::Component
{
public:
    void setAxes (Axis x, Axis y);        // sets the default view and resets zoom
    void setAxesKeepZoom (Axis x, Axis y); // updates defaults, keeps zoom if active
    void resetZoom();
    void setInfo (const juce::StringArray& lines)
    {
        const bool layoutChanges = lines.size() != info.size();
        info = lines;
        if (layoutChanges) viewChanged(); else repaint();
    }

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;

protected:
    juce::Rectangle<float> plotArea() const;
    double toNorm (double v, const Axis& a, const Axis& view) const;
    float xToPx (double x) const;
    float yToPx (double y) const;
    double pxToX (float px) const;
    double pxToY (float py) const;

    void paintGridAndAxes (juce::Graphics& g);
    void paintOverlay (juce::Graphics& g);
    virtual void viewChanged() { repaint(); }
    virtual juce::String readoutAt (juce::Point<float> p) const;

    void zoomAxis (Axis& view, const Axis& def, double centreNorm, double factor);
    void panAxis (Axis& view, double deltaNorm);

    Axis defX, defY, viewX, viewY;
    bool zoomed = false;
    juce::StringArray info;

    juce::Point<float> dragStart, dragNow;
    bool dragging = false;
    juce::Point<float> hover { -1.0f, -1.0f };
};

class LinePlot : public PlotBase
{
public:
    struct Series
    {
        std::vector<float> x, y;
        juce::Colour colour;
        juce::String label;
        float thickness = 1.6f;
        bool dashed = false;
    };

    void setSeries (std::vector<Series> s) { series = std::move (s); repaint(); }
    void setStored (std::vector<Series> s) { stored = std::move (s); repaint(); }
    const std::vector<Series>& getSeries() const { return series; }
    bool hasStored() const { return ! stored.empty(); }

    void paint (juce::Graphics& g) override;

private:
    void drawSeries (juce::Graphics& g, const Series& s, juce::Rectangle<float> area);
    std::vector<Series> series, stored;
};

class HeatmapPlot : public PlotBase
{
public:
    void setData (const Heatmap* map);   // copies; nullptr clears
    void setRange (float minDb, float maxDb);
    void setLogFrequency (bool shouldBeLog);

    void paint (juce::Graphics& g) override;
    void resized() override { imageDirty = true; }

private:
    void viewChanged() override { imageDirty = true; repaint(); }
    juce::String readoutAt (juce::Point<float> p) const override;
    void rebuildImage();
    float valueAt (double t, double f) const;

    Heatmap data;
    bool hasData = false;
    float rangeMin = -160.0f, rangeMax = 0.0f;
    juce::Image image;
    bool imageDirty = true;
    std::array<juce::Colour, 256> lut {};
    bool lutReady = false;
};

/** Small colour bar with a two-value slider for the heatmap range. */
class RangeBar : public juce::Component
{
public:
    RangeBar();
    std::function<void (float, float)> onChange;
    void setRange (float lo, float hi);
    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    juce::Slider slider { juce::Slider::TwoValueVertical, juce::Slider::NoTextBox };
};

} // namespace pa
