#include <JuceHeader.h>
#include "../Source/Navigation.h"
#include "TestUtils.h"
#include "NavigationTests.h"

namespace
{
    using TestUtils::check;

    SessionClip clipAt (SessionState& s, juce::int64 position, juce::int64 length)
    {
        SessionClip c;
        c.id = s.nextId++;
        c.position = position;
        c.length = length;
        return c;
    }

    /** Steps from a frame to each edge in turn until there are no more. */
    std::vector<juce::int64> walk (const std::vector<juce::int64>& stops, juce::int64 from, bool forwards)
    {
        std::vector<juce::int64> visited;

        while (const auto next = Navigation::nextEdge (stops, from, forwards))
            visited.push_back (from = *next);

        return visited;
    }

    void testFile()
    {
        const auto none = Navigation::fileEdges (48000, {});
        check (walk (none, 1000, true) == std::vector<juce::int64> { 48000 } && walk (none, 1000, false) == std::vector<juce::int64> { 0 },
               "edges: in a file with no selection, the next edge is its end and the previous its start");

        const auto selected = Navigation::fileEdges (48000, { 10000, 20000 });
        check (walk (selected, 0, true) == std::vector<juce::int64> { 10000, 20000, 48000 }
                   && walk (selected, 48000, false) == std::vector<juce::int64> { 20000, 10000, 0 }
                   && walk (selected, 15000, true) == std::vector<juce::int64> { 20000, 48000 },
               "edges: with a selection, the cursor stops at its start and end on the way, either way, and from inside it");

        check (! Navigation::nextEdge (selected, 48000, true) && ! Navigation::nextEdge (selected, 0, false)
                   && Navigation::nextEdge (selected, 10000, true) == 20000,
               "edges: none past the end or before the start, and from an edge the next is the one after it");
    }

    void testSession()
    {
        SessionState s;
        auto& a = s.addTrack();
        a.clips.push_back (clipAt (s, 1000, 4000));     // 1000 to 5000
        a.clips.push_back (clipAt (s, 8000, 2000));     // 8000 to 10000
        auto& b = s.addTrack();
        b.clips.push_back (clipAt (s, 3000, 4000));     // 3000 to 7000, overlapping the first on another track
        auto& hidden = s.addTrack();
        hidden.hidden = true;
        hidden.clips.push_back (clipAt (s, 6000, 500)); // 6000 to 6500, out of the lanes

        const auto stops = Navigation::sessionEdges (s, {});
        check (walk (stops, 0, true) == std::vector<juce::int64> { 1000, 3000, 5000, 7000, 8000, 10000 }
                   && walk (stops, 10000, false) == std::vector<juce::int64> { 8000, 7000, 5000, 3000, 1000, 0 },
               "edges: in a session the cursor stops at every clip's start and end on the tracks shown, in time order either way, "
               "and not at a hidden track's clips");

        const auto withSelection = Navigation::sessionEdges (s, { 2000, 7500 });
        check (walk (withSelection, 0, true) == std::vector<juce::int64> { 1000, 2000, 3000, 5000, 7000, 7500, 8000, 10000 },
               "edges: and at the time selection's ends");

        const SessionState empty;
        check (Navigation::sessionEdges (empty, {}) == std::vector<juce::int64> { 0, 0 } && ! Navigation::nextEdge (Navigation::sessionEdges (empty, {}), 0, true),
               "edges: an empty session has nowhere to go");
    }
}

void runNavigationTests()
{
    testFile();
    testSession();
}
