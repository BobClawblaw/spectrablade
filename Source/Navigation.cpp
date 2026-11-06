#include "Navigation.h"

namespace Navigation
{
    std::optional<juce::int64> nextEdge (const std::vector<juce::int64>& stops, juce::int64 from, bool forwards)
    {
        std::optional<juce::int64> best;

        for (auto s : stops)
        {
            if (forwards ? s > from && (! best || s < *best)
                         : s < from && (! best || s > *best))
                best = s;
        }

        return best;
    }

    std::vector<juce::int64> fileEdges (juce::int64 length, juce::Range<juce::int64> selection)
    {
        std::vector<juce::int64> stops { 0, length };

        if (! selection.isEmpty())
            stops.insert (stops.end(), { selection.getStart(), selection.getEnd() });

        return stops;
    }

    std::vector<juce::int64> sessionEdges (const SessionState& state, juce::Range<juce::int64> timeSelection)
    {
        std::vector<juce::int64> stops { 0, state.getLength() };

        for (auto& track : state.tracks)
            if (state.isShown (track))
                for (auto& clip : track.clips)
                    stops.insert (stops.end(), { clip.position, clip.getEnd() });

        if (! timeSelection.isEmpty())
            stops.insert (stops.end(), { timeSelection.getStart(), timeSelection.getEnd() });

        return stops;
    }
}
