#pragma once

#include "AudioDocument.h"

/** Spectral recovery: the highs missing from band-limited audio (a phone line, a low-rate MP3, an
    old transfer) rebuilt from the band that is there. The audio is analysed in 4,096-point frames
    at 75 % overlap (square-root Hann windows). The cutoff is found from the long-term spectrum (the
    average power over the range, by bin): the highest frequency under which the spectrum holds
    within 30 dB of its level an octave lower (that level itself within 60 dB of the spectrum's
    mean), scanning down from the top; or it is given. In each
    frame, the bins of the octave below the cutoff are copied up an octave onto the bins above it
    (frequency doubled, so a partial at f gives one at 2f, where its harmonics would lie), with
    their phases advanced for the doubled frequency so the copy stays coherent from frame to frame,
    scaled so the rebuilt band continues the spectrum's slope just below the cutoff (measured over
    the octave below it, in dB an octave, in each frame) and then by the level; copied again an
    octave higher where that still lies under the top of the band (the second copy from the first).
    Bins already holding more than the copy keep what they have. Nothing below the cutoff changes.
    The level sets how loud the rebuilt highs are against that continuation. */
namespace SpectralRecovery
{
    struct Settings
    {
        double cutoffHz = 0.0;        // 0: found from the audio; else 1,000 to 16,000
        double levelDb = -3.0;        // -24 to +6: the rebuilt highs against the continued slope
        double highestHz = 16000.0;   // 2,000 to 20,000: rebuilt up to here
    };

    bool isValid (const Settings&, double sampleRate);

    /** The cutoff found in [start, end), in Hz; 0 where the spectrum reaches the top with no edge
        (nothing to rebuild) or the range is under a window. */
    double findCutoff (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate);

    /** Rebuilds the highs over [start, end). Nullptr for settings that aren't valid, a range under
        a window, no cutoff found, or when cancelled. */
    AudioDocument::Snapshot recover (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&,
                                     const std::function<bool (float)>& progress = {});
}
