#pragma once

#include "AudioDocument.h"

/** Wow and flutter: the pitch drift of a tape or a record (the speed wandering slowly, wow, or
    quickly, flutter) found and taken out. The drift is read from the partials: in 4,096-point
    frames every 1,024, the spectral peaks (bins standing over their neighbours and over -60 dB)
    are followed from frame to frame (the nearest peak within 3 % continues a track), and each
    peak's deviation is its frequency against its own track's median over the 3 s round it, on
    tracks that have run 0.3 s or longer. A frame's speed is the power-weighted median of those
    deviations, taken only where the partials agree as a machine's speed makes them (four fifths
    of their weight within 0.3 % of the median) and three or more of the agreeing have held a
    second or longer: so what every partial does together counts, and what one voice does on its
    own (vibrato, a slide; a lone voice's harmonics also move together, but seldom hold a second)
    does not, and where there is no such agreement the speed is left at 1. No reading goes past 3 %.
    Nothing is summed from frame to frame, so a wrong reading stays in its own frame. The curve is
    low-passed at the fastest drift asked for (fourth-order, run forwards and back) and scaled to
    a mean of 1 (the length is kept). The audio is then played back at the curve's inverse (a
    varispeed, four-point interpolation), so a moment that ran fast is slowed back, pitch and time
    together, as the machine's wobble changed them. The amount scales how much of the drift is
    taken out. The speed curve is given back for showing and tests. */
namespace WowFlutter
{
    struct Settings
    {
        double fastestHz = 4.0;      // 0.5 to 20: drift faster than this is left (vibrato, trills)
        double amountPercent = 100.0;
    };

    bool isValid (const Settings&);

    struct Drift
    {
        int hop = 0;
        std::vector<double> speed;   // the speed read at each frame, relative to the mean (1 = none)
    };

    /** The speed curve over [start, end), from the partials. Empty for a range under half a second
        or when cancelled. */
    Drift measure (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&,
                   const std::function<bool (float)>& progress = {});

    /** Takes the drift out over [start, end). Nullptr for settings that aren't valid, a range under
        half a second, or when cancelled. */
    AudioDocument::Snapshot correct (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&,
                                     const std::function<bool (float)>& progress = {});
}
