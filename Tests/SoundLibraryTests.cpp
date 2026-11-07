#include <JuceHeader.h>
#include "../Source/SoundLibrary.h"
#include "../Source/SoundLibraryView.h"
#include "TestUtils.h"
#include "SoundLibraryTests.h"

namespace
{
    using TestUtils::check;
    using SoundLibrary::Index;

    /** A WAV of `frames` frames of quiet noise, with a BWF description when one is given. */
    void writeWav (const juce::File& file, int frames, double rate = 48000.0, int channels = 2, int bits = 24, const juce::String& description = {})
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        std::unordered_map<juce::String, juce::String> metadata;

        if (description.isNotEmpty())
            metadata[juce::WavAudioFormat::bwavDescription] = description;

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
        auto options = juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (channels).withBitsPerSample (bits)
                           .withMetadataValues (metadata);
        auto writer = wav.createWriterFor (out, options);
        juce::AudioBuffer<float> b (channels, frames);
        juce::Random r (frames);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, 0.1f * (r.nextFloat() - 0.5f));

        writer->writeFromAudioSampleBuffer (b, 0, frames);
    }

    juce::StringArray names (const std::vector<SoundLibrary::Sound>& sounds)
    {
        juce::StringArray out;

        for (auto& s : sounds)
            out.add (s.name);

        return out;
    }

    void testWords()
    {
        check (SoundLibrary::wordsOf ("DOORWood_Creak Old Barn 02_JD").joinIntoString (" ") == "DOOR Wood Creak Old Barn 02 JD"
                   && SoundLibrary::wordsOf ("footsteps-gravel.v2").joinIntoString (" ") == "footsteps gravel v 2"
                   && SoundLibrary::wordsOf ("  ").isEmpty() && SoundLibrary::wordsOf (juce::String (juce::CharPointer_UTF8 ("Caf\xc3\xa9 Cr\xc3\xa8me"))).size() == 2
                   && SoundLibrary::wordsOf (juce::String (juce::CharPointer_UTF8 ("Caf\xc3\xa9")))[0] == juce::String (juce::CharPointer_UTF8 ("Caf\xc3\xa9")),
               "sound library: names split into words at anything not a letter or digit, at case changes and between letters and digits");
    }

    void testScan (const juce::File& dir)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        const auto library = dir.getChildFile ("Library");
        writeWav (library.getChildFile ("Doors/DOORWood_Creak Old Barn_JD_SFX.wav"), 24000, 48000.0, 2, 24, "Old barn door creaking slowly");
        writeWav (library.getChildFile ("Doors/DOORMetal_Slam Heavy_JD_SFX.wav"), 12000, 96000.0, 1, 16);
        writeWav (library.getChildFile ("Footsteps/FOLYFeet_Gravel Walk_JD_SFX.wav"), 48000, 44100.0, 2, 24, "Walking on loose gravel");

        for (int i = 0; i < 27; ++i)
            writeWav (library.getChildFile ("Ambience/Room " + juce::String (i + 1) + "/AMBRoom_Tone " + juce::String (i + 1) + ".wav"), 1000 + 100 * i);

        library.getChildFile ("notes.txt").replaceWithText ("not audio");
        library.getChildFile ("Doors/broken.wav").replaceWithText ("RIFF but not really a wave file");

        const auto database = dir.getChildFile ("index/Sound Library.sqlite");
        Index index (database);
        const auto refusedMissing = ! index.addFolder (dir.getChildFile ("Nowhere"));
        const auto added = index.addFolder (library);
        const auto refusedInside = ! index.addFolder (library.getChildFile ("Doors")) && index.getError().contains ("inside");
        const auto refusedTwice = ! index.addFolder (library);
        int calls = 0;
        const auto first = index.scan (formats, [&calls] (int) { ++calls; return true; });
        const auto door = index.find ({ "creak" });

        check (index.isOpen() && refusedMissing && added && refusedInside && refusedTwice && first.added == 30 && first.unreadable == 1
                   && first.removed == 0 && ! first.stopped && index.count() == 30 && door.size() == 1
                   && door[0].name == "DOORWood_Creak Old Barn_JD_SFX" && door[0].frames == 24000 && juce::exactlyEqual (door[0].sampleRate, 48000.0)
                   && door[0].channels == 2 && door[0].bitDepth == 24 && door[0].description == "Old barn door creaking slowly"
                   && door[0].format == "WAV file" && door[0].folder == library && std::abs (door[0].seconds() - 0.5) < 1.0e-9,
               "sound library: a scan indexes the 30 audio files in a watched folder and its folders (header, format, length, rate, "
               "channels, bits and BWF description), skipping a text file and counting a broken WAV as unreadable; a missing "
               "folder, the same folder again or one inside it is refused");

        // Searching: the start of any word in the name or description, every word needed.
        const auto byWord = names (index.find ({ "wood" }));
        const auto both = index.count ("door creak"), neither = index.count ("door gravel"), described = index.count ("loose");
        const auto partial = index.count ("grav"), doors = index.count ("door"), none = index.count ("zebra");
        check (byWord.size() == 1 && both == 1 && neither == 0 && described == 1 && partial == 1 && doors == 2 && none == 0
                   && index.count ("\"; DROP TABLE sounds; --") == 0 && index.count() == 30,
               "sound library: \"wood\" finds DOORWood_Creak, \"door creak\" one sound, \"door gravel\" none, \"loose\" the gravel walk by "
               "its description, \"grav\" by the start of a word, \"door\" both doors; quotes and SQL in a search are only words");

        // Sorting and paging: pages of 7 by length, longest first, cover every sound once in order.
        std::vector<SoundLibrary::Sound> paged;

        for (int offset = 0;; offset += 7)
        {
            const auto page = index.find ({ {}, SoundLibrary::Column::length, false, offset, 7 });
            paged.insert (paged.end(), page.begin(), page.end());

            if (page.size() < 7)
                break;
        }

        auto ordered = paged.size() == 30;

        for (size_t i = 1; i < paged.size(); ++i)
            ordered = ordered && paged[i - 1].seconds() >= paged[i].seconds() && paged[i - 1].id != paged[i].id;

        const auto byName = names (index.find ({ {}, SoundLibrary::Column::name, true, 0, 3 }));
        check (ordered && paged.front().name == "FOLYFeet_Gravel Walk_JD_SFX" && byName[0] == "AMBRoom_Tone 1" && byName[1] == "AMBRoom_Tone 10",
               "sound library: pages of 7 sorted by length, longest first, list all 30 once each in order; by name, A to Z");

        // A rescan reads only what changed: one rewritten, one deleted, one new.
        const auto second = index.scan (formats);
        writeWav (library.getChildFile ("Doors/DOORMetal_Slam Heavy_JD_SFX.wav"), 36000, 96000.0, 1, 16);
        library.getChildFile ("Doors/DOORMetal_Slam Heavy_JD_SFX.wav").setLastModificationTime (juce::Time::getCurrentTime() + juce::RelativeTime::seconds (5));
        library.getChildFile ("Ambience/Room 3/AMBRoom_Tone 3.wav").deleteFile();
        writeWav (library.getChildFile ("Doors/DOORGlass_Shatter_JD_SFX.wav"), 5000);
        const auto third = index.scan (formats);
        const auto slam = index.find ({ "slam" });
        check (second.unchanged == 30 && second.added == 0 && second.updated == 0 && second.removed == 0
                   && third.updated == 1 && third.removed == 1 && third.added == 1 && third.unchanged == 28 && index.count() == 30
                   && slam.size() == 1 && slam[0].frames == 36000 && index.count ("tone 3") == 0 && index.count ("shatter") == 1,
               "sound library: a rescan with nothing changed reads no file again (30 unchanged); with one rewritten, one deleted and "
               "one new it updates, removes and adds just those");

        // Stopped, it keeps what it did and removes nothing; the index is there when opened again,
        // and another connection reads it meanwhile.
        writeWav (library.getChildFile ("Ambience/Room 3/AMBRoom_Tone 3.wav"), 1300);
        int asked = 0;
        const auto stopped = index.scan (formats, [&asked] (int) { return ++asked < 1; });
        Index reader (database);
        const auto seenElsewhere = reader.count();
        const auto removed = index.removeFolder (library);
        Index reopened (database);
        check (stopped.stopped && stopped.removed == 0 && seenElsewhere >= 30 && removed && reopened.isOpen() && reopened.count() == 0
                   && reopened.getFolders().isEmpty() && reopened.count ("creak") == 0,
               "sound library: a stopped scan removes nothing; a second connection reads the index while another holds it; removing "
               "the folder takes its sounds out, and that is kept when the index is opened again");
    }

    void testLarge (const juce::File& dir)
    {
        // A million sounds with UCS-like names, then how long searches take.
        static const char* categories[] { "DOOR", "FOLY", "AMB", "WATR", "WIND", "VEH", "GUN", "IMPT", "WHSH", "CRWD" };
        static const char* materials[] { "Wood", "Metal", "Glass", "Stone", "Plastic", "Gravel", "Leather", "Cloth", "Paper", "Ice" };
        static const char* actions[] { "Creak", "Slam", "Hit", "Scrape", "Roll", "Drop", "Rattle", "Squeak", "Tap", "Rub" };
        constexpr int total = 1000000, batch = 50000;
        Index index (dir.getChildFile ("large/Sound Library.sqlite"));
        juce::Random r (7);
        const auto started = juce::Time::getMillisecondCounterHiRes();

        for (int done = 0; done < total; done += batch)
        {
            std::vector<SoundLibrary::Sound> sounds ((size_t) batch);

            for (int i = 0; i < batch; ++i)
            {
                auto& s = sounds[(size_t) i];
                const auto n = done + i;
                s.name = juce::String (categories[r.nextInt (10)]) + materials[r.nextInt (10)] + "_" + actions[r.nextInt (10)] + " "
                         + juce::String (n) + "_LIB";
                s.folder = juce::File ("/Library/SFX");
                s.file = s.folder.getChildFile (s.name + ".wav");
                s.frames = 1000 + r.nextInt (1000000);
                s.sampleRate = 48000.0;
                s.channels = 2;
                s.bitDepth = 24;
                s.format = "WAV file";
            }

            index.putAll (sounds);
        }

        const auto built = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
        const auto timed = [] (auto&& f) { const auto t = juce::Time::getMillisecondCounterHiRes(); f(); return juce::Time::getMillisecondCounterHiRes() - t; };
        juce::int64 all = 0, creaks = 0, woodCreaks = 0, one = 0;
        std::vector<SoundLibrary::Sound> page, deep, sorted;
        const auto countAll = timed ([&] { all = index.count(); });
        const auto countCreak = timed ([&] { creaks = index.count ("creak"); });
        const auto countBoth = timed ([&] { woodCreaks = index.count ("wood creak"); });
        const auto countOne = timed ([&] { one = index.count ("123456"); });
        const auto firstPage = timed ([&] { page = index.find ({ "wood creak", SoundLibrary::Column::name, true, 0, 100 }); });
        const auto deepPage = timed ([&] { deep = index.find ({ {}, SoundLibrary::Column::name, true, 500000, 100 }); });
        const auto byLength = timed ([&] { sorted = index.find ({ "creak", SoundLibrary::Column::length, false, 0, 100 }); });
        const auto ms = [] (double v) { return juce::String (v, 1) + " ms"; };

        check (all == total && creaks > 90000 && creaks < 110000 && woodCreaks > 8000 && woodCreaks < 12000 && one == 1
                   && page.size() == 100 && deep.size() == 100 && sorted.size() == 100,
               "sound library, " + juce::String (total) + " sounds (built in " + juce::String (built, 1) + " s): counting all " + ms (countAll)
                   + ", \"creak\" (" + juce::String (creaks) + ") " + ms (countCreak) + ", \"wood creak\" (" + juce::String (woodCreaks) + ") "
                   + ms (countBoth) + ", one sound by its number " + ms (countOne) + "; the first page of \"wood creak\" by name "
                   + ms (firstPage) + ", page 5001 of everything by name " + ms (deepPage) + ", the longest 100 of \"creak\" " + ms (byLength));
    }
}

