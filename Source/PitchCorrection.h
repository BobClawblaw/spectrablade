#pragma once

#include "AudioDocument.h"

/** Pitch correction for one voice or instrument at a time (a singer, a solo line): each moment's
    pitch is found and moved to the nearest note of a scale, as fast or as gently as asked.

    The pitch is found with YIN (de Cheveigné and Kawahara, 2002) every 5 ms, from 60 to 1000 Hz:
    the difference between the sound and itself a period later, normalised, at its first dip below
    0.15, refined between frames. Moments with no clear period (breaths, consonants, noise, silence)
    are left as they are.

    The pitch is moved by time-domain pitch-synchronous overlap-add (PSOLA): grains two periods
    long, cut around marks one period apart (from the loudest point of a note's first period), are
    put back closer together or further apart, each at its exact place between frames. The periods
    are spaced differently rather than played faster, and the length doesn't change. Every channel
    is moved the same, by the pitch of their mix. A note keeps its target until the pitch strays a tenth of a semitone past halfway to the
    next, so vibrato near the middle doesn't flip between two notes. Notes are tuned to A = 440 Hz. */
namespace PitchCorrection
{
    enum class Scale { chromatic, major, minor };

    struct Settings
    {
        int key = 0;                       // the scale's first note, 0 (C) to 11 (B)
        Scale scale = Scale::chromatic;
        float speedMs = 20.0f;             // how long a correction takes to settle, 0 (at once) to slowest
        float strengthPercent = 100.0f;    // how much of the way to the note, 0 to 100

        static constexpr float slowestMs = 500.0f;
    };

    bool isValid (const Settings&);

    /** The pitch every `hop` frames from `first` on (0 where there is no clear one), in Hz. */
    struct Track
    {
        juce::int64 first = 0;
        int hop = 0;
        std::vector<float> hz;
    };

    constexpr double lowestHz = 60.0, highestHz = 1000.0;

    /** The pitch of the channels' mix over [start, end). Empty when cancelled (progress gets 0..1
        and returns false to stop) or for an empty range. */
    Track detect (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress = {});

    /** Whether a note (in MIDI numbers, 69 = A 440 Hz) is in the scale. */
    bool inScale (int note, const Settings&);

    /** Moves the pitch over [start, end) to the scale, easing from the audio as it was over 10 ms at
        the edges of a range that doesn't reach the file's. nullptr for an empty range, invalid
        settings, or when cancelled. */
    AudioDocument::Snapshot correct (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, Settings,
                                     const std::function<bool (float)>& progress = {});
}
