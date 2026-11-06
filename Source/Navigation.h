#pragma once

#include <JuceHeader.h>
#include "Session.h"

/** Moving the cursor between edges: the file's or session's start and end, the selection's ends
    and, in a session, the clips' starts and ends. */
namespace Navigation
{
    /** The nearest stop after from (or before it, going backwards), or none. */
    std::optional<juce::int64> nextEdge (const std::vector<juce::int64>& stops, juce::int64 from, bool forwards);

    /** A file's edges: its start and end, and the selection's ends when there is a selection. */
    std::vector<juce::int64> fileEdges (juce::int64 length, juce::Range<juce::int64> selection);

    /** A session's edges: its start and end, every clip's start and end on the tracks shown in the
        lanes, and the time selection's ends when there is one. */
    std::vector<juce::int64> sessionEdges (const SessionState&, juce::Range<juce::int64> timeSelection);
}
