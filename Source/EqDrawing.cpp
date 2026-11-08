#include "EqDrawing.h"
#include "Analysis.h"
#include "Theme.h"

namespace EqDrawing
{
juce::Colour bandColour (int index)
{
    // Five hues spread round the wheel, bright enough to read on a dark or a light drawing.
    static const juce::Colour colours[] { juce::Colour (0xffff7a59), juce::Colour (0xfff2c14e), juce::Colour (0xff5fd38d),
                                          juce::Colour (0xff4fb3f6), juce::Colour (0xffb58cf5) };
    return colours[(size_t) (juce::jmax (0, index) % 5)];
}

float xForFrequency (juce::Rectangle<float> area, double frequency)
{
    return area.getX() + area.getWidth() * (float) (std::log (frequency / minHz) / std::log (maxHz / minHz));
}

double frequencyForX (juce::Rectangle<float> area, float x)
{
    return minHz * std::pow (maxHz / minHz, (double) ((x - area.getX()) / area.getWidth()));
}

void HeldSpectrum::take (const std::vector<float>& reading, double sampleRate)
{
    if (levels.size() != reading.size() || ! juce::exactlyEqual (rate, sampleRate))
        levels.assign (reading.size(), -150.0f);

    rate = sampleRate;

    for (size_t i = 0; i < reading.size(); ++i)
        levels[i] = juce::jmax (reading[i], levels[i] - 0.6f);
}

bool HeldSpectrum::read (const std::function<double (juce::AudioBuffer<float>&)>& reader, juce::AudioBuffer<float>& scratch)
{
    if (reader == nullptr)
        return false;

    scratch.setSize (2, Analysis::fftSize, false, false, true);
    const auto sampleRate = reader (scratch);

    if (sampleRate <= 0.0)
        return false;

    std::vector<float> mono ((size_t) Analysis::fftSize);

    for (int i = 0; i < Analysis::fftSize; ++i)
        mono[(size_t) i] = 0.5f * (scratch.getSample (0, i) + scratch.getSample (1, i));

    take (Analysis::spectrumDb (mono.data(), Analysis::fftSize), sampleRate);
    return true;
}

void paintWell (juce::Graphics& g, juce::Rectangle<float> area)
{
    g.setGradientFill (juce::ColourGradient (Theme::canvas.brighter (0.06f), area.getX(), area.getY(),
                                             Theme::canvas.darker (0.25f), area.getX(), area.getBottom(), false));
    g.fillRoundedRectangle (area, 6.0f);
}

void paintSpectrum (juce::Graphics& g, juce::Rectangle<float> area, const HeldSpectrum& held)
{
    const auto& spectrum = held.levels;

    if (spectrum.empty() || held.rate <= 0.0)
        return;

    constexpr float floorDb = -90.0f;
    juce::Path shape;
    shape.startNewSubPath (area.getX(), area.getBottom());

    const auto binOf = [&held] (double hz) { return hz * Analysis::fftSize / held.rate; };
    const auto last = (int) spectrum.size() - 1;

    for (auto x = area.getX(); x <= area.getRight(); x += 1.5f)
    {
        const auto from = binOf (frequencyForX (area, x)), to = binOf (frequencyForX (area, x + 1.5f));
        auto level = -150.0f;

        if (to - from < 1.0)
        {
            const auto below = juce::jlimit (0, last, (int) std::floor (from));
            const auto above = juce::jmin (last, below + 1);
            level = juce::jmap ((float) (from - std::floor (from)), spectrum[(size_t) below], spectrum[(size_t) above]);
        }
        else
        {
            for (auto b = juce::jlimit (0, last, (int) std::floor (from)); b <= juce::jlimit (0, last, (int) std::ceil (to)); ++b)
                level = juce::jmax (level, spectrum[(size_t) b]);
        }

        shape.lineTo (x, area.getBottom() - area.getHeight() * juce::jlimit (0.0f, 1.0f, (level - floorDb) / -floorDb));
    }

    shape.lineTo (area.getRight(), area.getBottom());
    shape.closeSubPath();
    g.setGradientFill (juce::ColourGradient (Theme::overlay.withAlpha (0.16f), area.getX(), area.getY(),
                                             Theme::overlay.withAlpha (0.03f), area.getX(), area.getBottom(), false));
    g.fillPath (shape);
    g.setColour (Theme::overlay.withAlpha (0.22f));
    g.strokePath (shape, juce::PathStrokeType (1.0f));
}

void paintGrid (juce::Graphics& g, juce::Rectangle<float> area, double rangeDb)
{
    g.setFont (juce::FontOptions (10.5f));

    for (auto hz : { 30.0, 50.0, 100.0, 200.0, 300.0, 500.0, 1000.0, 2000.0, 3000.0, 5000.0, 10000.0 })
    {
        const auto x = xForFrequency (area, hz);
        const auto major = hz == 100.0 || hz == 1000.0 || hz == 10000.0;
        g.setColour (Theme::overlay.withAlpha (major ? 0.12f : 0.06f));
        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        g.setColour (Theme::dimText);
        g.drawText (hz >= 1000.0 ? juce::String ((int) (hz / 1000.0)) + "k" : juce::String ((int) hz),
                    juce::Rectangle<float> (x - 15.0f, area.getBottom() - 15.0f, 30.0f, 12.0f), juce::Justification::centred);
    }

    for (auto db = -rangeDb + 6.0; db < rangeDb - 1.0; db += 6.0)
    {
        const auto y = area.getCentreY() - (float) (db / rangeDb) * area.getHeight() / 2.0f;
        g.setColour (Theme::overlay.withAlpha (std::abs (db) < 0.5 ? 0.22f : 0.06f));
        g.drawHorizontalLine ((int) y, area.getX(), area.getRight());

        if (std::abs (db) > 3.0 && std::fmod (std::abs (db), 12.0) < 0.5)
        {
            g.setColour (Theme::dimText);
            g.drawText ((db > 0.0 ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (area.getRight() - 30.0f, y - 7.0f, 26.0f, 14.0f),
                        juce::Justification::right);
        }
    }
}

void paintGlow (juce::Graphics& g, const juce::Path& curve)
{
    const auto glow = Theme::accentLight;
    g.setColour (glow.withAlpha (0.10f));
    g.strokePath (curve, juce::PathStrokeType (9.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (glow.withAlpha (0.22f));
    g.strokePath (curve, juce::PathStrokeType (4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (glow.interpolatedWith (juce::Colours::white, 0.5f));
    g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void paintHandle (juce::Graphics& g, juce::Point<float> p, int index, bool strong, bool picked)
{
    const auto colour = bandColour (index);
    const auto radius = strong ? 9.5f : 8.0f;
    g.setColour (colour.withAlpha (strong ? 0.30f : 0.16f));
    g.fillEllipse (juce::Rectangle<float> (radius * 3.2f, radius * 3.2f).withCentre (p));
    g.setColour (colour);
    g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (p));

    if (picked)
    {
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.drawEllipse (juce::Rectangle<float> (radius * 2.0f + 5.0f, radius * 2.0f + 5.0f).withCentre (p), 1.5f);
    }

    g.setColour (juce::Colour (0xff15171c));
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText (juce::String (index + 1), juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (p), juce::Justification::centred);
}

void paintChannelTag (juce::Graphics& g, juce::Point<float> p, int index, Equalizer::Channel channel)
{
    if (channel == Equalizer::Channel::all)
        return;

    const auto box = juce::Rectangle<float> (13.0f, 13.0f).withCentre (p.translated (11.0f, -11.0f));
    g.setColour (juce::Colour (0xff15171c).withAlpha (0.85f));
    g.fillRoundedRectangle (box, 3.0f);
    g.setColour (bandColour (index));
    g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
    g.drawText (Equalizer::channelNames()[(int) channel].substring (0, 1), box, juce::Justification::centred);
}

void paintOffRing (juce::Graphics& g, juce::Point<float> p, int index, bool strong)
{
    g.setColour (bandColour (index).withAlpha (strong ? 0.8f : 0.35f));
    g.drawEllipse (juce::Rectangle<float> (14.0f, 14.0f).withCentre (p), 1.5f);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText (juce::String (index + 1), juce::Rectangle<float> (14.0f, 14.0f).withCentre (p), juce::Justification::centred);
}

void paintReadout (juce::Graphics& g, juce::Rectangle<float> area, juce::Point<float> at, const juce::String& text, juce::Colour edge)
{
    const juce::Font font (juce::FontOptions (12.0f));
    const auto width = juce::GlyphArrangement::getStringWidth (font, text) + 16.0f;
    auto box = juce::Rectangle<float> (width, 22.0f).withCentre (at.translated (0.0f, at.y - area.getY() > 40.0f ? -30.0f : 30.0f));
    box = box.withX (juce::jlimit (area.getX() + 2.0f, area.getRight() - width - 2.0f, box.getX()));

    g.setColour (Theme::inset.withAlpha (0.92f));
    g.fillRoundedRectangle (box, 5.0f);
    g.setColour (edge.withAlpha (0.8f));
    g.drawRoundedRectangle (box, 5.0f, 1.0f);
    g.setColour (Theme::textStrong);
    g.setFont (font);
    g.drawText (text, box, juce::Justification::centred);
}
}
