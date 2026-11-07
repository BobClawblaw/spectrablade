#pragma once

#include "AudioDocument.h"

/** Loudness as defined by ITU-R BS.1770-4 and EBU R128 (EBU Tech 3341 and 3342).

    Channels are K-weighted (a high shelf and a high-pass, worked out for the file's rate) and
    summed by power with BS.1770 weights: 1.0 for the front channels, 1.41 for the surrounds of a
    6-channel (5.1) file and the side surrounds of an 8-channel (7.1) one, 1.0 for 7.1's rear
    surrounds (at 135 degrees), and the LFE (channel 4 of a 6- or 8-channel file) left out. True peak is the largest absolute value
    after 4x oversampling with the sample-rate converter. */
namespace Loudness
{
    struct Result
    {
        double integrated = -HUGE_VAL;     // LUFS, gated (-70 LUFS absolute, -10 LU relative)
        double range = 0.0;                // LU (EBU Tech 3342)
        double maxMomentary = -HUGE_VAL;   // LUFS, 400 ms windows
        double maxShortTerm = -HUGE_VAL;   // LUFS, 3 s windows
        double truePeak = -HUGE_VAL;       // dBTP
        double samplePeak = -HUGE_VAL;     // dBFS
    };

    /** Measures frames [start, end) of the snapshot. progress gets 0..1 and returns false to cancel. */
    std::optional<Result> measure (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate,
                                   const std::function<bool (float)>& progress = {});

    /** For a buffer (tests). */
    Result measure (const juce::AudioBuffer<float>&, double sampleRate);

    /** The K-weighted power of frames [start, end) in steps of stepFrames (the last one may be
        shorter), summed over channels with BS.1770's weights: -0.691 + 10 log10 of a mean of
        these is loudness in LUFS. Outside the snapshot counts as silence. progress as above;
        nothing when cancelled. */
    std::optional<std::vector<double>> stepPowers (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate,
                                                   int stepFrames, const std::function<bool (float)>& progress = {});

    /** "-23.0 LUFS", or "-inf" when there is nothing above the gate. */
    juce::String format (double value, const juce::String& unit);
}
