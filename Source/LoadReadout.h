#pragma once

#include <JuceHeader.h>

/** The audio callback's load and the dropouts counted since the readout was last clicked, in the
    status line: "Load 12%  |  0 dropouts". It turns red while the load is over 80% or there have
    been dropouts; clicking it starts the count again. */
class LoadReadout final : public juce::Component,
                          public juce::SettableTooltipClient,
                          private juce::Timer
{
public:
    struct Reading
    {
        double load = 0.0;   // 0 to 1: the share of each callback's time spent processing
        int dropouts = 0;    // under- and overruns since the device was opened
    };

    explicit LoadReadout (std::function<Reading()> read);

    /** Reads again and repaints when the text changes (done 4 times a second). */
    void refresh();

    juce::String getText() const                  { return text; }
    bool isWarning() const noexcept               { return warning; }
    int getDropouts() const noexcept              { return shownDropouts; }

    /** Counts dropouts from now (a click does this). */
    void resetDropouts();

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void timerCallback() override                 { refresh(); }

    std::function<Reading()> read;
    int baseline = 0, lastCount = 0, shownDropouts = 0;
    juce::String text;
    bool warning = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoadReadout)
};
