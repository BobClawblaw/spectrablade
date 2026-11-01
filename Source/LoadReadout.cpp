#include "LoadReadout.h"

LoadReadout::LoadReadout (std::function<Reading()> r) : read (std::move (r))
{
    setTooltip ("Audio load and dropouts; click to count dropouts from now");
    refresh();
    startTimerHz (4);
}

void LoadReadout::refresh()
{
    const auto reading = read != nullptr ? read() : Reading();

    // A new device starts its count again from 0.
    if (reading.dropouts < lastCount)
        baseline = 0;

    lastCount = reading.dropouts;
    shownDropouts = juce::jmax (0, reading.dropouts - baseline);

    const auto percent = juce::roundToInt (juce::jlimit (0.0, 1.0, reading.load) * 100.0);
    const auto next = "Load " + juce::String (percent) + "%  |  " + juce::String (shownDropouts) + (shownDropouts == 1 ? " dropout" : " dropouts");
    const auto nextWarning = reading.load > 0.8 || shownDropouts > 0;

    if (next != text || nextWarning != warning)
    {
        text = next;
        warning = nextWarning;
        repaint();
    }
}

void LoadReadout::resetDropouts()
{
    baseline = lastCount;
    refresh();
}

void LoadReadout::paint (juce::Graphics& g)
{
    g.setColour (warning ? juce::Colour (0xffff7b72) : juce::Colours::grey);
    g.setFont (14.0f);
    g.drawText (text, getLocalBounds().reduced (4, 0), juce::Justification::centredRight, true);
}

void LoadReadout::mouseDown (const juce::MouseEvent&)
{
    resetDropouts();
}
