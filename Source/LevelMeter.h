#pragma once

#include <JuceHeader.h>
#include "Theme.h"

/** A level meter (the input's and the output's): one bar per channel on a -60..0 dB scale with a
    falling peak hold and a clip light. Levels come from a callback polled on the message thread. */
class LevelMeter final : public juce::Component,
                         public juce::SettableTooltipClient,
                         private juce::Timer
{
public:
    /** Returns the peak of a channel (0 or 1) since the last call. */
    std::function<float (int channel)> readPeak;

    LevelMeter()                                  { startTimerHz (30); }

    void setNumChannels (int n)                   { numChannels = juce::jlimit (0, 2, n); repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Theme::inset);
        g.fillRoundedRectangle (r, 3.0f);

        if (numChannels == 0)
            return;

        r.reduce (2.0f, 2.0f);
        const auto clipWidth = 6.0f;
        const auto rowHeight = (r.getHeight() - (float) (numChannels - 1)) / (float) numChannels;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto row = r.withHeight (rowHeight).withY (r.getY() + (float) ch * (rowHeight + 1.0f));
            const auto clipArea = row.removeFromRight (clipWidth);
            row.removeFromRight (2.0f);

            g.setColour (clipped[ch] ? Theme::danger : Theme::clipOff);
            g.fillRect (clipArea);

            const auto w = row.getWidth() * proportion (level[ch]);
            g.setGradientFill (juce::ColourGradient::horizontal (Theme::meterLow, row.getX(),
                                                                 Theme::meterMid, row.getRight()));
            g.fillRect (row.withWidth (w));

            if (hold[ch] > 0.0f)
            {
                g.setColour (juce::Colours::white);
                g.fillRect (row.getX() + row.getWidth() * proportion (hold[ch]) - 1.0f, row.getY(), 1.0f, row.getHeight());
            }
        }
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        clipped[0] = clipped[1] = false;   // click to clear the clip lights
        repaint();
    }

private:
    static float proportion (float gain)
    {
        const auto db = juce::Decibels::gainToDecibels (gain, -60.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    }

    void timerCallback() override
    {
        if (readPeak == nullptr)
            return;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto peak = readPeak (ch);
            level[ch] = juce::jmax (peak, level[ch] * 0.80f);

            if (peak >= hold[ch])
            {
                hold[ch] = peak;
                holdTicks[ch] = 45;
            }
            else if (--holdTicks[ch] <= 0)
            {
                hold[ch] *= 0.90f;
            }

            if (peak >= 1.0f)
                clipped[ch] = true;
        }

        repaint();
    }

    int numChannels = 0;
    float level[2] {}, hold[2] {};
    int holdTicks[2] {};
    bool clipped[2] {};
};
