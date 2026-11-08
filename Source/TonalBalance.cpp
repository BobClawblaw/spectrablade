#include "TonalBalance.h"
#include "Theme.h"
#include "EqDrawing.h"

namespace TonalBalance
{
const juce::StringArray& styleNames()
{
    static const juce::StringArray names { "Full mix", "Bass-heavy", "Bright", "Speech", "Orchestral" };
    return names;
}

namespace
{
    constexpr double lowestShown = 30.0, highestShown = 16000.0;

    // The centre curve for a style at a frequency: a slope against pink noise (-3 dB an octave here)
    // and the style's shelves, before the mean is taken out.
    double centreOf (Style style, double hz)
    {
        const auto octaves = std::log2 (hz / 1000.0);
        auto db = -3.0 * octaves;
        const auto shelf = [hz] (double corner, bool below, double amount)   // a smooth shelf, 6 dB an octave into its amount
        {
            const auto t = std::log2 (hz / corner) * (below ? -1.0 : 1.0);
            return amount / (1.0 + std::exp (-2.0 * t));
        };

        switch (style)
        {
            case Style::fullMix:    break;
            case Style::bassHeavy:  db += shelf (100.0, true, 3.0) - shelf (5000.0, false, 1.0); break;
            case Style::bright:     db += shelf (4000.0, false, 2.0) - shelf (100.0, true, 2.0); break;
            case Style::speech:     db -= shelf (100.0, true, 6.0) + shelf (8000.0, false, 6.0); break;
            case Style::orchestral: db -= 1.5 * octaves; break;
        }

        // The ends: rolling off below 40 Hz and above 12 kHz, 6 dB an octave.
        if (hz < 40.0)
            db -= 6.0 * std::log2 (40.0 / hz);

        if (hz > 12000.0)
            db -= 6.0 * std::log2 (hz / 12000.0);

        return db;
    }

