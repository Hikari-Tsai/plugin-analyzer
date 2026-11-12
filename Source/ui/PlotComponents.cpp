#include "PlotComponents.h"

namespace pa
{

using juce::Colour;
using juce::Graphics;
using juce::String;

juce::Colour Theme::curveColour (int chain, int channel)
{
    static const Colour colours[3][2] = {
        { Colour (0xffff9f1c), Colour (0xffffd166) },   // chain 1
        { Colour (0xff2ec4b6), Colour (0xff8ecae6) },   // chain 2
        { Colour (0xfff15bb5), Colour (0xffc77dff) },   // difference
    };
    return colours[juce::jlimit (0, 2, chain)][juce::jlimit (0, 1, channel)];
}

juce::Colour Theme::orderColour (int order)
{
    static const Colour colours[] = { Colour (0xffff9f1c), Colour (0xff2ec4b6), Colour (0xfff15bb5),
                                      Colour (0xff8ac926), Colour (0xff6a8cff), Colour (0xffffd166),
                                      Colour (0xffe63946) };
    return colours[(size_t) juce::jlimit (0, 6, order - 1)];
}

void Theme::applyToDefaultLookAndFeel()
{
    auto& lf = juce::LookAndFeel::getDefaultLookAndFeel();
    lf.setColour (juce::ResizableWindow::backgroundColourId, background);
    lf.setColour (juce::ComboBox::backgroundColourId, plotBg);
    lf.setColour (juce::ComboBox::outlineColourId, grid);
    lf.setColour (juce::PopupMenu::backgroundColourId, panel);
    lf.setColour (juce::TabbedButtonBar::tabOutlineColourId, grid);
    lf.setColour (juce::TabbedButtonBar::frontTextColourId, accent);
    lf.setColour (juce::TabbedButtonBar::tabTextColourId, textDim);
}

String formatAxisValue (double v, AxisFormat f, bool withUnit)
{
    auto trim = [] (double x, int decimals)
    {
        auto s = String (x, decimals);
        if (s.containsChar ('.'))
            s = s.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");
        return s == "-0" ? String ("0") : s;
    };

    auto decimalsFor = [] (double x)
    {
        const double a = std::abs (x);
        return a >= 100 ? 0 : a >= 10 ? 1 : a >= 1 ? 2 : 3;
    };

    switch (f)
    {
        case AxisFormat::Hz:
            if (std::abs (v) >= 1000.0)
                return trim (v / 1000.0, 2) + (withUnit ? " kHz" : "k");
            return trim (v, v < 10 ? 2 : 1) + (withUnit ? " Hz" : "");
        case AxisFormat::dB:      return trim (v, decimalsFor (v)) + (withUnit ? " dB" : "");
        case AxisFormat::Degrees: return trim (v, 1) + (withUnit ? " deg" : "");
        case AxisFormat::Ms:      return trim (v, decimalsFor (v)) + (withUnit ? " ms" : "");
        case AxisFormat::Seconds: return trim (v, 3) + (withUnit ? " s" : "");
        case AxisFormat::Percent: return trim (v, decimalsFor (v)) + (withUnit ? " %" : "");
        case AxisFormat::Samples: return trim (v, 0);
        case AxisFormat::Plain:
        default:                  return trim (v, decimalsFor (v));
    }
}

//==============================================================================
static double fwd (double v, bool log) { return log ? std::log10 (juce::jmax (1.0e-12, v)) : v; }
static double inv (double t, bool log) { return log ? std::pow (10.0, t) : t; }

void PlotBase::setAxes (Axis x, Axis y)
{
    defX = viewX = x;
    defY = viewY = y;
    zoomed = false;
    viewChanged();
}

void PlotBase::setAxesKeepZoom (Axis x, Axis y)
{
    defX = x;
    defY = y;

    if (! zoomed)
    {
        viewX = x;
        viewY = y;
    }
    else
    {
        viewX.log = x.log; viewX.format = x.format;
        viewY.log = y.log; viewY.format = y.format;
    }

    viewChanged();
}

void PlotBase::resetZoom()
{
    viewX = defX;
    viewY = defY;
    zoomed = false;
    viewChanged();
}

juce::Rectangle<float> PlotBase::plotArea() const
{
    // Info lines get their own header strip so they never hide the data.
    const float header = info.isEmpty() ? 8.0f : 10.0f + 15.0f * (float) info.size();
    return getLocalBounds().toFloat().withTrimmedLeft (58.0f).withTrimmedRight (12.0f)
                                     .withTrimmedTop (header).withTrimmedBottom (24.0f);
}

double PlotBase::toNorm (double v, const Axis&, const Axis& view) const
{
    const double a = fwd (view.min, view.log), b = fwd (view.max, view.log);
    return ! juce::exactlyEqual (a, b) ? (fwd (v, view.log) - a) / (b - a) : 0.0;
}

float PlotBase::xToPx (double x) const
{
    const auto r = plotArea();
    return r.getX() + (float) toNorm (x, defX, viewX) * r.getWidth();
}

float PlotBase::yToPx (double y) const
{
    const auto r = plotArea();
    return r.getBottom() - (float) toNorm (y, defY, viewY) * r.getHeight();
}

double PlotBase::pxToX (float px) const
{
    const auto r = plotArea();
    const double n = (px - r.getX()) / r.getWidth();
    const double a = fwd (viewX.min, viewX.log), b = fwd (viewX.max, viewX.log);
    return inv (a + n * (b - a), viewX.log);
}

double PlotBase::pxToY (float py) const
{
    const auto r = plotArea();
    const double n = (r.getBottom() - py) / r.getHeight();
    const double a = fwd (viewY.min, viewY.log), b = fwd (viewY.max, viewY.log);
    return inv (a + n * (b - a), viewY.log);
}

static double niceStep (double range, int maxTicks)
{
    const double raw = range / juce::jmax (1, maxTicks);
    const double mag = std::pow (10.0, std::floor (std::log10 (raw)));

    for (double m : { 1.0, 2.0, 2.5, 5.0, 10.0 })
        if (m * mag >= raw)
            return m * mag;

    return 10.0 * mag;
}

void PlotBase::paintGridAndAxes (Graphics& g)
{
    const auto r = plotArea();
    g.setFont (juce::FontOptions (11.0f));

    // X axis
    if (viewX.log)
    {
        const double pxPerDecade = r.getWidth() / juce::jmax (1.0e-9, std::log10 (viewX.max / viewX.min));
        const int d0 = (int) std::floor (std::log10 (viewX.min)), d1 = (int) std::ceil (std::log10 (viewX.max));

        for (int d = d0; d <= d1; ++d)
        {
            for (int m = 1; m <= 9; ++m)
            {
                const double v = m * std::pow (10.0, d);
                if (v < viewX.min || v > viewX.max)
                    continue;

                const float x = xToPx (v);
                g.setColour (m == 1 ? Theme::gridMajor : Theme::grid);
                g.drawVerticalLine ((int) x, r.getY(), r.getBottom());

                const bool label = m == 1 || (pxPerDecade > 160 && (m == 2 || m == 5)) || (pxPerDecade > 420);
                if (label)
                {
                    g.setColour (Theme::textDim);
                    g.drawText (formatAxisValue (v, viewX.format, false), (int) x - 30, (int) r.getBottom() + 3, 60, 16,
                                juce::Justification::centredTop);
                }
            }
        }
    }
    else
    {
        const double step = niceStep (viewX.max - viewX.min, juce::jmax (2, (int) (r.getWidth() / 80)));
        for (double v = std::ceil (viewX.min / step) * step; v <= viewX.max + step * 1e-6; v += step)
        {
            const float x = xToPx (v);
            g.setColour (std::abs (v) < step * 1e-6 ? Theme::gridMajor : Theme::grid);
            g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
            g.setColour (Theme::textDim);
            g.drawText (formatAxisValue (v, viewX.format, false), (int) x - 34, (int) r.getBottom() + 3, 68, 16,
                        juce::Justification::centredTop);
        }
    }

    // Y axis
    if (viewY.log)
    {
        const int d0 = (int) std::floor (std::log10 (viewY.min)), d1 = (int) std::ceil (std::log10 (viewY.max));
        for (int d = d0; d <= d1; ++d)
        {
            for (int m = 1; m <= 9; ++m)
            {
                const double v = m * std::pow (10.0, d);
                if (v < viewY.min || v > viewY.max)
                    continue;
                const float y = yToPx (v);
                g.setColour (m == 1 ? Theme::gridMajor : Theme::grid);
                g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
                if (m == 1 || m == 2 || m == 5)
                {
                    g.setColour (Theme::textDim);
                    g.drawText (formatAxisValue (v, viewY.format, false), 0, (int) y - 8, (int) r.getX() - 6, 16,
                                juce::Justification::centredRight);
                }
            }
        }
    }
    else
    {
        const double step = niceStep (viewY.max - viewY.min, juce::jmax (2, (int) (r.getHeight() / 40)));
        for (double v = std::ceil (viewY.min / step) * step; v <= viewY.max + step * 1e-6; v += step)
        {
            const float y = yToPx (v);
            g.setColour (std::abs (v) < step * 1e-6 ? Theme::gridMajor : Theme::grid);
            g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
            g.setColour (Theme::textDim);
            g.drawText (formatAxisValue (v, viewY.format, false), 0, (int) y - 8, (int) r.getX() - 6, 16,
                        juce::Justification::centredRight);
        }
    }

    g.setColour (Theme::gridMajor);
    g.drawRect (r, 1.0f);
}

String PlotBase::readoutAt (juce::Point<float> p) const
{
    return formatAxisValue (pxToX (p.x), viewX.format, true) + "   " + formatAxisValue (pxToY (p.y), viewY.format, true);
}

void PlotBase::paintOverlay (Graphics& g)
{
    const auto r = plotArea();

    if (! info.isEmpty())
    {
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
        g.setColour (Theme::text);

        for (int i = 0; i < info.size(); ++i)
            g.drawText (info[i], juce::Rectangle<float> (r.getX() + 4.0f, 4.0f + 15.0f * (float) i, r.getWidth() - 8.0f, 15.0f),
                        juce::Justification::centredLeft, true);
    }

    if (dragging)
    {
        const auto rect = juce::Rectangle<float> (dragStart, dragNow);
        const bool zoomIn = dragNow.x > dragStart.x && dragNow.y > dragStart.y;
        g.setColour ((zoomIn ? Theme::accent : Theme::textDim).withAlpha (0.15f));
        g.fillRect (rect);
        g.setColour (zoomIn ? Theme::accent : Theme::textDim);
        g.drawRect (rect, 1.0f);
    }
    else if (r.contains (hover))
    {
        g.setColour (Theme::textDim.withAlpha (0.35f));
        g.drawVerticalLine ((int) hover.x, r.getY(), r.getBottom());
        g.drawHorizontalLine ((int) hover.y, r.getX(), r.getRight());

        const auto text = readoutAt (hover);
        g.setFont (juce::FontOptions (12.0f));
        const int w = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 12;
        auto box = juce::Rectangle<float> (hover.x + 10.0f, hover.y - 22.0f, (float) w, 18.0f);
        if (box.getRight() > r.getRight()) box.setX (hover.x - 10.0f - (float) w);
        if (box.getY() < r.getY()) box.setY (hover.y + 6.0f);
        g.setColour (Theme::panel.withAlpha (0.92f));
        g.fillRoundedRectangle (box, 3.0f);
        g.setColour (Theme::text);
        g.drawText (text, box, juce::Justification::centred);
    }
}

void PlotBase::mouseDown (const juce::MouseEvent& e)
{
    dragStart = dragNow = e.position;
    dragging = plotArea().contains (e.position);
}

void PlotBase::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging)
    {
        dragNow = e.position;
        repaint();
    }
}

