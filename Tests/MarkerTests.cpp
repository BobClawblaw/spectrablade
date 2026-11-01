#include <JuceHeader.h>
#include "../Source/AudioEdits.h"
#include "../Source/AudioEngine.h"
#include "../Source/AudioFileIO.h"
#include "TestUtils.h"
#include "MarkerTests.h"

namespace
{
    using TestUtils::check;

    juce::String describe (const Markers& markers)
    {
        juce::StringArray parts;

        for (auto& m : markers)
            parts.add (m.name + "@" + juce::String (m.start) + (m.isRegion() ? "+" + juce::String (m.length) : juce::String()));

        return parts.joinIntoString (" ");
    }

    void expect (const Markers& actual, const juce::String& expected, const juce::String& what)
    {
        const auto got = describe (actual);
        check (got == expected, what + (got == expected ? juce::String() : ": got [" + got + "], wanted [" + expected + "]"));
    }

    const Markers sample { { 100, 0, "a" }, { 200, 0, "b" }, { 300, 0, "c" },
                           { 150, 100, "r1" },   // [150, 250)
                           { 50, 400, "r2" },    // [50, 450)
                           { 220, 20, "r3" } };  // [220, 240)

    Markers sorted (Markers m)
    {
        MarkerEdits::sort (m);
        return m;
    }

