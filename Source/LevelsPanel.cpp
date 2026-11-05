#include "LevelsPanel.h"
#include "Theme.h"

LevelsPanel::LevelsPanel()
{
    setTooltip ("What goes to the outputs. Click to reset the peaks and clip lights.");
    startTimerHz (30);
}

void LevelsPanel::take (float left, float right)
{
    const float peaks[2] { left, right };

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto peak = peaks[ch];
        level[ch] = juce::jmax (peak, level[ch] * 0.8f);
        highest[ch] = juce::jmax (highest[ch], peak);

        if (peak >= hold[ch])
        {
            hold[ch] = peak;
            holdTicks[ch] = 45;
        }
        else if (--holdTicks[ch] <= 0)
        {
            hold[ch] *= 0.9f;
        }

        if (peak >= 1.0f)
            clipped[ch] = true;
    }
}

juce::String LevelsPanel::readout (int channel) const
{
    const auto peak = highest[channel];

    if (peak <= 0.0f)
        return "-inf";

    const auto db = juce::Decibels::gainToDecibels (peak, -200.0f);
    return (db > 0.05f ? "+" : "") + juce::String (db, 1);
}

void LevelsPanel::reset()
{
    for (int ch = 0; ch < 2; ++ch)
    {
        highest[ch] = hold[ch] = 0.0f;
        clipped[ch] = false;
    }

    repaint();
}

float LevelsPanel::xFor (float decibels, float width)
{
    return width * juce::jlimit (0.0f, 1.0f, (decibels - floorDb) / -floorDb);
}

void LevelsPanel::timerCallback()
{
    if (readPeaks == nullptr)
        return;

    const auto [left, right] = readPeaks();
    take (left, right);
    repaint();
}

void LevelsPanel::paint (juce::Graphics& g)
{
    g.fillAll (Theme::base);
    auto r = getLocalBounds().reduced (10, 3);
    auto names = r.removeFromLeft (16);
    auto numbers = r.removeFromRight (64);
    r.removeFromRight (8);
    auto lights = r.removeFromRight (10);
    r.removeFromRight (6);
    // The scale goes when there is too little height for it and the bars.
    const auto showScale = r.getHeight() >= 30;
    auto scale = showScale ? r.removeFromBottom (14) : juce::Rectangle<int>();
    const auto bars = r;
    const auto barHeight = (float) juce::jmax (3, (bars.getHeight() - 4) / 2);
    const auto width = (float) bars.getWidth();

    const auto dbAt = [] (float gain) { return juce::Decibels::gainToDecibels (gain, -200.0f); };

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto y = (float) bars.getY() + (float) ch * (barHeight + 4.0f);
        const juce::Rectangle<float> bar ((float) bars.getX(), y, width, barHeight);
        g.setColour (Theme::inset);
        g.fillRoundedRectangle (bar, 2.0f);

        const auto filled = bar.withWidth (xFor (dbAt (level[ch]), width));
        g.setGradientFill (juce::ColourGradient::horizontal (Theme::meterLow, bar.getX(), Theme::meterMid,
                                                             bar.getX() + xFor (-6.0f, width)));
        g.fillRect (filled.withWidth (juce::jmin (filled.getWidth(), xFor (-6.0f, width))));

        if (filled.getWidth() > xFor (-6.0f, width))
        {
            g.setColour (Theme::meterHigh);
            g.fillRect (filled.withTrimmedLeft (xFor (-6.0f, width)));
        }

        if (hold[ch] > 0.0f)
        {
            g.setColour (Theme::textStrong);
            g.fillRect (bar.getX() + xFor (dbAt (hold[ch]), width) - 1.0f, y, 2.0f, barHeight);
        }

        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (Theme::smallText));
        g.drawText (ch == 0 ? "L" : "R", names.getX(), (int) y, names.getWidth(), (int) barHeight, juce::Justification::centredLeft);

        g.setColour (clipped[ch] ? Theme::danger : Theme::clipOff);
        g.fillRoundedRectangle ((float) lights.getX(), y, (float) lights.getWidth(), barHeight, 2.0f);

        g.setColour (clipped[ch] ? Theme::danger.brighter (0.3f) : Theme::text);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
        g.drawText (readout (ch), numbers.getX(), (int) y, numbers.getWidth(), (int) barHeight, juce::Justification::centredRight);
    }

    if (! showScale)
        return;

    g.setFont (juce::FontOptions (Theme::smallText - 1.0f));

    for (auto db : { -60, -48, -36, -24, -18, -12, -9, -6, -3, 0 })
    {
        const auto x = (float) bars.getX() + xFor ((float) db, width);
        g.setColour (Theme::lineStrong);
        g.fillRect (x, (float) scale.getY(), 1.0f, 3.0f);
        g.setColour (Theme::dimText);
        g.drawText (juce::String (db), juce::roundToInt (x) - 15, scale.getY() + 2, 30, scale.getHeight() - 2, juce::Justification::centred);
    }
}