void PlotBase::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;

    dragging = false;
    const auto d = dragNow - dragStart;

    if (d.x > 6 && d.y > 6)
    {
        const double x0 = pxToX (dragStart.x), x1 = pxToX (dragNow.x);
        const double y0 = pxToY (dragNow.y), y1 = pxToY (dragStart.y);
        viewX.min = x0; viewX.max = x1;
        viewY.min = y0; viewY.max = y1;
        zoomed = true;
        viewChanged();
    }
    else if (d.x < -6 && d.y < -6)
    {
        resetZoom();
    }
    else
    {
        repaint();
    }
}

void PlotBase::mouseMove (const juce::MouseEvent& e)
{
    hover = e.position;
    repaint();
}

void PlotBase::mouseExit (const juce::MouseEvent&)
{
    hover = { -1.0f, -1.0f };
    repaint();
}

void PlotBase::mouseDoubleClick (const juce::MouseEvent&)
{
    resetZoom();
}

void PlotBase::zoomAxis (Axis& view, const Axis& def, double centreNorm, double factor)
{
    juce::ignoreUnused (def);
    const double a = fwd (view.min, view.log), b = fwd (view.max, view.log);
    const double c = a + centreNorm * (b - a);
    view.min = inv (c - (c - a) * factor, view.log);
    view.max = inv (c + (b - c) * factor, view.log);
    zoomed = true;
}