    void testEditRules()
    {
        // Delete [180, 260): b and r3 lie inside; r1 loses its tail, r2 shrinks, c moves back.
        expect (MarkerEdits::afterReplace (sorted (sample), 180, 260, 0),
                "r2@50+320 a@100 r1@150+30 c@220", "markers: delete removes what is inside and closes the gap");

        // Insert 1000 frames at 200: b stays at the insert point, later markers move.
        expect (MarkerEdits::afterReplace (sorted (sample), 200, 200, 1000),
                "r2@50+1400 a@100 r1@150+1100 b@200 r3@1220+20 c@1300", "markers: insert pushes later markers and stretches spanning regions");

        // Replace [150, 250) with 10 frames: a region matching the range exactly fits the new material.
        expect (MarkerEdits::afterReplace (sorted (sample), 150, 250, 10),
                "r2@50+310 a@100 r1@150+10 c@210", "markers: a region over exactly the replaced range keeps the new material");

        // A region starting inside a deletion keeps its part after it.
        expect (MarkerEdits::afterReplace ({ { 100, 100, "r" } }, 50, 150, 0), "r@50+50",
                "markers: a region overlapping a deletion keeps its remaining part");

        expect (MarkerEdits::afterCrop (sorted (sample), 100, 300),
                "a@0 r2@0+200 r1@50+100 b@100 r3@120+20 c@200", "markers: crop keeps what overlaps and moves it to the start");

        check (MarkerEdits::nextName ({ { 0, 0, "Marker 01" }, { 5, 0, "Marker 03" } }, false) == "Marker 02"
                   && MarkerEdits::nextName ({}, true) == "Region 01",
               "markers: new names fill the first free number");

        // Spans between stops: marker starts and region ends, [150, 250) and [220, 240) among them.
        using Span = std::optional<juce::Range<juce::int64>>;
        const auto spans = sorted (sample);
        const auto between = MarkerEdits::spanAround (spans, 205, 1000) == Span ({ 200, 220 });
        const auto onStop = MarkerEdits::spanAround (spans, 200, 1000) == Span ({ 200, 220 });
        const auto regionEnd = MarkerEdits::spanAround (spans, 245, 1000) == Span ({ 240, 250 });
        const auto first = MarkerEdits::spanAround (spans, 10, 1000) == Span ({ 0, 50 });
        const auto last = MarkerEdits::spanAround (spans, 500, 1000) == Span ({ 450, 1000 });
        const auto none = ! MarkerEdits::spanAround ({}, 10, 1000) && ! MarkerEdits::spanAround (spans, 460, 450)
                          && ! MarkerEdits::spanAround ({ { 0, 0, "z" } }, 0, 0) && ! MarkerEdits::spanAround (spans, 1200, 1000);
        check (between && onStop && regionEnd && first && last && none,
               "markers: the span around a frame runs from the stop at or before it to the next, from 0 or to the end where there is none");

        const auto innermost = MarkerEdits::regionAt (spans, 230) == Span ({ 220, 240 }) && MarkerEdits::regionAt (spans, 200) == Span ({ 150, 250 })
                               && MarkerEdits::regionAt (spans, 150) == Span ({ 150, 250 }) && MarkerEdits::regionAt (spans, 400) == Span ({ 50, 450 });
        const auto outside = ! MarkerEdits::regionAt (spans, 20) && ! MarkerEdits::regionAt (spans, 450) && ! MarkerEdits::regionAt ({ { 5, 0, "p" } }, 5);
        check (innermost && outside, "markers: the region at a frame is the shortest holding it; none at a region's end, outside them or at a point");

        // Moving between regions ([50, 450), [150, 250), [220, 240) and a point between them).
        const auto stepping = MarkerEdits::adjacentRegion (spans, 0, true) == Span ({ 50, 450 }) && MarkerEdits::adjacentRegion (spans, 50, true) == Span ({ 150, 250 })
                              && MarkerEdits::adjacentRegion (spans, 150, true) == Span ({ 220, 240 }) && ! MarkerEdits::adjacentRegion (spans, 220, true)
                              && MarkerEdits::adjacentRegion (spans, 220, false) == Span ({ 150, 250 }) && ! MarkerEdits::adjacentRegion (spans, 50, false)
                              && MarkerEdits::adjacentRegion ({ { 10, 20, "long" }, { 10, 5, "short" } }, 0, true) == Span ({ 10, 15 });
        check (stepping, "markers: the next and previous region by start, points passed over, the shortest of those starting together");

        // Markers at intervals: every 100 frames through [50, 400), skipping the existing one at 150.
        auto spaced = Markers { { 150, 0, "Marker 01" }, { 0, 1000, "Region 01" } };
        const auto added = MarkerEdits::addAtIntervals (spaced, { 50, 400 }, 100);
        juce::StringArray placed;

        for (auto& m : spaced)
            placed.add (m.name + "@" + juce::String (m.start) + (m.isRegion() ? "+" + juce::String (m.length) : juce::String()));

        Markers scratch = spaced;
        const auto refused = MarkerEdits::addAtIntervals (scratch, { 0, 400 }, 0) == 0
                             && MarkerEdits::addAtIntervals (scratch, { 0, MarkerEdits::maxIntervalMarkers + 1 }, 1) == 0
                             && MarkerEdits::addAtIntervals (scratch, { 0, MarkerEdits::maxIntervalMarkers }, 1) == MarkerEdits::maxIntervalMarkers - 4;
        // Regions from markers: each point to the next point (regions in between don't count), the
        // last to the end; a point at the end stays a point; names and colours kept.
        auto toRegions = Markers { { 100, 0, "Intro" }, { 300, 0, "Verse", 2 }, { 150, 50, "Hit" }, { 700, 0, "Coda" }, { 1000, 0, "End" } };
        const auto turned = MarkerEdits::regionsFromPoints (toRegions, 1000);
        juce::StringArray made;

        for (auto& m : toRegions)
            made.add (m.name + "@" + juce::String (m.start) + "+" + juce::String (m.length) + (m.colour >= 0 ? "c" + juce::String (m.colour) : juce::String()));

        check (turned == 3 && made.joinIntoString (" ") == "Intro@100+200 Hit@150+50 Verse@300+400c2 Coda@700+300 End@1000+0",
               "markers: regions from markers run each point to the next and the last to the end, keeping names and colours ("
                   + made.joinIntoString (" ") + ")");

        // Pieces between markers: a "Start" piece before the first point, each point to the next,
        // regions and points at or past the end left out; none without points; no "Start" at frame 0.
        const auto describePieces = [] (const Markers& pieces)
        {
            juce::StringArray out;

            for (auto& m : pieces)
                out.add (m.name + "@" + juce::String (m.start) + "+" + juce::String (m.length));

            return out.joinIntoString (" ");
        };
        const auto pieces = describePieces (MarkerEdits::piecesBetweenPoints (
            { { 700, 0, "Coda" }, { 100, 0, "Intro" }, { 150, 50, "Hit" }, { 300, 0, "Verse" }, { 1000, 0, "End" } }, 1000));
        const auto fromZero = describePieces (MarkerEdits::piecesBetweenPoints ({ { 0, 0, "A" }, { 500, 0, "B" } }, 1000));
        check (pieces == "Start@0+100 Intro@100+200 Verse@300+400 Coda@700+300" && fromZero == "A@0+500 B@500+500"
                   && MarkerEdits::piecesBetweenPoints ({ { 150, 50, "Hit" }, { 1000, 0, "End" } }, 1000).empty(),
               "markers: pieces between markers cover the file from the start, one per point, regions ignored (" + pieces + ")");

        auto thinned = Markers { { 100, 0, "A" }, { 200, 500, "Long" }, { 300, 0, "B" }, { 400, 0, "C" }, { 50, 100, "Early" } };
        const auto gone = MarkerEdits::removeStartingIn (thinned, { 200, 400 });
        const auto left = describePieces (thinned);
        check (gone == 2 && left == "A@100+0 C@400+0 Early@50+100" && MarkerEdits::removeStartingIn (thinned, { 500, 500 }) == 0,
               "markers: deleting in a range takes those starting inside it, the end excluded, and keeps ones reaching into it (" + left + ")");

        auto shifted = Markers { { 100, 0, "A" }, { 200, 100, "R" }, { 300, 0, "B" }, { 500, 0, "C" } };
        const auto moved = MarkerEdits::moveStartingIn (shifted, { 150, 400 }, 250, 1000);
        const auto afterMove = describePieces (shifted);
        auto refusedMoves = shifted;
        check (moved == 2 && afterMove == "A@100+0 R@450+100 C@500+0 B@550+0"
                   && MarkerEdits::moveStartingIn (refusedMoves, { 400, 600 }, 600, 1000) == 0 && describePieces (refusedMoves) == afterMove
                   && MarkerEdits::moveStartingIn (refusedMoves, { 0, 200 }, -101, 1000) == 0 && MarkerEdits::moveStartingIn (refusedMoves, { 0, 200 }, 0, 1000) == 0
                   && MarkerEdits::moveStartingIn (refusedMoves, { 0, 200 }, -100, 1000) == 1 && refusedMoves.front().start == 0,
               "markers: moving in a range shifts only those starting inside it, kept sorted; past the start or end, or by nothing, refused (" + afterMove + ")");

        // Renaming in sequence: time order, regions included, colours kept; an empty name refused.
        auto renamed = toRegions;
        juce::StringArray names;

        if (MarkerEdits::renameInSequence (renamed, "  Take "))
            for (auto& m : renamed)
                names.add (m.name);

        auto many = Markers (100, Marker {});

        for (size_t i = 0; i < many.size(); ++i)
            many[i].start = (juce::int64) i;

        auto untouched = toRegions;
        check (names.joinIntoString (",") == "Take 01,Take 02,Take 03,Take 04,Take 05" && renamed[2].colour == 2
                   && MarkerEdits::renameInSequence (many, "T") && many.front().name == "T 001" && many.back().name == "T 100"
                   && ! MarkerEdits::renameInSequence (untouched, " ") && untouched == toRegions,
               "markers: renamed in sequence in time order with two digits, three from 100; an empty name changes nothing");

        // CSV in: what toCsv writes comes back the same at the same rate, odd names included; at
        // another rate by its times; past the end left out or cut; anything else refused.
        const Markers listed { { 0, 0, "Start" }, { 48000, 96000, "Verse, \"A\"" }, { 200000, 0, "Two\nlines" }, { 470000, 20000, "Tail" } };
        const auto back = MarkerEdits::fromCsv (MarkerEdits::toCsv (listed, 48000.0), 48000.0, 1000000);
        const auto halfRate = MarkerEdits::fromCsv (MarkerEdits::toCsv (listed, 48000.0), 24000.0, 1000000);
        const auto cut = MarkerEdits::fromCsv (MarkerEdits::toCsv (listed, 48000.0), 48000.0, 480000);
        check (back && *back == listed && halfRate && halfRate->size() == 4 && (*halfRate)[1].start == 24000 && (*halfRate)[1].length == 48000
                   && cut && cut->size() == 4 && cut->back().getEnd() == 480000
                   && MarkerEdits::fromCsv (MarkerEdits::toCsv (listed, 48000.0), 48000.0, 100000)->size() == 2,
               "markers: a CSV list reads back exactly, by its times at another rate, cut to the file's length");
        check (! MarkerEdits::fromCsv ("Track,Name\r\n", 48000.0, 1000) && ! MarkerEdits::fromCsv ({}, 48000.0, 1000)
                   && ! MarkerEdits::fromCsv ("Name,Start (s),End (s),Length (s)\r\nA,abc,,\r\n", 48000.0, 1000)
                   && ! MarkerEdits::fromCsv ("Name,Start (s),End (s),Length (s)\r\n\"open,1,,\r\n", 48000.0, 1000)
                   && MarkerEdits::fromCsv ("Name,Start (s),End (s),Length (s)\r\n", 48000.0, 1000)->empty(),
               "markers: another CSV, a bad time and an open quote are refused; a header alone is an empty list");

        auto mergedInto = Markers { { 0, 0, "Start" } };
        check (MarkerEdits::merge (mergedInto, listed) == 3 && mergedInto == listed && MarkerEdits::merge (mergedInto, listed) == 0,
               "markers: merging adds only markers not already there, kept sorted");

        check (added == 3 && placed.joinIntoString (" ") == "Region 01@0+1000 Marker 02@50 Marker 01@150 Marker 03@250 Marker 04@350" && refused,
               "markers: a marker every step through the range, sorted and named in turn, skipping taken frames; a zero step and too many refused ("
                   + placed.joinIntoString (" ") + ")");
    }

