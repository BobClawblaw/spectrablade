#include "GraphicEq.h"
#include "Theme.h"

namespace GraphicEq
{
const std::vector<double>& centres (int numBands)
{
    static const std::vector<double> octaves { 31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };
    static const std::vector<double> thirds { 20.0, 25.0, 31.5, 40.0, 50.0, 63.0, 80.0, 100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0, 500.0, 630.0,
                                              800.0, 1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0, 4000.0, 5000.0, 6300.0, 8000.0, 10000.0,
                                              12500.0, 16000.0, 20000.0 };
    return numBands == 31 ? thirds : octaves;
}

double bandQ (int numBands)
{
    // A bell spanning the octaves between its neighbours: Q = sqrt(2^w) / (2^w - 1).
    const auto ratio = std::pow (2.0, numBands == 31 ? 1.0 / 3.0 : 1.0);
    return std::sqrt (ratio) / (ratio - 1.0);
}

bool isValid (const std::vector<double>& gains)
{
    if (gains.size() != 10 && gains.size() != 31)
        return false;

    for (auto g : gains)
        if (! (std::abs (g) <= maxGainDb))
            return false;

    return true;
}

std::vector<Equalizer::Band> bandsFor (const std::vector<double>& gains, double sampleRate)
{
    std::vector<Equalizer::Band> bands;

    if (! isValid (gains))
        return bands;

    const auto& f = centres ((int) gains.size());
    const auto q = bandQ ((int) gains.size());

    for (size_t i = 0; i < gains.size(); ++i)
    {
        const Equalizer::Band band { Equalizer::BandType::peak, f[i], gains[i], q };

        if (! Equalizer::isFlat (band) && Equalizer::isValid (band, sampleRate))
            bands.push_back (band);
    }

    return bands;
}

AudioEdits::Snapshot apply (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const std::vector<double>& gains)
{
    const auto bands = bandsFor (gains, sampleRate);
    return bands.empty() ? nullptr : Equalizer::apply (s, start, end, sampleRate, bands);
}

//==============================================================================
namespace
{
    // Each band's colour: warm at the bottom of the range to cool at the top.
    juce::Colour colourFor (int band, int numBands)
    {
        const auto t = numBands > 1 ? (float) band / (float) (numBands - 1) : 0.0f;
        return juce::Colour::fromHSV (0.02f + 0.68f * t, 0.62f, 0.98f, 1.0f);
    }