void PlotBase::panAxis (Axis& view, double deltaNorm)
{
    const double a = fwd (view.min, view.log), b = fwd (view.max, view.log);
    const double shift = deltaNorm * (b - a);
    view.min = inv (a + shift, view.log);
    view.max = inv (b + shift, view.log);
    zoomed = true;
}

void PlotBase::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const auto r = plotArea();
    const bool leftZone = e.position.x < r.getX();
    const bool bottomZone = e.position.y > r.getBottom();
    const double nx = (e.position.x - r.getX()) / r.getWidth();
    const double ny = (r.getBottom() - e.position.y) / r.getHeight();

    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        const double factor = std::pow (1.15, -w.deltaY * 8.0);
        if (leftZone || e.mods.isShiftDown())
            zoomAxis (viewY, defY, ny, factor);
        else
            zoomAxis (viewX, defX, nx, factor);
    }
    else
    {
        if (leftZone)
            panAxis (viewY, w.deltaY * 0.5);
        else if (bottomZone)
            panAxis (viewX, -w.deltaY * 0.5);
        else
        {
            panAxis (viewX, -w.deltaX * 0.5);
            panAxis (viewY, w.deltaY * 0.5);
        }
    }

    viewChanged();
}

void PlotBase::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    const auto r = plotArea();
    const double factor = 1.0 / juce::jmax (0.01f, scaleFactor);

    if (e.position.x < r.getX())
        zoomAxis (viewY, defY, (r.getBottom() - e.position.y) / r.getHeight(), factor);
    else
        zoomAxis (viewX, defX, (e.position.x - r.getX()) / r.getWidth(), factor);

    viewChanged();
}

