#pragma once

#include "AudioDocument.h"

/** Automatic phase correction: finding a time offset between the two channels of a stereo file
    (a microphone pair at different distances, a tape head out of line) and taking it out.

    The offset is where the channels' cross-correlation peaks, within the most allowed either way:
    the cross-spectrum is summed over blocks of the range, and the peak found first at whole frames
    and then between them, from the cross-spectrum itself (exact for band-limited audio). A peak
    that is negative means one channel is also upside down.

    Taking it out moves each channel half the offset, the one that is late earlier and the other
    later, so neither moves far, with a windowed-sinc fractional delay; a channel found upside down
    can be turned over too (the right one). */
namespace PhaseCorrection
{
    struct Finding
    {
        double offset = 0.0;       // frames the right channel is late on the left (negative: early)
        double correlation = 0.0;  // at the offset, -1 to 1; negative when one channel is upside down
    };

    /** The most offset looked for, in milliseconds. */
    constexpr double longestMs = 50.0, shortestMs = 0.1;

    /** A correlation weaker than this (either sign) is taken as no relation between the channels. */
    constexpr double weakest = 0.1;

    /** The offset between the channels over [start, end), looked for up to mostMs either way.
        Nothing for audio that isn't two channels, a range too short, settings out of range, a peak
        at the edge of what was looked for (so the offset may be further), silence, or a
        cancelled progress. */
    std::optional<Finding> measure (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, double mostMs,
                                    const std::function<bool (float)>& progress = {});

    /** Moves the channels over [start, end) to take out the offset (and turns the right channel
        over when `turnOver`), easing from the audio as it was over 10 ms at the edges of a range
        that doesn't reach the file's. nullptr for audio that isn't two channels or an empty range. */
    AudioDocument::Snapshot correct (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, double offset, bool turnOver);
}