    juce::String frequencyLabel (double hz)
    {
        return hz >= 1000.0 ? juce::String (hz / 1000.0) + "k" : juce::String (hz);   // 31.5, 63, 1k, 12.5k
    }
}

Panel::Panel (int numBands)
    : gains ((size_t) centres (numBands).size(), 0.0)
{
    flat.onClick = [this]
    {
        setGains (std::vector<double> (gains.size(), 0.0));
        changed();
    };
    addAndMakeVisible (flat);
    setSize (60 + numBands * (numBands == 31 ? 30 : 52), 300);
}

std::vector<double> Panel::getGains() const
{
    return gains;
}

void Panel::setGains (const std::vector<double>& g)
{
    for (size_t i = 0; i < gains.size() && i < g.size(); ++i)
        gains[i] = juce::jlimit (-maxGainDb, maxGainDb, g[i]);

    repaint();
}

void Panel::changed()
{
    repaint();

    if (onChange != nullptr)
        onChange();
}

void Panel::setBand (int band, double db)
{
    if (juce::isPositiveAndBelow (band, (int) gains.size()))
        gains[(size_t) band] = juce::jlimit (-maxGainDb, maxGainDb, std::round (db * 10.0) / 10.0);
}

juce::Rectangle<float> Panel::getBarArea() const
{
    return getLocalBounds().toFloat().reduced (6.0f).withTrimmedLeft (34.0f).withTrimmedBottom (48.0f).withTrimmedTop (6.0f);
}

float Panel::xForBand (int band) const
{
    const auto area = getBarArea();
    const auto width = area.getWidth() / (float) juce::jmax ((size_t) 1, gains.size());
    return area.getX() + width * ((float) band + 0.5f);
}

int Panel::bandAtX (float x) const
{
    const auto area = getBarArea();
    const auto width = area.getWidth() / (float) juce::jmax ((size_t) 1, gains.size());
    return juce::jlimit (0, (int) gains.size() - 1, (int) std::floor ((x - area.getX()) / width));
}

float Panel::yForGain (double db) const
{
    const auto area = getBarArea();
    return area.getCentreY() - (float) (juce::jlimit (-maxGainDb, maxGainDb, db) / maxGainDb) * area.getHeight() / 2.0f;
}

double Panel::gainForY (float y) const
{
    const auto area = getBarArea();
    const auto db = (double) ((area.getCentreY() - y) / (area.getHeight() / 2.0f)) * maxGainDb;
    return juce::jlimit (-maxGainDb, maxGainDb, std::round (db * 10.0) / 10.0);
}

float Panel::xForFrequency (double hz) const
{
    const auto& f = centres ((int) gains.size());
    const auto n = (int) f.size();

    if (hz <= f.front())
        return xForBand (0) - (float) (std::log (f.front() / hz) / std::log (f[1] / f[0])) * (xForBand (1) - xForBand (0));

    for (int i = 0; i + 1 < n; ++i)
        if (hz <= f[(size_t) i + 1])
            return xForBand (i) + (float) (std::log (hz / f[(size_t) i]) / std::log (f[(size_t) i + 1] / f[(size_t) i])) * (xForBand (i + 1) - xForBand (i));

    return xForBand (n - 1) + (float) (std::log (hz / f.back()) / std::log (f[(size_t) n - 1] / f[(size_t) n - 2])) * (xForBand (n - 1) - xForBand (n - 2));
}

void Panel::mouseMove (const juce::MouseEvent& e)
{
    const auto now = getBarArea().expanded (0.0f, 4.0f).contains (e.position) ? bandAtX (e.position.x) : -1;

    if (now != hovered)
    {
        hovered = now;
        repaint();
    }
}

void Panel::mouseExit (const juce::MouseEvent&)
{
    hovered = -1;
    repaint();
}

void Panel::mouseDown (const juce::MouseEvent& e)
{
    if (! getBarArea().expanded (0.0f, 6.0f).contains (e.position))
        return;

    last = e.position;
    hovered = bandAtX (e.position.x);
    setBand (hovered, gainForY (e.position.y));
    changed();
}

void Panel::mouseDrag (const juce::MouseEvent& e)
{
    if (! getBarArea().expanded (0.0f, 6.0f).contains (e.mouseDownPosition))
        return;

    // Every band between the last step and this one, along the line between them: a quick stroke
    // across skips none.
    const auto from = bandAtX (last.x), to = bandAtX (e.position.x);
    const auto step = from <= to ? 1 : -1;

    for (auto band = from; ; band += step)
    {
        const auto x = xForBand (band);
        const auto span = e.position.x - last.x;
        const auto t = std::abs (span) < 1.0e-3f ? 1.0f : juce::jlimit (0.0f, 1.0f, (x - last.x) / span);
        setBand (band, gainForY (from == to ? e.position.y : last.y + t * (e.position.y - last.y)));

        if (band == to)
            break;
    }

    last = e.position;
    hovered = to;
    changed();
}

void Panel::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! getBarArea().expanded (0.0f, 6.0f).contains (e.position))
        return;

    setBand (bandAtX (e.position.x), 0.0);
    changed();
}

void Panel::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! getBarArea().contains (e.position) || juce::exactlyEqual (wheel.deltaY, 0.0f))
    {
        Component::mouseWheelMove (e, wheel);
        return;
    }

    const auto band = bandAtX (e.position.x);
    const auto step = e.mods.isShiftDown() ? 0.1 : 0.5;
    setBand (band, gains[(size_t) band] + (wheel.deltaY > 0.0f ? step : -step));
    changed();
}

