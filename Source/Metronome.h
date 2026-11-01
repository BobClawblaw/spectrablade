#pragma once

#include <JuceHeader.h>

/** The metronome's beat grid and click. Beats fall on the session's tempo from frame 0, each
    rounded to the nearest frame, so the grid never drifts. */
namespace Metronome
{
    /** How long a click sounds. */
    constexpr double clickSeconds = 0.03;

    /** The frame beat k starts on. */
    juce::int64 beatFrame (juce::int64 beat, double tempo, double rate);

    /** The beat sounding at a frame: the last one starting at or before it. */
    juce::int64 beatAt (juce::int64 frame, double tempo, double rate);

    /** The frames in a bar (beatsPerBar beats, rounded). */
    juce::int64 barFrames (double tempo, int beatsPerBar, double rate);

    /** The click, `offset` frames after its beat starts (0 once it has ended): a short sine that
        dies away, higher and louder on the first beat of a bar. */
    float sample (juce::int64 offset, bool accent, double rate);
}
