#pragma once

#include <JuceHeader.h>

#include <numeric>

#include "Session.h"

/** A tempo from taps on a key: the average spacing of the last few taps, in beats a minute. A
    pause of 3 s or more (slower than 20 beats a minute) starts again. */
class TapTempo
{
public:
    static constexpr double restartAfter = 3.0;   // seconds
    static constexpr int maxIntervals = 8;

    /** A tap at this time (in seconds, from any fixed start). Gives the tempo from the second tap
        of a run on, rounded to hundredths and kept to 20 to 400. */
    std::optional<double> tap (double seconds)
    {
        if (last && seconds - *last > 0.0 && seconds - *last < restartAfter)
        {
            intervals.push_back (seconds - *last);

            if ((int) intervals.size() > maxIntervals)
                intervals.erase (intervals.begin());
        }
        else
        {
            intervals.clear();
        }

        last = seconds;

        if (intervals.empty())
            return {};

        const auto mean = std::accumulate (intervals.begin(), intervals.end(), 0.0) / (double) intervals.size();
        return juce::jlimit (20.0, 400.0, std::round (6000.0 / mean) / 100.0);
    }

    /** The spacings being averaged: 0 after the first tap of a run, then up to maxIntervals. */
    int getNumIntervals() const noexcept           { return (int) intervals.size(); }

private:
    std::optional<double> last;
    std::vector<double> intervals;
};

/** A tap on the session's tempo: from the second tap of a run, the tempo the taps give, as one
    undo step for the whole run ("Tap Tempo"). True when it changed the tempo. */
inline bool tapSessionTempo (Session& session, TapTempo& tapper, double seconds)
{
    const auto tempo = tapper.tap (seconds);

    if (! tempo || juce::exactlyEqual (*tempo, session.getState()->tempo))
        return false;

    auto next = *session.getState();

    if (! SessionEdits::setTempo (next, *tempo, next.beatsPerBar))
        return false;

    session.apply (std::move (next), "Tap Tempo", tapper.getNumIntervals() > 1 && session.getUndoDescription() == "Tap Tempo");
    return true;
}