namespace
{
    juce::MouseEvent mouseAt (juce::Component& on, juce::Point<float> at, juce::Point<float> down)
    {
        return { juce::Desktop::getInstance().getMainMouseSource(), at, {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &on, &on,
                 juce::Time::getCurrentTime(), down, juce::Time::getCurrentTime(), 1, at != down };
    }

    void testView (const juce::File& dir)
    {
        const auto library = dir.getChildFile ("View Library");
        writeWav (library.getChildFile ("Doors/DOORWood_Creak Old Barn_JD_SFX.wav"), 24000, 48000.0, 2, 24, "Old barn door creaking slowly");
        writeWav (library.getChildFile ("Doors/DOORMetal_Slam Heavy_JD_SFX.wav"), 12000, 96000.0, 1, 16);
        writeWav (library.getChildFile ("Doors/DOORMetal_Latch_JD_SFX.wav"), 12000, 192000.0, 1, 24);

        for (int i = 0; i < 7; ++i)
            writeWav (library.getChildFile ("Rooms/AMBRoom_Tone " + juce::String (i + 1) + ".wav"), 2000 + 9000 * i, 44100.0);

        SoundLibraryView view (dir.getChildFile ("view/Index.sqlite"));
        view.setSize (1000, 500);
        const auto empty = view.getNumRows() == 0 && view.getSummary().startsWith ("No folders yet") && view.getFolders().isEmpty();
        const auto refused = ! view.addFolder (dir.getChildFile ("Nowhere")) && view.getSummary().contains ("isn't a folder");
        const auto added = view.addFolder (library);
        const auto scanned = TestUtils::runUntil ([&view] { return ! view.isScanning() && view.getSummary().contains ("Last scan"); }, 20000);
        check (empty && refused && added && scanned && view.getNumRows() == 10
                   && view.getSummary() == "10 sounds in 1 folder. Last scan: 10 added, 0 updated, 0 removed",
               "sound library window: with no folders it scans nothing and says to add one; a missing folder is refused, saying why; an "
               "added one is scanned on its own thread and its 10 sounds listed (\"" + view.getSummary() + "\")");

        view.setSearch ("creak");
        const auto oneRow = view.getNumRows() == 1 && view.getText (0, SoundLibraryView::name) == "DOORWood_Creak Old Barn_JD_SFX"
                            && view.getText (0, SoundLibraryView::length) == "0.50 s" && view.getText (0, SoundLibraryView::rate) == "48 kHz"
                            && view.getText (0, SoundLibraryView::channels) == "2" && view.getText (0, SoundLibraryView::bits) == "24"
                            && view.getText (0, SoundLibraryView::format) == "WAV" && view.getText (0, SoundLibraryView::location) == "View Library/Doors"
                            && view.getText (0, SoundLibraryView::description) == "Old barn door creaking slowly"
                            && view.getSummary().startsWith ("1 of 10 sounds match");
        view.setSearch ({});
        view.setSort (SoundLibraryView::length, false);
        const auto longest = view.getText (0, SoundLibraryView::name) == "AMBRoom_Tone 7" && view.getText (0, SoundLibraryView::length) == "1.27 s";
        view.setSort (SoundLibraryView::rate, true);
        const auto slowest = view.getText (0, SoundLibraryView::rate) == "44.1 kHz" && view.getText (8, SoundLibraryView::rate) == "96 kHz"
                             && view.getText (9, SoundLibraryView::rate) == "192 kHz";
        check (oneRow && longest && slowest,
               "sound library window: \"creak\" shows the one sound with its length, rate, channels, bits, format, folder and "
               "description; sorted by length the longest comes first, by rate the slowest (44.1, 96 and 192 kHz written so)");

        // Dragging from a selected row carries the selected files out; from another row, nothing.
        juce::StringArray dragged;
        int drags = 0;
        view.onDragOut = [&] (const juce::StringArray& files) { dragged = files; ++drags; };
        juce::SparseSet<int> rows;
        rows.addRange ({ 0, 3 });
        view.selectRows (rows);
        juce::Component* list = nullptr;
        auto& asComponent = static_cast<juce::Component&> (view);

        for (auto* c : view.getChildren())
            if (dynamic_cast<juce::TableListBox*> (c) != nullptr)
                list = c;

        auto& t = *dynamic_cast<juce::TableListBox*> (list);
        const auto rowY = [&t] (int row) { return (float) (t.getHeader().getHeight() + row * t.getRowHeight() + t.getRowHeight() / 2); };
        const juce::Point<float> onRow1 { 100.0f, rowY (1) }, onRow5 { 100.0f, rowY (5) };
        asComponent.mouseDown (mouseAt (t, onRow1, onRow1));
        asComponent.mouseDrag (mouseAt (t, onRow1 + juce::Point<float> (3.0f, 0.0f), onRow1));
        const auto notYet = drags == 0;
        asComponent.mouseDrag (mouseAt (t, onRow1 + juce::Point<float> (40.0f, 30.0f), onRow1));
        asComponent.mouseDrag (mouseAt (t, onRow1 + juce::Point<float> (80.0f, 60.0f), onRow1));
        const auto once = drags == 1 && dragged.size() == 3 && dragged == view.getSelectedFiles();
        auto exist = true;

        for (auto& f : dragged)
            exist = exist && juce::File (f).existsAsFile() && juce::File (f).isAChildOf (library);

        asComponent.mouseDown (mouseAt (t, onRow5, onRow5));
        asComponent.mouseDrag (mouseAt (t, onRow5 + juce::Point<float> (40.0f, 30.0f), onRow5));
        check (notYet && once && exist && drags == 1,
               "sound library window: a drag from a selected row carries the 3 selected files out once it has moved 6 pixels (once per "
               "drag); one starting on a row not selected carries nothing");

        // A rescan finds what changed; removing the folder empties it.
        writeWav (library.getChildFile ("Doors/DOORGlass_Shatter_JD_SFX.wav"), 5000);
        view.rescan();
        const auto rescanned = TestUtils::runUntil ([&view] { return ! view.isScanning() && view.getSummary().contains ("1 added"); }, 20000);
        const auto eleven = view.getNumRows() == 11;
        const auto removed = view.removeFolder (library);
        check (rescanned && eleven && removed && view.getNumRows() == 0 && view.getSummary().startsWith ("No folders yet"),
               "sound library window: Rescan adds a new file (11 sounds); removing the folder takes them all out");
    }
}

void runSoundLibraryTests()
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("SoundLibraryTests", {}, false);
    dir.createDirectory();
    testWords();
    testScan (dir);
    testView (dir);
    testLarge (dir);
    dir.deleteRecursively();
}
