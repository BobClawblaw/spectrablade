#pragma once

#include "AudioEdits.h"

/** Dynamics: a compressor and a limiter, run offline over a range of a file. Both work out one
    gain for every channel at each frame, from the loudest channel, so a stereo image stays put. */
namespace Dynamics
{
    struct Compressor
    {
        double thresholdDb = -20.0;
        double ratio = 4.0;         // 1 changes nothing; above the threshold, ratio dB in come out as 1
        double kneeDb = 6.0;        // the soft knee's width, centred on the threshold; 0 is a hard knee
        double attackMs = 10.0;     // the time for the gain to move 63 % of the way down to its target
        double releaseMs = 100.0;   // and back up
        double makeupDb = 0.0;      // added after
    };

    /** Whether the settings can be used: a threshold from -80 to 0 dB, a ratio from 1 to 100, a knee
        from 0 to 24 dB, attack from 0.01 to 1,000 ms, release from 1 to 5,000 ms, makeup within 48 dB. */
    bool isValid (const Compressor&);

    /** The gain the compressor settles at for a steady level, in dB (0 or less; the makeup not included). */
    double staticGainDb (const Compressor&, double levelDb);

    /** Compresses [start, end): at each frame the level is the loudest channel's sample magnitude, its
        static gain is the target, and the gain in dB moves toward it with the attack time when it
        falls and the release time when it rises. Nullptr for settings that aren't valid or an empty range. */
    AudioEdits::Snapshot compress (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Compressor&);

    struct Limiter
    {
        double inputGainDb = 0.0;   // applied first, to make the audio louder into the ceiling
        double ceilingDb = -1.0;    // no sample comes out above it
        double releaseMs = 50.0;    // the time for the gain to recover 63 % of the way after a peak
        double lookaheadMs = 5.0;   // the gain starts falling this long before a peak
    };

    /** Whether the settings can be used: input gain within 48 dB, a ceiling from -48 to 0 dBFS, release
        from 1 to 5,000 ms and look-ahead from 0.1 to 50 ms. */
    bool isValid (const Limiter&);

    /** Limits [start, end) so that no sample is above the ceiling. The gain each frame needs is the
        smallest needed anywhere in the look-ahead after it; that is averaged over the look-ahead, so
        the gain falls smoothly into a peak and is already down when it comes, and it recovers with
        the release time. Every output sample is at or below the ceiling (sample peaks; between samples
        it can go a little above). With no input gain, frames more than the look-ahead before the first
        frame that needs limiting come out bit for bit, as do frames after the gain has come all the
        way back. Nullptr for settings that aren't valid or an empty range. */
    AudioEdits::Snapshot limit (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Limiter&);
}
