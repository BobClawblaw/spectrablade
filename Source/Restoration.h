#pragma once

#include <JuceHeader.h>
#include "AudioEdits.h"

/** Restoration: taking mains hum out. */
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
}