    double toleranceOf (Style style)
    {
        return style == Style::speech || style == Style::orchestral ? 4.0 : 3.0;
    }
}

Target targetFor (Style style)
{
    std::array<double, MatchEq::numPoints> centre {};

    for (int i = 0; i < MatchEq::numPoints; ++i)
        centre[(size_t) i] = centreOf (style, MatchEq::frequencyOf (i));

    centre = relative (centre);
    Target t;
    const auto tolerance = toleranceOf (style);

    for (size_t i = 0; i < centre.size(); ++i)
    {
        t.low[i] = centre[i] - tolerance;
        t.high[i] = centre[i] + tolerance;
    }

    return t;
}

std::array<double, MatchEq::numPoints> relative (const std::array<double, MatchEq::numPoints>& levelDb)
{
    auto mean = 0.0;
    auto n = 0;

    for (int i = 0; i < MatchEq::numPoints; ++i)
        if (const auto hz = MatchEq::frequencyOf (i); hz >= 100.0 && hz <= 10000.0 && levelDb[(size_t) i] > -100.0)
        {
            mean += levelDb[(size_t) i];
            ++n;
        }

    if (n == 0)
        return levelDb;

    mean /= n;
    auto out = levelDb;

    for (auto& v : out)
        if (v > -100.0)
            v -= mean;

    return out;
}

double shareWithin (const std::array<double, MatchEq::numPoints>& r, const Target& t)
{
    auto within = 0, counted = 0;

    for (int i = 0; i < MatchEq::numPoints; ++i)
    {
        const auto hz = MatchEq::frequencyOf (i);

        if (hz < lowestShown || hz > highestShown || r[(size_t) i] <= -100.0)
            continue;

        ++counted;

        if (r[(size_t) i] >= t.low[(size_t) i] && r[(size_t) i] <= t.high[(size_t) i])
            ++within;
    }

    return counted > 0 ? (double) within / counted : 0.0;
}

//==============================================================================
Panel::Panel()
    : target (targetFor (Style::fullMix))
{
    styles.addItemList (styleNames(), 1);
    styles.setSelectedItemIndex (0, juce::dontSendNotification);
    styles.onChange = [this] { setStyle ((Style) juce::jmax (0, styles.getSelectedItemIndex())); };
    addAndMakeVisible (styles);
    shown.fill (-150.0);
    setSize (420, 220);
    startTimerHz (10);
}

Panel::~Panel()
{
    stopTimer();
}

void Panel::setStyle (Style s)
{
    style = s;
    target = targetFor (s);
    styles.setSelectedItemIndex ((int) s, juce::dontSendNotification);
    refresh();
}

void Panel::take (const std::vector<float>& db, double sampleRate)
{
    if (sampleRate <= 0.0 || db.empty())
        return;

    // Each point's power from the bins in its twelfth of an octave, as MatchEq measures.
    const auto binHz = sampleRate / Analysis::fftSize;
    const auto bins = (int) db.size();

    for (int i = 0; i < MatchEq::numPoints; ++i)
    {
        const auto centre = MatchEq::frequencyOf (i);
        const auto low = centre / std::pow (2.0, 0.5 / MatchEq::pointsPerOctave), high = centre * std::pow (2.0, 0.5 / MatchEq::pointsPerOctave);
        auto from = (int) std::floor (low / binHz), to = (int) std::ceil (high / binHz);

        if (to <= from)
            to = from + 1;

        from = juce::jlimit (0, bins - 1, from);
        to = juce::jlimit (from + 1, bins, to);
        auto sum = 0.0;

        for (int k = from; k < to; ++k)
            sum += std::pow (10.0, db[(size_t) k] / 10.0);

        const auto now = sum / (to - from);
        power[(size_t) i] = anything ? 0.9 * power[(size_t) i] + 0.1 * now : now;
    }

    anything = true;
    refresh();
}

void Panel::refresh()
{
    if (! anything)
    {
        share = 0.0;
        repaint();
        return;
    }

    std::array<double, MatchEq::numPoints> levels {};

    for (size_t i = 0; i < levels.size(); ++i)
        levels[i] = 10.0 * std::log10 (juce::jmax (1.0e-15, power[i]));

    shown = relative (levels);
    share = shareWithin (shown, target);
    repaint();
}

void Panel::timerCallback()
{
    if (readOutput == nullptr || ! isShowing())
        return;

    const auto sampleRate = readOutput (recent);

    if (sampleRate <= 0.0)
        return;

    std::vector<float> mono ((size_t) Analysis::fftSize);

    for (int i = 0; i < Analysis::fftSize; ++i)
        mono[(size_t) i] = 0.5f * (recent.getSample (0, i) + recent.getSample (1, i));

    take (Analysis::spectrumDb (mono.data(), Analysis::fftSize), sampleRate);
}

juce::Rectangle<float> Panel::plotArea() const
{
    return getLocalBounds().withTrimmedTop (30).reduced (8).toFloat();
}

void Panel::resized()
{
    styles.setBounds (getLocalBounds().removeFromTop (30).reduced (8, 4).removeFromLeft (150));
}

void Panel::paint (juce::Graphics& g)
{
    const auto area = plotArea();
    g.fillAll (Theme::base);
    EqDrawing::paintWell (g, area);

    // The scale: 30 Hz to 16 kHz across (log), -18 to +18 dB up.
    constexpr double rangeDb = 18.0;
    const auto xFor = [&area] (double hz) { return area.getX() + area.getWidth() * (float) (std::log (hz / lowestShown) / std::log (highestShown / lowestShown)); };
    const auto yFor = [&area] (double db) { return area.getCentreY() - (float) (juce::jlimit (-rangeDb, rangeDb, db) / rangeDb) * area.getHeight() / 2.0f; };

    g.setFont (juce::FontOptions (10.5f));

    for (auto hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
    {
        g.setColour (Theme::overlay.withAlpha (hz == 100.0 || hz == 1000.0 || hz == 10000.0 ? 0.12f : 0.06f));
        g.drawVerticalLine ((int) xFor (hz), area.getY(), area.getBottom());
        g.setColour (Theme::dimText);
        g.drawText (hz >= 1000.0 ? juce::String ((int) (hz / 1000.0)) + "k" : juce::String ((int) hz), juce::Rectangle<float> (xFor (hz) - 15.0f, area.getBottom() - 15.0f, 30.0f, 12.0f),
                    juce::Justification::centred);
    }

    for (auto db : { -12.0, -6.0, 0.0, 6.0, 12.0 })
    {
        g.setColour (Theme::overlay.withAlpha (juce::exactlyEqual (db, 0.0) ? 0.2f : 0.06f));
        g.drawHorizontalLine ((int) yFor (db), area.getX(), area.getRight());
        g.setColour (Theme::dimText);
        g.drawText ((db > 0.0 ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (area.getRight() - 30.0f, yFor (db) - 7.0f, 26.0f, 14.0f), juce::Justification::right);
    }

    // The target band, shaded, between its bounds.
    juce::Path band;
    auto started = false;

    for (int i = 0; i < MatchEq::numPoints; ++i)
    {
        const auto hz = MatchEq::frequencyOf (i);

        if (hz < lowestShown || hz > highestShown)
            continue;

        const auto p = juce::Point<float> (xFor (hz), yFor (target.high[(size_t) i]));
        started ? band.lineTo (p) : band.startNewSubPath (p);
        started = true;
    }

    for (int i = MatchEq::numPoints - 1; i >= 0; --i)
    {
        const auto hz = MatchEq::frequencyOf (i);

        if (hz >= lowestShown && hz <= highestShown)
            band.lineTo (xFor (hz), yFor (target.low[(size_t) i]));
    }

    band.closeSubPath();
    g.setColour (Theme::accentLight.withAlpha (0.16f));
    g.fillPath (band);
    g.setColour (Theme::accentLight.withAlpha (0.35f));
    g.strokePath (band, juce::PathStrokeType (1.0f));

    // The curve, in the warning colour where it leaves the band.
    if (anything)
    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (area.toNearestInt());

        for (int i = 1; i < MatchEq::numPoints; ++i)
        {
            const auto hz0 = MatchEq::frequencyOf (i - 1), hz1 = MatchEq::frequencyOf (i);

            if (hz0 < lowestShown || hz1 > highestShown || shown[(size_t) i - 1] <= -100.0 || shown[(size_t) i] <= -100.0)
                continue;

            const auto outside = shown[(size_t) i] < target.low[(size_t) i] || shown[(size_t) i] > target.high[(size_t) i];
            g.setColour (outside ? Theme::warn : Theme::textStrong);
            g.drawLine (xFor (hz0), yFor (shown[(size_t) i - 1]), xFor (hz1), yFor (shown[(size_t) i]), 2.0f);
        }
    }

    g.setColour (Theme::textStrong);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText (anything ? juce::String (juce::roundToInt (share * 100.0)) + " % within" : "No output yet", getLocalBounds().removeFromTop (30).reduced (8, 4).removeFromRight (120),
                juce::Justification::centredRight);
}
}
