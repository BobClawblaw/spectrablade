#pragma once

#include "AudioDocument.h"

/** Evens out the level of speech: quiet passages are raised and loud ones lowered towards a
    target loudness, as an engineer riding a fader would.

    Loudness is measured as in BS.1770 (K-weighted, channels summed with its weights) in 400 ms
    blocks every 50 ms. A block counts as speech when it is above the silence threshold and within
    10 LU of the loudest block it overlaps (so one only just reaching into speech at a pause's edge
    doesn't). A step whose own block is speech gets the mean of the speech blocks within 750 ms of
    it as its level, and the gain that brings that to the target, kept within the largest boost
    and cut. The other steps are pauses: they are not levelled
    themselves but take the gain of the speech either side, so the room noise between words is
    never pumped up on its own. The gains may move no faster than the fastest change allows (in
    dB a second: a gain is held under what a rise at that rate from its neighbours' reaches, so
    it never goes above what the passages ask, only takes longer to get there), then they are
    smoothed over half a second either side, looking both ways (the whole range is known), and
    every channel gets the same gain, so the stereo image stays. At the fastest change's largest
    setting, 30 dB a second, the gains move as the smoothing alone lets them (on real speech
    that was 5 to 12.6 dB a second, at pauses and changes of speaker).

    No sample comes out above the ceiling (-1 dBFS): each step's gain is first held under what
    would take the peaks around it there, and smoothing never lifts a gain above its neighbours'
    limits. Where the audio already peaks above the ceiling, that can cut by more than the
    largest cut. The gain eases in and out over 10 ms at the edges of the range (where audio
    already above the ceiling stays as it was). */
namespace Leveller
{
    struct Settings
    {
        float targetLufs = -20.0f;    // where speech is brought to
        float maxBoostDb = 12.0f;     // the most a quiet passage is raised
        float maxCutDb = 12.0f;       // the most a loud passage is lowered
        float silenceLufs = -50.0f;   // windows quieter than this are pauses
        float speedDbPerSecond = 30.0f;   // the fastest the gain may change, 1 to 30 (30: as fast as the smoothing lets it)

        static constexpr float ceilingDb = -1.0f;   // no sample is raised above this (dBFS)
    };

    bool isValid (const Settings&);

    /** The gain applied over frames [start, end), in dB, one per 50 ms step from start (for
        showing and tests); nothing when cancelled. */
    std::optional<std::vector<float>> gains (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate,
                                             Settings, const std::function<bool (float)>& progress = {});

    /** Levels frames [start, end). progress gets 0..1 and returns false to cancel; then (and for
        an empty range or invalid settings) this returns nullptr. */
    AudioDocument::Snapshot level (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate,
                                   Settings, const std::function<bool (float)>& progress = {});
}