//==============================================================================
void LinePlot::drawSeries (Graphics& g, const Series& s, juce::Rectangle<float> area)
{
    juce::Path p;
    bool started = false;
    const size_t n = juce::jmin (s.x.size(), s.y.size());

    for (size_t i = 0; i < n; ++i)
    {
        if (viewX.log && s.x[i] <= 0.0f)
            continue;

        const float px = xToPx (s.x[i]);
        const float py = juce::jlimit (area.getY() - 2000.0f, area.getBottom() + 2000.0f, yToPx (s.y[i]));

        if (! std::isfinite (px) || ! std::isfinite (py))
        {
            started = false;
            continue;
        }

        if (! started) { p.startNewSubPath (px, py); started = true; }
        else           p.lineTo (px, py);
    }

    g.setColour (s.colour);

    if (s.dashed)
    {
        juce::Path dashed;
        const float dashes[] = { 5.0f, 4.0f };
        juce::PathStrokeType (s.thickness).createDashedStroke (dashed, p, dashes, 2);
        g.fillPath (dashed);
    }
    else
    {
        g.strokePath (p, juce::PathStrokeType (s.thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void LinePlot::paint (Graphics& g)
{
    g.fillAll (Theme::panel);
    const auto r = plotArea();
    g.setColour (Theme::plotBg);
    g.fillRect (r);
    paintGridAndAxes (g);

    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (r.toNearestInt());

        for (auto& s : stored)
            drawSeries (g, s, r);
        for (auto& s : series)
            drawSeries (g, s, r);
    }

    // Legend
    g.setFont (juce::FontOptions (12.0f));
    float y = r.getY() + 8.0f;
    auto legendEntry = [&] (const Series& s)
    {
        if (s.label.isEmpty())
            return;
        g.setColour (s.colour);
        g.fillRect (r.getX() + 10.0f, y + 6.0f, 14.0f, 3.0f);
        g.setColour (Theme::text);
        g.drawText (s.label, (int) r.getX() + 30, (int) y, 220, 16, juce::Justification::centredLeft);
        y += 16.0f;
    };

    for (auto& s : series) legendEntry (s);
    for (auto& s : stored) legendEntry (s);

    paintOverlay (g);
}

//==============================================================================
void HeatmapPlot::setData (const Heatmap* map)
{
    hasData = map != nullptr && map->numX > 0 && map->numY > 0;
    if (hasData)
        data = *map;
    imageDirty = true;
    repaint();
}

void HeatmapPlot::setRange (float minDb, float maxDb)
{
    rangeMin = minDb;
    rangeMax = juce::jmax (minDb + 1.0f, maxDb);
    imageDirty = true;
    repaint();
}

void HeatmapPlot::setLogFrequency (bool shouldBeLog)
{
    auto y = defY;
    y.log = shouldBeLog;
    if (shouldBeLog && y.min <= 0.0)
        y.min = 20.0;
    setAxes (defX, y);
}

float HeatmapPlot::valueAt (double t, double f) const
{
    if (! hasData)
        return -999.0f;

    const double fx = (t - data.xMin) / juce::jmax (1.0e-9, (double) (data.xMax - data.xMin)) * (data.numX - 1);
    const int xi = (int) std::lround (fx);
    const int yi = (int) std::lround (f / juce::jmax (1.0e-9, (double) data.yMax) * data.numY);

    if (xi < 0 || xi >= data.numX || yi < 0 || yi >= data.numY)
        return -999.0f;

    return data.db[(size_t) xi * (size_t) data.numY + (size_t) yi];
}

void HeatmapPlot::rebuildImage()
{
    imageDirty = false;
    const auto r = plotArea().toNearestInt();

    if (r.isEmpty())
    {
        image = {};
        return;
    }

    if (! lutReady)
    {
        juce::ColourGradient grad (Colour (0xff0b0d2e), 0, 0, Colour (0xffe8222a), 1, 0, false);
        grad.addColour (0.18, Colour (0xff1f3fbf));
        grad.addColour (0.38, Colour (0xff18b5c9));
        grad.addColour (0.55, Colour (0xff3ccf5a));
        grad.addColour (0.72, Colour (0xfff2e43a));
        grad.addColour (0.86, Colour (0xfff2752a));
        for (int i = 0; i < 256; ++i)
            lut[(size_t) i] = grad.getColourAtPosition (i / 255.0);
        lutReady = true;
    }

    image = juce::Image (juce::Image::RGB, r.getWidth(), r.getHeight(), true);

    if (! hasData)
        return;

    juce::Image::BitmapData bd (image, juce::Image::BitmapData::writeOnly);
    const double binHz = data.yMax / (double) data.numY;
    const float scale = 255.0f / (rangeMax - rangeMin);

    std::vector<int> rowLo ((size_t) r.getHeight()), rowHi ((size_t) r.getHeight());
    for (int row = 0; row < r.getHeight(); ++row)
    {
        const double fTop = pxToY ((float) (r.getY() + row));
        const double fBot = pxToY ((float) (r.getY() + row + 1));
        int lo = (int) std::floor (juce::jmin (fTop, fBot) / binHz + 0.5);
        int hi = (int) std::floor (juce::jmax (fTop, fBot) / binHz + 0.5);
        rowLo[(size_t) row] = lo;
        rowHi[(size_t) row] = juce::jmax (lo, hi);
    }

    for (int col = 0; col < r.getWidth(); ++col)
    {
        const double t = pxToX ((float) r.getX() + (float) col + 0.5f);
        const double fx = (t - data.xMin) / juce::jmax (1.0e-9, (double) (data.xMax - data.xMin)) * (data.numX - 1);
        const int xi = (int) std::lround (fx);

        if (xi < 0 || xi >= data.numX)
            continue;

        const float* column = data.db.data() + (size_t) xi * (size_t) data.numY;

        for (int row = 0; row < r.getHeight(); ++row)
        {
            const int lo = juce::jmax (0, rowLo[(size_t) row]);
            const int hi = juce::jmin (data.numY - 1, rowHi[(size_t) row]);
            if (lo > hi)
                continue;

            float v = column[lo];
            for (int k = lo + 1; k <= hi; ++k)
                v = juce::jmax (v, column[k]);

            const int idx = juce::jlimit (0, 255, (int) ((v - rangeMin) * scale));
            bd.setPixelColour (col, row, lut[(size_t) idx]);
        }
    }
}

void HeatmapPlot::paint (Graphics& g)
{
    g.fillAll (Theme::panel);

    if (imageDirty)
        rebuildImage();

    const auto r = plotArea();
    g.setColour (Theme::plotBg);
    g.fillRect (r);

    if (image.isValid())
        g.drawImageAt (image, (int) r.getX(), (int) r.getY());

    paintGridAndAxes (g);

    if (! hasData)
    {
        g.setColour (Theme::textDim);
        g.drawText ("Measuring...", r, juce::Justification::centred);
    }

    paintOverlay (g);
}

String HeatmapPlot::readoutAt (juce::Point<float> p) const
{
    const double t = pxToX (p.x), f = pxToY (p.y);
    const float v = valueAt (t, f);
    return formatAxisValue (t, viewX.format, true) + "   " + formatAxisValue (f, AxisFormat::Hz, true)
         + (v > -999.0f ? "   " + String (v, 1) + " dB" : String());
}

//==============================================================================
RangeBar::RangeBar()
{
    slider.setRange (-250.0, 20.0, 1.0);
    slider.setMinAndMaxValues (-160.0, 0.0, juce::dontSendNotification);
    slider.setColour (juce::Slider::trackColourId, Theme::accent.withAlpha (0.4f));
    slider.onValueChange = [this]
    {
        if (onChange)
            onChange ((float) slider.getMinValue(), (float) slider.getMaxValue());
        repaint();
    };
    addAndMakeVisible (slider);
}

void RangeBar::setRange (float lo, float hi)
{
    slider.setMinAndMaxValues (lo, hi, juce::dontSendNotification);
    repaint();
}

void RangeBar::resized()
{
    slider.setBounds (getLocalBounds().withTrimmedTop (18).withTrimmedBottom (18).removeFromRight (getWidth() / 2));
}

void RangeBar::paint (Graphics& g)
{
    auto area = getLocalBounds().withTrimmedTop (18).withTrimmedBottom (18);
    auto strip = area.removeFromLeft (getWidth() / 2).reduced (4, 8).toFloat();
    juce::ColourGradient grad (Colour (0xffe8222a), strip.getX(), strip.getY(),
                               Colour (0xff0b0d2e), strip.getX(), strip.getBottom(), false);
    grad.addColour (0.14, Colour (0xfff2752a));
    grad.addColour (0.28, Colour (0xfff2e43a));
    grad.addColour (0.45, Colour (0xff3ccf5a));
    grad.addColour (0.62, Colour (0xff18b5c9));
    grad.addColour (0.82, Colour (0xff1f3fbf));
    g.setGradientFill (grad);
    g.fillRect (strip);

    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (String ((int) slider.getMaxValue()) + " dB", getLocalBounds().removeFromTop (16), juce::Justification::centred);
    g.drawText (String ((int) slider.getMinValue()) + " dB", getLocalBounds().removeFromBottom (16), juce::Justification::centred);
}

} // namespace pa
