#pragma once

#include <JuceHeader.h>
#include "AudioEdits.h"

/** Restoration: taking mains hum out, and rebuilding clipped peaks. */
namespace Restoration
{
    /** Hum at a mains frequency and its harmonics, each taken out with a notch equally narrow in
        hertz: the mains frequency divided by q (50 / 30 = 1.7 Hz wide at q 30). */
    struct Hum
    {
        double frequency = 50.0;        // 50 or 60 Hz, or any from 20 to 1000
        int harmonics = 8;              // the fundamental and the multiples up to this one, 1 to 40
        double q = 30.0;                // 5 (wide) to 200 (narrow)
    };

    bool isValid (const Hum&, double sampleRate);

    /** The frequencies notched: the fundamental and its multiples up to `harmonics`, below 0.45
        of the sample rate. */
    std::vector<double> humFrequencies (const Hum&, double sampleRate);

    /** Takes the hum out of [start, end). The notches settle first on a second of audio: what
        comes before start, and where there isn't a second of it, the audio turned about its first
        frame, so the hum goes from the first frame. Null for a range that is empty or settings
        that aren't valid. */
    AudioEdits::Snapshot removeHum (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Hum&);

    /** Clipped peaks: runs of two or more samples in a row, of one sign, at or above a share of
        the channel's own peak in the range (so audio clipped at any level is found), and level
        with each other, within `flatness` of the peak: clipping leaves them equal, while even a
        100 Hz sine's top at 48 kHz changes by 9 times that from one sample to the next. */
    struct Declip
    {
        double threshold = 0.99;        // the share of the peak, 0.5 to 1
        bool keepPeak = true;           // scale the range so the rebuilt peaks reach the clipped level
    };

    struct DeclipResult
    {
        AudioEdits::Snapshot audio;     // null when nothing was rebuilt
        int repaired = 0;               // runs rebuilt
        int tooLong = 0;                // runs found but longer than maxRunSeconds, left as they were
    };

    constexpr double maxRunSeconds = 0.005;
    constexpr float flatness = 1.0e-5f;
    constexpr int fitPoints = 6;        // samples used on each side of a run

    /** Rebuilds each clipped run in [start, end) as the cubic that best fits (least squares) the
        fitPoints samples on each side of it, kept at or beyond the clipped level. Runs too near
        the range's edges to have them are left. With keepPeak, the range is then scaled so its
        peak is the level the clipped runs had. */
    DeclipResult declip (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Declip&);
}