    void testUndo (AudioEngine& engine)
    {
        auto& doc = engine.document;
        juce::AudioBuffer<float> b (2, 1000);
        b.clear();
        doc.reset (AudioEdits::fromBuffer (std::move (b)), 48000.0, {}, 24, { { 500, 0, "m" } });

        const auto version = doc.getVersion();
        doc.setMarkers ({ { 500, 0, "m" }, { 100, 50, "r" } }, "Add r");
        check (doc.getVersion() == version && doc.isDirty() && doc.getMarkers().size() == 2,
               "markers: a marker-only step is undoable without touching the audio");

        const auto snap = doc.getSnapshot();
        doc.apply (AudioEdits::remove (*snap, 0, 200), "Delete",
                   MarkerEdits::afterReplace (doc.getMarkers(), 0, 200, 0));
        expect (doc.getMarkers(), "m@300", "markers: move with an edit");

        doc.undo();
        expect (doc.getMarkers(), "r@100+50 m@500", "markers: undo brings them back");
        doc.undo();
        expect (doc.getMarkers(), "m@500", "markers: undo the marker step");
        check (! doc.isDirty(), "markers: back at the saved state");
        doc.redo();
        doc.redo();
        expect (doc.getMarkers(), "m@300", "markers: redo");
    }

    void testFiles (AudioEngine& engine)
    {
        const auto rate = 48000.0;
        juce::AudioBuffer<float> b (2, 96000);

        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, (float) std::sin (i * 0.01) * 0.5f), b.setSample (1, i, 0.25f);

