#pragma once

#include "AudioDocument.h"

/** Azimuth correction: the time offset between the two channels of a tape transfer (a head out of
    line with the tracks) found as it drifts and taken out. Automatic Phase Correction finds one
    offset for a whole range; a transfer's offset wanders as the tape moves across the head, so
    here it is measured over windows of two seconds, a second apart, each with phase correction's
    measure (the cross-correlation's peak, between frames), with windows that give no clear
    reading (a correlation under 0.1, or silence) filled from their neighbours; the readings are
    smoothed over three (but the ends) and joined by straight lines, which run on past the first
    and the last reading. The channels are then moved by half the
    offset each, the late one earlier and the other later, by a windowed-sinc fractional delay
    (16 taps, Hann) whose delay follows the line sample by sample. Mono audio and a range under
    two seconds are left. */
namespace Azimuth
{
    struct Settings
    {
        double mostMs = 5.0;          // 0.5 to 50: the most offset looked for either way
        double amountPercent = 100.0; // 0 to 100
    };

    bool isValid (const Settings&);

    struct Drift
    {
        std::vector<double> offsetFrames;   // the right channel's lateness, a reading a second from the range's start plus one second
        int hopFrames = 0;                  // frames between readings
        juce::int64 firstCentre = 0;        // the first reading's centre
        bool anyClear = false;              // whether any window read clearly
    };

    /** The offset over [start, end) as it drifts. Empty readings for audio that isn't two channels
        or a range under two seconds; anyClear false where no window read clearly (the readings are
        then all 0). */
    Drift measure (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&,
                   const std::function<bool (float)>& progress = {});

    /** Takes the drifting offset out over [start, end). nullptr for audio that isn't two channels,
        a range under two seconds, no clear reading, settings that aren't valid, or when cancelled. */
    AudioDocument::Snapshot correct (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&,
                                     const std::function<bool (float)>& progress = {});
}
