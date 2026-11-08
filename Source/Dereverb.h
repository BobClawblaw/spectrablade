#pragma once

#include "AudioDocument.h"

/** Lowers a room's reverberation, by late-reverberation suppression in short-time Fourier frames
    (square-root Hann windows, 75% overlap, about 43 ms long), after Lebart, Boucher and Denbigh's
    2001 paper and Polack's statistical model of reverberation.

    Reverberation dies away by 60 dB over the room's reverb time. So in each frequency band, what
    is heard now that is only reverberation is predicted as the power about 80 ms ago, decayed by
    that much (each band's power taken over its neighbours and smoothed over time, as one bin's
    jumps about). The band is lowered by how much of it is that prediction: a pure tail, falling just
    as the reverb time says, goes down by the whole reduction, while a sound starting or holding
    its level is left alone (a gain normalised so a steady level passes, where the paper's plain
    subtraction would lower it too). The gains are smoothed over neighbouring bands and, falling,
    over time, so what is left doesn't warble.

    A reverb time set shorter than the room's leaves more of its tail. One set longer than the
    recording's own (measure) is taken as the recording's: with a longer time the model expects a
    slower fall, so any sound that stops faster than that, a dry voice's every syllable included,
    would read as a tail and be cut. The direct sound, and notes held steady, are kept either way. */
namespace Dereverb
{
    struct Settings
    {
        float reverbSeconds = 1.0f;   // the room's reverb time (to 60 dB down); capped at what the recording measures
        float reductionDb = 12.0f;    // how far reverberation is lowered, at most

        static constexpr float shortestReverb = 0.1f, longestReverb = 10.0f, mostReduction = 30.0f;
    };

    bool isValid (const Settings&);

    /** The frame length at a sample rate: about 43 ms, a power of two. */
    int frameSize (double sampleRate);

    /** The recording's reverb time over frames [start, end), in seconds: how long its sounds take
        to die away. In four bands from 250 Hz to 4 kHz, every frame whose band stood 20 dB over
        the band's floor (its quietest tenth) 80 ms before is compared with that earlier frame, and
        the typical fall over those 80 ms (the median of the falls) is taken as the room's decay,
        read as the time 60 dB would take. A dry recording, whose sounds stop dead, measures short
        (the test's dry bursts under 0.1 s, a studio voice-over 0.3 s); a room measures its own time
        (synthetic rooms of 0.5, 1 and 2 s measure 0.51, 1.01 and 1.74). Nothing when cancelled, or
        when the range is too short for a frame and the 80 ms. */
    std::optional<float> measure (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate,
                                  const std::function<bool (float)>& progress = {});

    /** Lowers the reverberation over frames [start, end), easing in and out over about a frame at
        the edges; audio more than a frame outside the range is untouched. The reverb time used is
        the setting or the measured time, whichever is shorter (never under shortestReverb), and
        is written to reverbSecondsUsed when given. progress gets 0..1 and returns false to cancel;
        then (and for an empty range or invalid settings) this returns nullptr. */
    AudioDocument::Snapshot reduce (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate,
                                    Settings, const std::function<bool (float)>& progress = {}, float* reverbSecondsUsed = nullptr);
}
