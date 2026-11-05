#pragma once

#include <JuceHeader.h>

/** The Levels panel: what goes to the outputs as two long bars across the panel on a dB scale from
    -60 to 0, each with a falling peak hold, the highest peak since the last reset in numbers, and
    a clip light. Click to reset the peaks and lights. */
class LevelsPanel final : public juce::Component,
                          public juce::SettableTooltipClient,
                          private juce::Timer
{
public:
    /** The peaks of the left and right channels since the last call. */
    std::function<std::pair<float, float>()> readPeaks;

    LevelsPanel();

    /** Takes in a pair of peaks, as each tick of the timer does. */
    void take (float left, float right);

    /** The highest peak of a channel since the reset, as shown: "-12.3", "-inf" or "+0.4". */
    juce::String readout (int channel) const;
    bool isClipped (int channel) const              { return clipped[channel]; }
    void reset();

    /** Where a level in dB sits across a bar of this width (-60 dB and below at 0, 0 dB at the end). */
    static float xFor (float decibels, float width);

    static constexpr float floorDb = -60.0f;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override   { reset(); }

private:
    void timerCallback() override;

    float level[2] {}, hold[2] {}, highest[2] {};
    int holdTicks[2] {};
    bool clipped[2] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelsPanel)
};
