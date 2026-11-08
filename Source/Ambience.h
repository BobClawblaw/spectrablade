#pragma once

#include "AudioDocument.h"

/** Ambience match: the room tone of one part of a recording laid under another, so an edit, a
    replaced line or a silence between takes has the same background as the rest. The room tone is
    learned from a stretch of it alone (as a noise print is): the average power of each bin of
    4,096-point frames, with each bin's phase thrown away. Laying it under a range makes new room
    tone from that print: frames of random phase at the print's magnitudes, overlapped and added
    with square-root Hann windows (so it is steady and never repeats), scaled by the level, and
    added to the range, eased in and out over a frame at its edges. Where the range already holds
    room tone, Fill only the quiet adds tone only where the range's own level falls under the
    print's (its power smoothed over the bins round each, since one frame's single bin swings too
    much to compare), by the shortfall, so the background is evened out rather than doubled. */
namespace Ambience
{
    constexpr int frameSize = 4096;

    struct Print
    {
        double sampleRate = 0.0;
        std::vector<std::vector<float>> power;   // per channel, frameSize / 2 + 1 bins (a window's average)
    };

    /** The print of [start, end), or nothing for a range under a frame. */
    std::optional<Print> capture (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate);

    struct Settings
    {
        float levelDb = 0.0f;         // -24 to +12: the tone against the print's level
        bool fillOnlyTheQuiet = true; // add tone only where the range is quieter than the print, by the shortfall
    };

    bool isValid (const Settings&);

    /** Lays the print's tone under [start, end). Nullptr for a print at another rate, settings that
        aren't valid or an empty range. The tone is the same for the same print and range (a fixed
        seed), so applying twice gives the same result. */
    AudioDocument::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Print&, const Settings&);
}
