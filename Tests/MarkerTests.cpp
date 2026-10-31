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