void Panel::paint (juce::Graphics& g)
{
    const auto area = getBarArea();
    const auto n = (int) gains.size();
    const auto& f = centres (n);
    const auto zero = yForGain (0.0);
    const auto column = area.getWidth() / (float) juce::jmax (1, n);

    g.fillAll (Theme::base);
    g.setGradientFill (juce::ColourGradient (Theme::canvas.brighter (0.06f), area.getX(), area.getY(),
                                             Theme::canvas.darker (0.25f), area.getX(), area.getBottom(), false));
    g.fillRoundedRectangle (area.expanded (2.0f), 6.0f);

    g.setFont (juce::FontOptions (10.5f));

    for (auto db : { -12.0, -6.0, 0.0, 6.0, 12.0 })
    {
        const auto y = yForGain (db);
        g.setColour (Theme::overlay.withAlpha (juce::exactlyEqual (db, 0.0) ? 0.22f : 0.07f));
        g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
        g.setColour (Theme::dimText);
        g.drawText ((db > 0.0 ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (area.getX() - 34.0f, y - 7.0f, 28.0f, 14.0f),
                    juce::Justification::right);
    }

    // The bars, from the 0 dB line to each gain, brighter toward their end, with a cap at the gain.
    for (int i = 0; i < n; ++i)
    {
        const auto colour = colourFor (i, n);
        const auto x = xForBand (i);
        const auto y = yForGain (gains[(size_t) i]);
        const auto bar = juce::Rectangle<float> (x - column * 0.32f, juce::jmin (y, zero), column * 0.64f, std::abs (y - zero));
        const auto strong = i == hovered;

        if (strong)
        {
            g.setColour (Theme::overlay.withAlpha (0.06f));
            g.fillRect (juce::Rectangle<float> (x - column / 2.0f, area.getY(), column, area.getHeight()));
        }

        if (bar.getHeight() > 0.5f)
        {
            g.setGradientFill (juce::ColourGradient (colour.withAlpha (strong ? 0.85f : 0.65f), x, y, colour.withAlpha (0.12f), x, zero, false));
            g.fillRoundedRectangle (bar, juce::jmin (3.0f, bar.getWidth() / 3.0f));
        }

        g.setColour (colour.withAlpha (strong ? 0.35f : 0.18f));
        g.fillRoundedRectangle (juce::Rectangle<float> (column * 0.72f + 6.0f, 9.0f).withCentre ({ x, y }), 4.5f);
        g.setColour (colour);
        g.fillRoundedRectangle (juce::Rectangle<float> (column * 0.72f, 5.0f).withCentre ({ x, y }), 2.5f);

        // Every frequency when there is room, every other one when there isn't.
        if (column >= 26.0f || i % 2 == 0)
        {
            g.setColour (strong ? Theme::textStrong : Theme::dimText);
            g.drawText (frequencyLabel (f[(size_t) i]), juce::Rectangle<float> (x - 22.0f, area.getBottom() + 6.0f, 44.0f, 14.0f), juce::Justification::centred);
        }
    }

    // The response the bands make together (drawn at 48 kHz), glowing over the bars.
    juce::Path curve;
    const auto bands = bandsFor (gains, 48000.0);
    auto started = false;

    for (auto x = area.getX(); x <= area.getRight(); x += 1.0f)
    {
        // The frequency at this x: the inverse of xForFrequency, found by bisection on its log.
        auto lo = std::log (10.0), hi = std::log (24000.0);

        for (int k = 0; k < 30; ++k)
        {
            const auto mid = 0.5 * (lo + hi);
            (xForFrequency (std::exp (mid)) < x ? lo : hi) = mid;
        }

        const auto hz = std::exp (0.5 * (lo + hi));
        auto db = 0.0;

        for (const auto& b : bands)
            db += Equalizer::responseDb (b, 48000.0, hz);

        const auto y = yForGain (db);
        started ? curve.lineTo (x, y) : curve.startNewSubPath (x, y);
        started = true;
    }

    const auto glow = Theme::accentLight;
    g.setColour (glow.withAlpha (0.12f));
    g.strokePath (curve, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (glow.interpolatedWith (juce::Colours::white, 0.5f).withAlpha (0.9f));
    g.strokePath (curve, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (hovered >= 0)
    {
        const auto db = gains[(size_t) hovered];
        const auto text = frequencyLabel (f[(size_t) hovered]) + " Hz  " + (db > 0.0 ? "+" : "") + juce::String (db, 1) + " dB";
        const juce::Font font (juce::FontOptions (12.0f));
        const auto width = juce::GlyphArrangement::getStringWidth (font, text) + 16.0f;
        const auto x = juce::jlimit (area.getX() + 2.0f, area.getRight() - width - 2.0f, xForBand (hovered) - width / 2.0f);
        const auto box = juce::Rectangle<float> (x, area.getY() + 4.0f, width, 22.0f);
        g.setColour (Theme::inset.withAlpha (0.92f));
        g.fillRoundedRectangle (box, 5.0f);
        g.setColour (colourFor (hovered, n).withAlpha (0.8f));
        g.drawRoundedRectangle (box, 5.0f, 1.0f);
        g.setColour (Theme::textStrong);
        g.setFont (font);
        g.drawText (text, box, juce::Justification::centred);
    }
}

void Panel::resized()
{
    flat.setBounds (getLocalBounds().reduced (6).removeFromBottom (24).removeFromRight (70));
}
}
