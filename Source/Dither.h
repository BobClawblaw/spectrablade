#pragma once

#include "AudioEdits.h"

/** Dither and word length: the audio rounded to 16 or 24 bits with dither, as the last step before
    a file at that depth, so the rounding's error is noise rather than distortion that follows the
    signal. The dither is triangular (two uniform random values added, a least-significant bit
    either way) and the rounding's error can be shaped: fed back through a short filter so the
    noise sits mostly where hearing is least sensitive, high up, and less in the middle. Shaping
    None leaves the noise flat; Light pushes it up 12 dB an octave (the error fed back from the last
    two samples, with weights 2 and -1); Strong 18 dB an octave (the last three: 3, -3 and 1). Shaped noise is louder in
    all but quieter below a few kilohertz. Every sample comes out an exact multiple of the step
    (1 / 32,768 at 16 bits, 1 / 8,388,608 at 24), within full scale. The noise is the same for the
    same audio (a fixed seed), so applying twice gives the same result. */
namespace Dither
{
    enum class Shaping { none, light, strong };

    const juce::StringArray& shapingNames();

    struct Settings
    {
        int bits = 16;              // 16 or 24
        Shaping shaping = Shaping::light;
    };

    bool isValid (const Settings&);

    /** The step between values at this depth: 2 to the (1 - bits). */
    double stepFor (int bits);

    /** Dithers [start, end) to the depth. Nullptr for settings that aren't valid or an empty range. */
    AudioEdits::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, const Settings&);
}
