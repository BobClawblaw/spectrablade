#pragma once

#include "AudioEdits.h"

#include <deque>

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

    /** The compressor run a frame at a time, its gain carried from frame to frame: compress runs
        through it, and so does the live effect, so they give the same samples. */
    class CompressorCore
    {
    public:
        /** New settings (valid) at this rate; the gain carries on from where it is. */
        void set (const Compressor&, double sampleRate);
        void reset() noexcept                       { gainDb = 0.0; }

        /** The factor for a frame whose loudest channel has this magnitude. */
        double next (double level) noexcept;

    private:
        Compressor settings;
        double attack = 0.0, release = 0.0, gainDb = 0.0;
    };

    struct Limiter
    {
        double inputGainDb = 0.0;   // applied first, to make the audio louder into the ceiling
        double ceilingDb = -1.0;    // no sample comes out above it
        double releaseMs = 50.0;    // the time for the gain to recover 63 % of the way after a peak
        double lookaheadMs = 5.0;   // the gain starts falling this long before a peak
        bool truePeak = false;      // hold the true peak (between samples, 4x oversampled) under the ceiling
    };

    /** Whether the settings can be used: input gain within 48 dB, a ceiling from -48 to 0 dBFS, release
        from 1 to 5,000 ms and look-ahead from 0.1 to 50 ms. */
    bool isValid (const Limiter&);

    /** The look-ahead in frames at this rate (at least 1). */
    int lookaheadFrames (const Limiter&, double sampleRate);

    /** The coefficient of a one-pole smoother that goes 63 % of the way in `ms`. */
    double smoothingCoefficient (double ms, double sampleRate);

    /** The ceiling as a gain, rounded to a float, so a clamped float sample never passes it. */
    double ceilingGain (double ceilingDb);

    /** The gain a frame needs to stay under the ceiling: its peak times the input gain, over the ceiling. */
    float limiterNeed (double peak, double inputGain, double ceiling);

    /** The limiter's gain, a frame at a time: push each frame's need (the gain that frame needs on its
        own to stay under the ceiling) and, once the look-ahead's worth of frames has come in, it gives
        the gain for the frame that many behind: the least need over that frame's look-ahead, averaged
        over the look-ahead before it, falling at once and recovering with the release. Frames before
        the first count as needing nothing. limit runs through it (then pushes a look-ahead of frames
        needing nothing to finish), and so does the live effect, a look-ahead behind. */
    class LimiterCore
    {
    public:
        void prepare (int lookahead, double releaseCoefficient);
        void setRelease (double releaseCoefficient) noexcept   { release = releaseCoefficient; }

        /** The gain for frame (pushed - lookahead), or nothing while fewer than lookahead + 1 frames
            have come in. */
        std::optional<double> push (float need);

        int getLookahead() const noexcept                       { return ahead; }

    private:
        int ahead = 1;
        double release = 0.0, sum = 0.0, gain = 1.0;
        juce::int64 pushed = 0;
        std::deque<std::pair<juce::int64, float>> window;   // a monotonic queue of (frame, need)
        std::vector<float> leasts;                          // the last ahead + 1 frames' least needs
        size_t oldest = 0;
    };

    /** Limits [start, end) so that no sample is above the ceiling. The gain each frame needs is the
        smallest needed anywhere in the look-ahead after it; that is averaged over the look-ahead, so
        the gain falls smoothly into a peak and is already down when it comes, and it recovers with
        the release time. Every output sample is at or below the ceiling. With no input gain, frames
        more than the look-ahead before the first frame that needs limiting come out bit for bit, as
        do frames after the gain has come all the way back. Nullptr for settings that aren't valid or
        an empty range.

        With truePeak, each frame's need also counts the peaks between it and its neighbours, found
        as truePeakDb finds them. The gain only approximates the waveform between samples, so the
        result is measured, and while its true peak is still above the ceiling (by more than 0.001 dB)
        it is limited again from the original, with the ceiling lowered by the overshoot, up to four
        passes. truePeakDb of the result tells what was reached. */
    AudioEdits::Snapshot limit (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Limiter&);

    /** The true peak of [start, end) in dBTP: the largest magnitude of any channel after 4x
        oversampling with the sample-rate converter, or of any sample, with silence taken outside the
        range (as Loudness measures it). -inf for silence or an empty range. */
    double truePeakDb (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate);

    struct Matched
    {
        AudioEdits::Snapshot snapshot;
        double gainDb = 0.0;        // the gain put in front of the limiter
        double integrated = 0.0;    // LUFS, measured on the result
        double truePeak = 0.0;      // dBTP, measured on the result
    };

    /** Brings [start, end), measured at `integrated` LUFS, to the target loudness with gain into a
        true-peak limiter at the ceiling (50 ms release, 5 ms look-ahead). Limiting takes loudness
        off, the more the harder it works, so the result is measured and the gain raised by the
        shortfall divided by how much the last raise gained (the first time, the whole shortfall),
        up to eight passes, until it is within 0.05 LU of the target. The gain stops at 48 dB. Nullptr snapshot
        for an empty range or a target, ceiling or measurement that isn't finite. */
    Matched matchLoudness (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate,
                           double integrated, double targetLufs, double ceilingDbtp);
}