        const auto snap = AudioEdits::fromBuffer (std::move (b));
        const Markers markers { { 0, 0, "Start" }, { 12345, 0, juce::String (juce::CharPointer_UTF8 ("Chorus \xc3\xbc")) },
                                { 24000, 48000, "Verse" }, { 95999, 0, "Last" } };

        for (auto ext : { ".wav", ".aiff", ".flac" })
        {
            juce::TemporaryFile temp (ext);
            const auto error = AudioFileIO::save (*snap, rate, temp.getFile(), 24, engine.formatManager, markers);
            const auto loaded = AudioFileIO::load (temp.getFile(), engine.formatManager);
            const juce::String name (ext);

            if (name == ".wav")
                expect (loaded.markers, describe (markers), "markers: WAV keeps points, regions and names");
            else if (name == ".aiff")
                expect (loaded.markers, juce::String (juce::CharPointer_UTF8 ("Start@0 Chorus \xc3\xbc@12345 Verse@24000 Last@95999")),
                        "markers: AIFF keeps points and names (regions become their start)");
            else
                check (error.isEmpty() && loaded.markers.empty() && ! AudioFileIO::canStoreMarkers (temp.getFile()),
                       "markers: FLAC saves without them and says it cannot hold them");
        }
    }
}

void runMarkerTests (AudioEngine& engine)
{
    testEditRules();
    testUndo (engine);
    testFiles (engine);
}
