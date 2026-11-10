#include <JuceHeader.h>
#include "../Source/AudioFileIO.h"
#include "../Source/SoundLibrary.h"
#include "../Source/SoundLibraryView.h"
#include "../Source/Surround.h"
#include "../Source/SoundCategories.h"
#include "../Source/SoundMetadata.h"
#include <sqlite3.h>
#include <filesystem>
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
        AudioFileIO::registerFormats (formats);

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
        const auto log = database.getSiblingFile (database.getFileName() + "-wal");
        check (log.getSize() == 0 && database.getSize() > 16384,
               "sound library: after a scan what it wrote is in the index file (" + juce::String (database.getSize()) + " bytes) and SQLite's log is "
               "empty (" + juce::String (log.getSize()) + " bytes), no read left open to hold it");

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

        // Rated, favourite and tagged sounds among the million: every 10th rated, every 100th a favourite, every 200th tagged.
        std::vector<juce::int64> ratedIds, favouriteIds, taggedIds;

        for (juce::int64 id = 1; id <= total; ++id)
        {
            if (id % 10 == 0)   ratedIds.push_back (id);
            if (id % 100 == 0)  favouriteIds.push_back (id);
            if (id % 200 == 0)  taggedIds.push_back (id);
        }

        const auto marking = timed ([&] { index.setRating (ratedIds, 4); index.setFavourite (favouriteIds, true); index.addTag (taggedIds, "Keep"); });
        SoundLibrary::Query fourStars, favourites, kept, keptCreaks;
        fourStars.minRating = 4;
        favourites.favouritesOnly = true;
        kept.tag = "keep";
        keptCreaks.tag = "keep";
        keptCreaks.text = "creak";
        juce::int64 nRated = 0, nFavourites = 0, nKept = 0, nKeptCreaks = 0;
        std::vector<SoundLibrary::Sound> keptPage;
        const auto countRated = timed ([&] { nRated = index.count (fourStars); });
        const auto countFavourites = timed ([&] { nFavourites = index.count (favourites); });
        const auto countKept = timed ([&] { nKept = index.count (kept); });
        const auto countKeptCreaks = timed ([&] { nKeptCreaks = index.count (keptCreaks); });
        const auto keptFirst = timed ([&] { keptPage = index.find (kept); });
        check (nRated == total / 10 && nFavourites == total / 100 && nKept == total / 200 && nKeptCreaks > 0 && nKeptCreaks < nKept
                   && keptPage.size() == 100,
               "sound library, " + juce::String (total) + " sounds: rating 100,000, marking 10,000 and tagging 5,000 took " + ms (marking)
                   + "; counting those rated 4 stars or more " + ms (countRated) + ", the favourites " + ms (countFavourites) + ", the tagged "
                   + ms (countKept) + " (with \"creak\" " + ms (countKeptCreaks) + "), the first page of the tagged " + ms (keptFirst));
    }

    /** An AIFF of `frames` frames of silence. */
    void writeAiff (const juce::File& file, int frames)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        juce::AiffAudioFormat aiff;
        std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
        auto writer = aiff.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (2).withBitsPerSample (24));
        juce::AudioBuffer<float> b (2, frames);
        b.clear();
        writer->writeFromAudioSampleBuffer (b, 0, frames);
    }

    void testFormats (const juce::File& dir)
    {
        const auto choices = SoundLibrary::formatChoices();
        const auto labelOf = [&choices] (const juce::String& name)
        {
            for (const auto& c : choices)
                if (c.name == name)
                    return c.label;

            return juce::String();
        };
        auto twice = false;

        for (size_t i = 0; i < choices.size(); ++i)
            for (size_t j = i + 1; j < choices.size(); ++j)
                twice = twice || choices[i].name == choices[j].name;

       #if JUCE_MAC
        check (labelOf ("WAV file") == "WAV" && labelOf ("AIFF file") == "AIFF" && labelOf ("FLAC file") == "FLAC"
                   && labelOf ("CoreAudio supported file") == "Other" && labelOf ("MP3 file").isEmpty() && ! twice,
               "sound library formats: the choices are WAV, AIFF, FLAC and the rest with short labels, CoreAudio's as Other, each once, "
               "and MP3 is not offered on its own (CoreAudio reads every MP3 first)");
       #else
        check (labelOf ("WAV file") == "WAV" && labelOf ("AIFF file") == "AIFF" && labelOf ("FLAC file") == "FLAC"
                   && labelOf ("MP3 file") == "MP3"
                  #if JUCE_WINDOWS
                   && labelOf ("M4A file") == "M4A"
                  #endif
                   && labelOf ("CoreAudio supported file").isEmpty() && ! twice,
               "sound library formats: the choices are WAV, AIFF, FLAC, MP3 and the rest with short labels, each once");
       #endif

        // A library with a sound as WAV and, in a folder of its own, as AIFF: WAV alone lists it once,
        // and so does leaving the folder out.
        const auto library = dir.getChildFile ("Twins");
        const auto aiffs = library.getChildFile ("Aiff");
        writeWav (library.getChildFile ("Owl Hoot.wav"), 1000);
        writeWav (library.getChildFile ("Rain on Roof.wav"), 1200);
        writeAiff (aiffs.getChildFile ("Owl Hoot.aif"), 1000);
        juce::StringArray every;

        for (const auto& c : choices)
            every.add (c.name);

        {
            juce::AudioFormatManager formats;
            AudioFileIO::registerFormats (formats);
            Index index (dir.getChildFile ("formats/Index.sqlite"));
            index.addFolder (library);
            index.scan (formats);
            SoundLibrary::Query q;
            q.formats = { "WAV file" };
            const auto found = index.find (q);
            auto wavs = found.size() == 2;

            for (const auto& s : found)
                wavs = wavs && s.format == "WAV file";

            SoundLibrary::Query owls;
            owls.text = "owl";
            owls.formats = { "WAV file" };
            SoundLibrary::Query without;
            without.foldersLeftOut = { aiffs };
            SoundLibrary::Query owlsWithout;
            owlsWithout.text = "owl";
            owlsWithout.foldersLeftOut = { aiffs };
            check (index.find ({}).size() == 3 && wavs && index.count (q) == 2 && index.find (owls).size() == 1 && index.count (owls) == 1
                       && index.find (without).size() == 2 && index.count (without) == 2 && index.find (owlsWithout).size() == 1,
                   "sound library formats: a query for WAV alone finds and counts only the WAVs, with a search too; so does one leaving the AIFF folder out");
        }

        {
            SoundLibraryView view (dir.getChildFile ("formats view/Index.sqlite"));
            view.setSize (1300, 700);
            view.addFolder (library);
            TestUtils::runUntil ([&view] { return ! view.isScanning() && view.getSummary().contains ("Last scan"); }, 20000);
            int told = 0;
            juce::StringArray lastTold;
            view.onShownFormatsChanged = [&] (const juce::StringArray& f) { ++told; lastTold = f; };

            const auto wavFirst = view.getNumRows() == 2 && view.getShownFormats() == juce::StringArray { "WAV file" }
                                  && view.getSummary().startsWith ("2 of 3 sounds match");
            view.toggleFormat ("AIFF file");
            const auto added = view.getNumRows() == 3 && told == 1 && lastTold == juce::StringArray { "WAV file", "AIFF file" }
                               && view.getSummary().startsWith ("3 sounds in 1 folder");
            view.toggleFormat ("WAV file");
            const auto aiffOnly = view.getNumRows() == 1 && view.getText (0, SoundLibraryView::format) == "AIFF" && told == 2;
            view.toggleFormat ("AIFF file");
            const auto lastStays = view.getNumRows() == 1 && told == 2;
            view.toggleFormat ({});
            const auto all = view.getNumRows() == 3 && view.getShownFormats() == every && told == 3;
            view.showFormats ({ "AIFF file", "Nothing file" });
            const auto set = view.getNumRows() == 1 && view.getShownFormats() == juce::StringArray { "AIFF file" } && told == 3;
            view.showFormats ({});
            check (wavFirst && added && aiffOnly && lastStays && all && set && view.getShownFormats() == juce::StringArray { "WAV file" },
                   "sound library window: WAV alone to start with (2 of 3); the menu's toggles add AIFF (reported), leave only the AIFFs, keep "
                   "the last format, and show every format; the app sets a list (a name not offered dropped, no report back), none meaning WAV");

            // The AIFF folder left out, with every format shown.
            int foldersTold = 0;
            view.onFoldersLeftOutChanged = [&] (const juce::Array<juce::File>&) { ++foldersTold; };
            view.showFormats (every);
            view.leaveOutFolder (aiffs);
            const auto leftOut = view.getNumRows() == 2 && view.getFoldersLeftOut() == juce::Array<juce::File> { aiffs } && foldersTold == 1
                                 && view.getSummary().startsWith ("2 of 3 sounds match");
            view.leaveOutFolder (aiffs);
            view.showFolderAgain (aiffs);
            const auto back = view.getNumRows() == 3 && view.getFoldersLeftOut().isEmpty() && foldersTold == 2;
            view.setFoldersLeftOut ({ aiffs, aiffs, juce::File() });
            check (leftOut && back && view.getNumRows() == 2 && view.getFoldersLeftOut().size() == 1 && foldersTold == 2,
                   "sound library window: a folder left out drops its sounds from the list (reported once, a second time ignored), comes back "
                   "with Show Again, and the app's list is taken without repeats or an empty folder and without a report");
        }
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

        // The folder tree keeps what is open through a rebuild (a scan's end rebuilds it, items and all).
        {
            juce::TreeView* folders = nullptr;

            for (auto* c : view.getChildren())
                if (auto* t = dynamic_cast<juce::TreeView*> (c); t != nullptr && t->isVisible())
                    folders = t;

            auto* item = folders != nullptr && folders->getRootItem() != nullptr ? folders->getRootItem()->getSubItem (1) : nullptr;   // the library, after All Folders
            const auto itemName = item != nullptr ? item->getUniqueName() : juce::String();
            const auto hadFolder = item != nullptr && itemName == library.getFullPathName() && ! item->isOpen();

            if (item != nullptr)
                item->setOpen (true);

            const auto subFolders = item != nullptr ? item->getNumSubItems() : 0;
            view.rescan();   // the rebuild at its end deletes the items: item is not used past here
            const auto rescanned = TestUtils::runUntil ([&view] { return ! view.isScanning() && view.getSummary().contains ("0 added, 0 updated"); }, 20000);
            auto* after = folders != nullptr && folders->getRootItem() != nullptr ? folders->getRootItem()->getSubItem (1) : nullptr;
            const juce::String why = juce::String (view.getPane() == SoundLibraryView::Pane::folders ? "folders pane, " : "categories pane, ")
                                     + (folders != nullptr ? "tree found, " : "no tree, ") + (itemName.isNotEmpty() ? "item " + itemName.quoted() + ", " : "no item, ")
                                     + (hadFolder ? "closed at first, " : "not the library closed, ") + (rescanned ? "rescanned, " : "not rescanned, ")
                                     + (after == nullptr ? "nothing after" : after->isOpen() ? "open after" : "closed after");
            check (view.getPane() == SoundLibraryView::Pane::folders && hadFolder && subFolders > 0 && rescanned && after != nullptr && after->isOpen(),
                   "sound library window: the folder tree lists the library closed, opens into its " + juce::String (subFolders)
                       + " folders, and is still open after the rescan rebuilds it (" + why + ")");
        }

        // The bar beside the pane on the left sizes it: dragged to x = 300 the pane is 297 wide and
        // the change is reported when the bar is let go; 0 puts the default back.
        {
            DividerBar* bar = nullptr;

            for (auto* c : view.getChildren())
                if (auto* d = dynamic_cast<DividerBar*> (c))
                    bar = d;

            const auto defaultX = bar != nullptr ? bar->getX() : -1;
            auto reported = -1;
            view.onPaneWidthChanged = [&reported] (int w) { reported = w; };

            if (bar != nullptr)
            {
                const juce::Point<float> at ((float) (300 - bar->getX()), 10.0f);
                bar->mouseDrag (mouseAt (*bar, at, { 3.0f, 10.0f }));
                bar->mouseUp (mouseAt (*bar, at, { 3.0f, 10.0f }));
            }

            const auto dragged = bar != nullptr && bar->getX() == 297 && view.getPaneWidth() == 297 && reported == 297;
            view.setPaneWidth (0);
            check (dragged && bar != nullptr && bar->getX() == defaultX && defaultX == 200,
                   "sound library window: the bar beside the folders is dragged to size the pane (to x = 300 makes it 297 wide, reported when let go), "
                   "and 0 puts the default (a fifth of a 1000 px window) back");
        }

        view.setSearch ("creak");
        const auto oneRow = view.getNumRows() == 1 && view.getText (0, SoundLibraryView::name) == "DOORWood_Creak Old Barn_JD_SFX"
                            && view.getText (0, SoundLibraryView::length) == "0.50 s" && view.getText (0, SoundLibraryView::rate) == "48 kHz"
                            && view.getText (0, SoundLibraryView::channels) == "2" && view.getText (0, SoundLibraryView::bits) == "24"
                            && view.getText (0, SoundLibraryView::format) == "WAV" && view.getText (0, SoundLibraryView::location).replaceCharacter ('\\', '/') == "View Library/Doors"
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

        // Selecting a row shows its sound in the preview; the list changing under it doesn't.
        juce::SparseSet<int> one;
        one.addRange ({ 4, 5 });
        view.selectRows (one);
        const auto chosen = juce::File (view.getSelectedFiles()[0]);
        const auto shown = view.getPreview().getPlayer().getFile() == chosen && view.getPreview().getPlayer().getLength() > 0;
        view.setSearch ("tone");
        const auto kept = view.getPreview().getPlayer().getFile() == chosen;
        view.setSearch ({});
        check (shown && kept, "sound library window: selecting a row shows its sound in the preview; searching keeps it there");

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

namespace
{
    using Player = SoundLibraryPreview::Player;

    juce::AudioBuffer<float> noise (int channels, int frames, int seed)
    {
        juce::AudioBuffer<float> b (channels, frames);
        juce::Random r (seed);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, 0.5f * (r.nextFloat() - 0.5f));

        return b;
    }

    /** Writes the buffer as a WAV: 32 bits are float, so it reads back exactly. */
    void writeBuffer (const juce::File& file, const juce::AudioBuffer<float>& b, double rate, int bits = 32)
    {
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
        auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (b.getNumChannels())
                                                    .withBitsPerSample (bits)
                                                    .withSampleFormat (bits == 32 ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                                                  : juce::AudioFormatWriterOptions::SampleFormat::integral));
        writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    juce::AudioBuffer<float> readBack (const juce::File& file, double* rate = nullptr, int* bits = nullptr)
    {
        juce::AudioFormatManager fm;
        AudioFileIO::registerFormats (fm);
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));

        if (r == nullptr)
            return {};

        juce::AudioBuffer<float> b ((int) r->numChannels, (int) r->lengthInSamples);
        r->read (&b, 0, b.getNumSamples(), 0, true, true);

        if (rate != nullptr)
            *rate = r->sampleRate;

        if (bits != nullptr)
            *bits = (int) r->bitsPerSample;

        return b;
    }

    /** Pulls frames from the player in device-sized blocks, as a device callback does. */
    juce::AudioBuffer<float> pull (Player& p, int frames, int block = 512)
    {
        juce::AudioBuffer<float> out (2, frames), b (2, block);
        out.clear();

        for (int done = 0; done < frames; done += block)
        {
            const auto n = juce::jmin (block, frames - done);
            p.getNextAudioBlock (juce::AudioSourceChannelInfo (&b, 0, n));

            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, done, b, ch, 0, n);
        }

        return out;
    }

    bool same (const juce::AudioBuffer<float>& a, int aChannel, int aStart, const juce::AudioBuffer<float>& b, int bChannel, int bStart, int frames,
               float scale = 1.0f, float tolerance = 0.0f)
    {
        for (int i = 0; i < frames; ++i)
            if (std::abs (a.getSample (aChannel, aStart + i) - scale * b.getSample (bChannel, bStart + i)) > tolerance)
                return false;

        return true;
    }

    bool silent (const juce::AudioBuffer<float>& a, int start, int frames)
    {
        for (int ch = 0; ch < a.getNumChannels(); ++ch)
            if (a.getMagnitude (ch, start, frames) != 0.0f)
                return false;

        return true;
    }

    // Folders from the index: the folders inside one that hold sounds (not a sibling whose name
    // starts the same), how many sounds are in each, and a query or the view limited to one.
    void testFolders (const juce::File& dir)
    {
        const auto library = dir.getChildFile ("Folder Library");
        writeWav (library.getChildFile ("Loose One.wav"), 1000);
        writeWav (library.getChildFile ("Loose Two.wav"), 1000);
        writeWav (library.getChildFile ("Doors/Wood/Creak.wav"), 1000);
        writeWav (library.getChildFile ("Doors/Wood/Deep/Slam.wav"), 1000);
        writeWav (library.getChildFile ("Doors/Metal Latch.wav"), 1000);
        writeWav (library.getChildFile ("Doors-Old/Hinge.wav"), 1000);
        writeWav (library.getChildFile ("Doors0/Knock.wav"), 1000);
        writeWav (library.getChildFile ("Rain/Light.wav"), 1000);
        library.getChildFile ("Empty/Nothing Here").createDirectory();
        library.getChildFile ("Doors/Notes.txt").replaceWithText ("not a sound");

        SoundLibrary::Index index (dir.getChildFile ("folders/Index.sqlite"));
        juce::AudioFormatManager formats;
        AudioFileIO::registerFormats (formats);
        index.addFolder (library);
        index.scan (formats);

        const auto names = [] (const juce::Array<juce::File>& folders)
        {
            juce::StringArray n;

            for (const auto& f : folders)
                n.add (f.getFileName());

            return n.joinIntoString ("|");
        };
        const auto top = names (index.subfoldersOf (library)), doors = names (index.subfoldersOf (library.getChildFile ("Doors")));
        const auto deepest = index.subfoldersOf (library.getChildFile ("Doors/Wood/Deep")).isEmpty();
        check (top == "Doors|Doors-Old|Doors0|Rain" && doors == "Wood" && deepest,
               "sound library folders: the folders holding sounds, from the index, in order, without empty ones (" + top + "; in Doors: " + doors + ")");

        const auto counted = index.countWithin (library) == 8 && index.countWithin (library.getChildFile ("Doors")) == 3
                             && index.countWithin (library.getChildFile ("Doors/Wood")) == 2 && index.countWithin (library.getChildFile ("Empty")) == 0;
        SoundLibrary::Query q;
        q.within = library.getChildFile ("Doors");
        const auto inDoors = index.find (q);
        auto onlyDoors = inDoors.size() == 3;

        for (const auto& sound : inDoors)
            onlyDoors = onlyDoors && sound.file.isAChildOf (library.getChildFile ("Doors"));

        q.text = "slam";
        const auto searched = index.count (q) == 1;
        check (counted && onlyDoors && searched,
               "sound library folders: 8 sounds in the library, 3 in Doors and deeper (not Doors-Old's or Doors0's), 2 in Wood; "
               "a query within Doors finds only its 3, and searched within it, 1");

        SoundLibraryView view (dir.getChildFile ("folders-view/Index.sqlite"));
        view.setSize (1000, 500);
        view.addFolder (library);
        TestUtils::runUntil ([&view] { return ! view.isScanning() && view.getSummary().contains ("Last scan"); }, 20000);
        const auto foldersFirst = view.getPane() == SoundLibraryView::Pane::folders && view.getNumRows() == 8;
        view.showFolder (library.getChildFile ("Doors/Wood"));
        const auto inWood = view.getNumRows() == 2 && view.getSummary().startsWith ("2 of 8 sounds match");
        view.showPane (SoundLibraryView::Pane::categories);
        const auto allAgain = view.getPane() == SoundLibraryView::Pane::categories && view.getNumRows() == 8;
        view.showPane (SoundLibraryView::Pane::folders);
        view.showFolder (library.getChildFile ("Rain"));
        const auto rain = view.getNumRows() == 1 && view.getText (0, SoundLibraryView::name) == "Light";
        view.removeFolder (library);
        const auto gone = view.getNumRows() == 0;
        check (foldersFirst && inWood && allAgain && rain && gone,
               "sound library window: By Folder comes first; choosing Doors/Wood lists its 2 sounds; By Category shows all 8 again; "
               "Rain lists its one; removing the library empties the list");
    }

    void testPlayer (const juce::File& dir)
    {
        const auto stereo = noise (2, 10000, 1), mono = noise (1, 7000, 2), surround = noise (6, 4000, 3);
        writeBuffer (dir.getChildFile ("preview/stereo.wav"), stereo, 48000.0);
        writeBuffer (dir.getChildFile ("preview/mono.wav"), mono, 48000.0);
        writeBuffer (dir.getChildFile ("preview/surround.wav"), surround, 48000.0);
        writeBuffer (dir.getChildFile ("preview/second.wav"), noise (2, 44100, 4), 44100.0);

        Player p;
        p.setReadTimeout (-1);   // wait for the disk, so what plays is exactly the file
        p.prepareToPlay (512, 48000.0);

        const auto loaded = p.load (dir.getChildFile ("preview/stereo.wav")) && p.getLength() == 10000 && ! p.isPlaying();
        p.play();
        const auto whole = pull (p, 12000);
        check (loaded && same (whole, 0, 0, stereo, 0, 0, 10000) && same (whole, 1, 0, stereo, 1, 0, 10000) && silent (whole, 10000, 2000)
                   && ! p.isPlaying() && p.getPosition() == 10000 && silent (pull (p, 1024), 0, 1024),
               "sound library preview: a stereo sound at the device's rate plays sample for sample, then stops at its end, silent after");

        p.setRange ({ 1000, 3000 });
        p.setLooping (true);
        p.play();
        const auto looped = pull (p, 5000);
        auto loops = p.isPlaying();

        for (int i = 0; i < 5000 && loops; ++i)
            loops = juce::exactlyEqual (looped.getSample (0, i), stereo.getSample (0, 1000 + i % 2000))
                    && juce::exactlyEqual (looped.getSample (1, i), stereo.getSample (1, 1000 + i % 2000));

        p.setLooping (false);
        p.setRange ({});
        p.playFrom (0);
        p.setGainDb (-6.0f);
        const auto quieter = pull (p, 4096);
        p.setGainDb (0.0f);
        check (loops && same (quieter, 0, 0, stereo, 0, 0, 4096, juce::Decibels::decibelsToGain (-6.0f), 1.0e-7f),
               "sound library preview: a part (frames 1000 to 3000) loops sample for sample, 2.5 times through in 5000 frames; -6 dB of "
               "preview volume plays it at 0.501 of its level");

        const auto monoLoaded = p.load (dir.getChildFile ("preview/mono.wav"));
        p.play();
        const auto both = pull (p, 7000);
        p.load (dir.getChildFile ("preview/surround.wav"));
        p.play();
        const auto folded = pull (p, 4000);
        juce::AudioBuffer<float> expected (2, 4000);
        expected.clear();
        Surround::foldDown (surround.getArrayOfReadPointers(), 6, expected.getWritePointer (0), expected.getWritePointer (1), 4000);
        check (monoLoaded && same (both, 0, 0, mono, 0, 0, 7000) && same (both, 1, 0, mono, 0, 0, 7000)
                   && same (folded, 0, 0, expected, 0, 0, 4000) && same (folded, 1, 0, expected, 1, 0, 4000),
               "sound library preview: mono plays the same in both channels; 5.1 plays folded down to stereo, as the fold-down gives it");

        // 44.1 kHz on a 48 kHz device: resampled, one second of sound takes one second of the device's frames.
        p.load (dir.getChildFile ("preview/second.wav"));
        p.play();
        int frames = 0;

        while (p.isPlaying() && frames < 96000)
        {
            pull (p, 480);
            frames += 480;
        }

        check (frames >= 47520 && frames <= 48480 && p.getPosition() == 44100,
               "sound library preview: a 44.1 kHz second on a 48 kHz device is resampled, playing for " + juce::String (frames)
                   + " device frames (48000 +- 480) to its end");

        const auto unreadable = dir.getChildFile ("preview/broken.wav");
        unreadable.replaceWithText ("not a wave file");
        check (! p.load (unreadable) && p.getLength() == 0 && ! p.isPlaying() && silent (pull (p, 512), 0, 512),
               "sound library preview: a file that can't be read loads nothing and plays silence");
    }

    void testPart (const juce::File& dir)
    {
        const auto source = noise (2, 10000, 5);
        const auto sound = dir.getChildFile ("part/Glass Hit.wav");
        writeBuffer (sound, source, 96000.0, 24);
        const auto asWritten = readBack (sound);
        const auto parts = dir.getChildFile ("part/Parts");
        const auto part = SoundLibraryPreview::writePart (sound, { 1000, 3500 }, parts);
        double rate = 0.0;
        int bits = 0;
        const auto got = readBack (part, &rate, &bits);
        const auto when = part.getLastModificationTime();
        const auto again = SoundLibraryPreview::writePart (sound, { 1000, 3500 }, parts);

        writeBuffer (dir.getChildFile ("part/Float.wav"), source, 48000.0, 32);
        int floatBits = 0;
        const auto floatPart = readBack (SoundLibraryPreview::writePart (dir.getChildFile ("part/Float.wav"), { 0, 500 }, parts), nullptr, &floatBits);

        check (part.getFileName() == "Glass Hit 0.010-0.036 s.wav" && part.isAChildOf (parts) && got.getNumSamples() == 2500
                   && got.getNumChannels() == 2 && juce::exactlyEqual (rate, 96000.0) && bits == 24
                   && same (got, 0, 0, asWritten, 0, 1000, 2500) && same (got, 1, 0, asWritten, 1, 1000, 2500)
                   && again == part && part.getLastModificationTime() == when
                   && floatBits == 32 && same (floatPart, 0, 0, source, 0, 0, 500)
                   && SoundLibraryPreview::writePart (sound, {}, parts) == juce::File()
                   && ! parts.getChildFile ("Glass Hit 0.010-0.036 s.partial").exists(),
               "sound library preview: a selected part (frames 1000 to 3500 of a 96 kHz 24-bit sound) is written as a WAV of its own, "
               "named after the sound and its times, at the sound's rate, channels and bits, sample for sample; asked again it gives "
               "the same file unwritten; a float sound's part is float; an empty part writes nothing");
    }

    void testPreviewMouse (const juce::File& dir)
    {
        const auto source = noise (2, 48000, 6);
        const auto sound = dir.getChildFile ("mouse/Whoosh.wav");
        writeBuffer (sound, source, 48000.0);
        SoundLibraryPreview preview (dir.getChildFile ("mouse/Parts"));
        preview.setSize (900, 200);
        preview.setPlayOnSelect (false);
        preview.show (sound);
        const auto drawn = TestUtils::runUntil ([&preview] { return preview.isWaveformLoaded(); }, 10000);
        auto& player = preview.getPlayer();
        const auto wave = preview.getWaveArea().toFloat();
        const auto at = [&wave] (float proportion, float y) { return juce::Point<float> (wave.getX() + proportion * wave.getWidth(), y); };
        const auto middle = wave.getCentreY();
        const auto stopped = ! player.isPlaying();

        // Dragging across the waveform selects a part.
        const auto from = at (0.25f, middle), to = at (0.5f, middle);
        preview.mouseDown (mouseAt (preview, from, from));
        preview.mouseDrag (mouseAt (preview, at (0.4f, middle), from));
        preview.mouseDrag (mouseAt (preview, to, from));
        preview.mouseUp (mouseAt (preview, to, from));
        const auto selected = preview.getSelection() == juce::Range<juce::int64> (12000, 24000) && player.getRange() == preview.getSelection();

        // Dragging from inside it carries that part out, once, after 6 pixels.
        juce::StringArray dragged;
        int drags = 0;
        preview.onDragOut = [&] (const juce::StringArray& files) { dragged = files; ++drags; };
        const auto inside = at (0.4f, middle);
        preview.mouseDown (mouseAt (preview, inside, inside));
        preview.mouseDrag (mouseAt (preview, inside + juce::Point<float> (3.0f, 0.0f), inside));
        const auto notYet = drags == 0;
        preview.mouseDrag (mouseAt (preview, inside + juce::Point<float> (40.0f, 30.0f), inside));
        preview.mouseDrag (mouseAt (preview, inside + juce::Point<float> (80.0f, 60.0f), inside));
        preview.mouseUp (mouseAt (preview, inside + juce::Point<float> (80.0f, 60.0f), inside));
        const auto part = dragged.size() == 1 ? readBack (juce::File (dragged[0])) : juce::AudioBuffer<float>();
        const auto carried = notYet && drags == 1 && part.getNumSamples() == 12000 && same (part, 0, 0, source, 0, 12000, 12000)
                             && preview.getSelection() == juce::Range<juce::int64> (12000, 24000);
        check (drawn && stopped && selected && carried,
               "sound library preview: with Play on Select off a shown sound waits; dragging across a quarter of its waveform selects "
               "frames 12000 to 24000 for the player; dragging from inside the selection carries out, once, a file of just those 12000 "
               "frames, sample for sample");

        // A click outside the selection drops it and plays from there; the strip at the top scrubs.
        const auto outside = at (0.8f, middle);
        preview.mouseDown (mouseAt (preview, outside, outside));
        preview.mouseUp (mouseAt (preview, outside, outside));
        const auto clicked = preview.getSelection().isEmpty() && player.isPlaying() && player.getPosition() == 38400;
        player.stop();
        const auto strip = wave.getY() + 3.0f;
        preview.mouseDown (mouseAt (preview, at (0.1f, strip), at (0.1f, strip)));
        const auto scrubStart = player.isPlaying() && player.getPosition() == 4800;
        preview.mouseDrag (mouseAt (preview, at (0.3f, strip), at (0.1f, strip)));
        const auto scrubbed = player.isPlaying() && player.getPosition() == 14400 && preview.getSelection().isEmpty();
        preview.mouseUp (mouseAt (preview, at (0.3f, strip), at (0.1f, strip)));
        preview.togglePlay();
        const auto toggled = ! player.isPlaying();
        preview.togglePlay();
        check (clicked && scrubStart && scrubbed && toggled && player.isPlaying(),
               "sound library preview: a click outside the selection drops it and plays from there (frame 38400 at 80%); pressing the "
               "strip at the top plays from that point and dragging along it moves the playhead with the mouse; Play/Stop toggles");
    }
}

namespace
{
    /** A WAV with an iXML chunk of our own, appended after what JUCE writes. */
    void writeWithIXml (const juce::File& file, int frames, const juce::String& xml)
    {
        writeWav (file, frames, 48000.0, 1, 16);
        juce::MemoryBlock data;
        file.loadFileAsData (data);
        juce::MemoryOutputStream out (data, true);
        const auto text = xml.toUTF8();
        const auto size = (int) std::strlen (text);
        out.writeInt ((int) juce::ByteOrder::littleEndianInt ("iXML"));
        out.writeInt (size);
        out.write (text, (size_t) size);

        if ((size & 1) != 0)
            out.writeByte (0);

        out.flush();
        *reinterpret_cast<juce::uint32*> (static_cast<char*> (data.getData()) + 4) = juce::ByteOrder::swapIfBigEndian ((juce::uint32) (data.getSize() - 8));
        file.replaceWithData (data.getData(), data.getSize());
    }

    void testUcsList()
    {
        using namespace Ucs;
        const auto* wood = byCatId ("DOORWood");
        const auto* lower = byCatId ("doorwood");
        check (list().size() == 753 && categories().size() == 82 && wood != nullptr && wood->describe() == "DOORS / WOOD" && lower == wood
                   && byNames ("doors", "wood") == wood && byCatId ("WOODHndl") != nullptr && byCatId ("WOODHandl") == nullptr
                   && subCategoriesOf ("AIR").size() == 5 && wood->synonyms.size() > 3 && byCatId ("") == nullptr,
               "ucs: the bundled list holds UCS v8.2.1's 753 subcategories in 82 categories, with synonyms; DOORWood is DOORS / WOOD, found "
               "ignoring case and by its names; the 8.2.1 fix is in (WOODHndl, no WOODHandl)");

        check (fromName ("DOORWood_Creak Old Barn_JD_SFX") == wood && fromName ("doorwood_Creak") == nullptr && fromName ("Wind_Chimes") == nullptr && fromName ("WIND_Gust") == byCatId ("WIND") && fromName ("Door Creak") == nullptr
                   && fromName ("_DOORWood") == nullptr && fromName ("NOPEcat_Thing") == nullptr
                   && fromIXml ("<BWFXML><USER><CATID>AIRBlow</CATID></USER></BWFXML>") == byCatId ("AIRBlow")
                   && fromIXml ("<BWFXML><ASWG><catId>AIRHiss</catId></ASWG></BWFXML>") == byCatId ("AIRHiss")
                   && fromIXml ("<BWFXML><USER><CATEGORY>AIR</CATEGORY><SUBCATEGORY>BURST</SUBCATEGORY></USER></BWFXML>") == byCatId ("AIRBrst")
                   && fromIXml ("<BWFXML><USER><CATID>NOPEcat</CATID></USER></BWFXML>") == nullptr && fromIXml ("not xml") == nullptr,
               "ucs: a UCS name gives its CatID (before the first underscore, in its own case: Wind_Chimes is no UCS name); iXML gives USER CATID, the ASWG catId, or USER "
               "CATEGORY and SUBCATEGORY; an unknown CatID or a plain name gives none");
    }

    void testCategories (const juce::File& dir)
    {
        juce::AudioFormatManager formats;
        AudioFileIO::registerFormats (formats);
        const auto library = dir.getChildFile ("Categorised");
        writeWav (library.getChildFile ("DOORWood_Creak Old Barn_JD_SFX.wav"), 1000);
        writeWav (library.getChildFile ("DOORWood_Squeak_JD_SFX.wav"), 1100);
        writeWithIXml (library.getChildFile ("Big Gust 01.wav"), 1200, "<BWFXML><USER><CATID>AIRBlow</CATID></USER></BWFXML>");
        writeWithIXml (library.getChildFile ("Valve Release.wav"), 1300,
                       "<BWFXML><USER><CATEGORY>AIR</CATEGORY><SUBCATEGORY>BURST</SUBCATEGORY></USER></BWFXML>");
        writeWithIXml (library.getChildFile ("DOORMetl_Slam_JD_SFX.wav"), 1400, "<BWFXML><USER><CATID>AIRBlow</CATID></USER></BWFXML>");
        {
            std::unordered_map<juce::String, juce::String> aswg { { juce::WavAudioFormat::aswgCatId, "AIRHiss" } };
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> out (library.getChildFile ("Tyre Leak.wav").createOutputStream());
            auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1)
                                                        .withBitsPerSample (16).withMetadataValues (aswg));
            juce::AudioBuffer<float> b (1, 1500);
            b.clear();
            writer->writeFromAudioSampleBuffer (b, 0, 1500);
        }
        writeWav (library.getChildFile ("Plain Thing.wav"), 1600);

        const auto database = dir.getChildFile ("categories/Index.sqlite");
        SoundLibrary::Index index (database);
        index.addFolder (library);
        const auto first = index.scan (formats);
        const auto one = [&index] (const juce::String& words)
        {
            const auto found = index.find ({ words });
            return found.size() == 1 ? found[0] : SoundLibrary::Sound();
        };
        using From = SoundLibrary::CategoryFrom;
        const auto creak = one ("creak"), squeak = one ("squeak"), gust = one ("gust"), valve = one ("valve"), slam = one ("slam"),
                   leak = one ("leak"), plain = one ("plain");
        check (first.added == 7 && creak.catId == "DOORWood" && creak.categoryFrom == From::name && squeak.catId == "DOORWood"
                   && gust.catId == "AIRBlow" && gust.categoryFrom == From::metadata && valve.catId == "AIRBrst"
                   && leak.catId == "AIRHiss" && leak.categoryFrom == From::metadata && slam.catId == "DOORMetl" && slam.categoryFrom == From::name
                   && plain.catId.isEmpty() && plain.categoryFrom == From::none,
               "sound library categories: a scan takes each sound's category from its UCS name, else from its iXML (USER "
               "CATID, USER CATEGORY and SUBCATEGORY, or the ASWG catId JUCE writes); the name wins over the iXML; a plain name and no "
               "iXML gives none");

        SoundLibrary::Query air;
        air.category = "AIR";
        SoundLibrary::Query blow;
        blow.catId = "AIRBlow";
        SoundLibrary::Query none;
        none.withoutCategory = true;
        SoundLibrary::Query airWithText = air;
        airWithText.text = "gust";
        const auto byCategory = index.countByCategory();
        auto sorted = index.find ({ {}, SoundLibrary::Column::category, true, 0, 10 });
        check (index.count (air) == 3 && index.count (blow) == 1 && index.count (none) == 1 && index.count (airWithText) == 1
                   && index.find (air).size() == 3 && byCategory.at ("DOORWood") == 2 && byCategory.at ("") == 1 && byCategory.size() == 6
                   && index.count ("doors") == 3 && index.count ("burst") == 1 && sorted.size() == 7 && sorted.front().catId == "AIRBlow"
                   && sorted.back().catId.isEmpty(),
               "sound library categories: a query shows one category (AIR: 3), one subcategory, or the sounds with none, with or without "
               "words; counts by CatID; searching finds the category's names (\"doors\" 3, \"burst\" 1); sorted by category, those with "
               "none come last");

        // By hand: kept through a rescan of a changed file, until cleared.
        const auto setPlain = index.setCategory ({ plain.id }, "AIRBlow");
        const auto setCreak = index.setCategory ({ creak.id }, "airhiss");
        const auto refused = ! index.setCategory ({ creak.id }, "NOPEcat");
        writeWav (library.getChildFile ("Plain Thing.wav"), 2600);
        writeWav (library.getChildFile ("DOORWood_Creak Old Barn_JD_SFX.wav"), 2000);
        const auto again = index.scan (formats);
        const auto plainKept = one ("plain"), creakKept = one ("creak");
        const auto byHandFound = index.count ("blow") == 2 && index.count ("hiss") == 2;
        index.setCategory ({ plain.id, creak.id }, {});
        const auto plainCleared = one ("plain"), creakCleared = one ("creak");
        check (setPlain && setCreak && refused && again.updated == 2 && plainKept.catId == "AIRBlow" && plainKept.categoryFrom == From::hand
                   && plainKept.frames == 2600 && creakKept.catId == "AIRHiss" && creakKept.categoryFrom == From::hand && byHandFound
                   && plainCleared.catId.isEmpty() && plainCleared.categoryFrom == From::none && creakCleared.catId == "DOORWood"
                   && creakCleared.categoryFrom == From::name && index.count ("hiss") == 1,
               "sound library categories: a category set by hand (any case; an unknown CatID refused) stays when a changed file is read "
               "again, and is searched by; cleared, each sound has its name's or iXML's again (or none)");

        // An index made before categories is brought up to date and its files read again.
        const auto old = dir.getChildFile ("categories/Old.sqlite");
        {
            sqlite3* raw = nullptr;
            sqlite3_open (old.getFullPathName().toRawUTF8(), &raw);
            const auto creakFile = library.getChildFile ("DOORWood_Creak Old Barn_JD_SFX.wav");
            const auto sql = juce::String ("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);"
                                           "CREATE TABLE folders (path TEXT PRIMARY KEY);"
                                           "CREATE TABLE sounds (id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, folder TEXT NOT NULL,"
                                           " name TEXT NOT NULL, size INTEGER, modified INTEGER, frames INTEGER, rate REAL, channels INTEGER, bits INTEGER,"
                                           " format TEXT, description TEXT, seen INTEGER DEFAULT 0);"
                                           "CREATE VIRTUAL TABLE words USING fts5 (text, tokenize='unicode61');"
                                           "INSERT INTO meta VALUES ('schema', '1');")
                             + "INSERT INTO folders VALUES ('" + library.getFullPathName().replace ("'", "''") + "');"
                             + "INSERT INTO sounds (path, folder, name, size, modified, frames, rate, channels, bits, format, description) VALUES ('"
                             + creakFile.getFullPathName().replace ("'", "''") + "', '" + library.getFullPathName().replace ("'", "''")
                             + "', 'DOORWood_Creak Old Barn_JD_SFX', " + juce::String (creakFile.getSize()) + ", "
                             + juce::String (creakFile.getLastModificationTime().toMilliseconds()) + ", 2000, 48000, 2, 24, 'WAV file', '');"
                             + "INSERT INTO words (rowid, text) VALUES (1, 'DOOR Wood Creak Old Barn JD SFX');";
            sqlite3_exec (raw, sql.toRawUTF8(), nullptr, nullptr, nullptr);
            sqlite3_close (raw);
        }
        SoundLibrary::Index upgraded (old);
        const auto before = upgraded.isOpen() && upgraded.count() == 1 && upgraded.find ({}).front().catId.isEmpty();
        const auto rescan = upgraded.scan (formats);
        check (before && rescan.updated == 1 && rescan.added == 6 && upgraded.find ({ "creak" }).front().catId == "DOORWood",
               "sound library categories: an index made before categories opens with its sounds, gains the columns, and its next scan "
               "reads the files again, giving them their categories");
    }

    void testCategoriesView (const juce::File& dir)
    {
        SoundLibraryView view (dir.getChildFile ("categories view/Index.sqlite"));
        view.setSize (1100, 600);
        view.addFolder (dir.getChildFile ("Categorised"));
        const auto scanned = TestUtils::runUntil ([&view] { return ! view.isScanning() && view.getSummary().contains ("Last scan"); }, 20000);
        const auto counted = view.found() == 7 && view.countInCategory ("AIR") == 3 && view.countWithCatId ("DOORWood") == 2
                             && view.countWithCatId ({}) == 1;
        view.showCategory ("AIR");
        const auto airRows = view.getNumRows() == 3 && view.getSummary().startsWith ("3 of 7 sounds match");
        view.showCategory ("DOORS", "DOORWood");
        const auto woodRows = view.getNumRows() == 2 && view.getText (0, SoundLibraryView::category) == "DOORS / WOOD";
        view.setSearch ("squeak");
        const auto narrowed = view.getNumRows() == 1 && view.countWithCatId ("DOORWood") == 1 && view.found() == 1;
        view.setSearch ({});
        view.showCategory ({}, {}, true);
        const auto noneRows = view.getNumRows() == 1 && view.getText (0, SoundLibraryView::category).isEmpty();
        juce::SparseSet<int> first;
        first.addRange ({ 0, 1 });
        view.selectRows (first);
        const auto set = view.setCategoryOfSelection ("AIRMisc");
        const auto moved = view.getNumRows() == 0 && view.countInCategory ("AIR") == 4;
        view.showCategory ({});
        check (scanned && counted && airRows && woodRows && narrowed && noneRows && set && moved && view.getNumRows() == 7,
               "sound library window: the category list counts the sounds found in each category and subcategory (and with none), "
               "following the search; choosing one shows only those; the Category column names it; setting a selected sound's category "
               "moves it (AIR then holds 4)");
    }
}

namespace
{
    void testTags (const juce::File& dir)
    {
        juce::AudioFormatManager formats;
        AudioFileIO::registerFormats (formats);
        const auto library = dir.getChildFile ("Tagged");
        writeWav (library.getChildFile ("Owl Hoot.wav"), 1000);
        writeWav (library.getChildFile ("Cricket Field.wav"), 1100);
        writeWav (library.getChildFile ("Rain on Roof.wav"), 1200);
        writeWav (library.getChildFile ("Car Pass.wav"), 1300);
        SoundLibrary::Index index (dir.getChildFile ("tags/Index.sqlite"));
        index.addFolder (library);
        index.scan (formats);
        const auto idOf = [&index] (const juce::String& words) { return index.find ({ words }).front().id; };
        const auto owl = idOf ("owl"), cricket = idOf ("cricket"), rain = idOf ("rain"), car = idOf ("car");
        const auto one = [&index] (const juce::String& words) { return index.find ({ words }).front(); };

        const auto set = index.setRating ({ owl }, 4) && index.setRating ({ cricket }, 2) && index.setRating ({ car }, 9)
                         && index.setFavourite ({ owl, rain }, true) && index.addTag ({ owl, cricket }, "  Night  ")
                         && index.addTag ({ cricket }, "night") && index.addTag ({ rain }, "Exterior")
                         && index.addTag ({ car }, "Street\tTraffic\nLoud") && index.addTag ({ car }, juce::String::repeatedString ("x", 70));
        const auto refused = ! index.addTag ({ owl }, "   ");
        SoundLibrary::Query rated, favourites, night, favouriteNight;
        rated.minRating = 3;
        favourites.favouritesOnly = true;
        night.tag = "NIGHT";
        favouriteNight.favouritesOnly = true;
        favouriteNight.tag = "night";
        const auto owlNow = one ("owl"), cricketNow = one ("cricket"), carNow = one ("pass");
        const auto counts = index.tagCounts();
        auto byRating = index.find ({ {}, SoundLibrary::Column::rating, false, 0, 10 });
        check (set && refused && owlNow.rating == 4 && owlNow.favourite && owlNow.tags == juce::StringArray { "Night" }
                   && cricketNow.tags == juce::StringArray { "Night" } && carNow.rating == 5
                   && carNow.tags.contains ("Street Traffic Loud") && carNow.tags.contains (juce::String::repeatedString ("x", 64))
                   && index.count (rated) == 2 && index.count (favourites) == 2 && index.count (night) == 2 && index.count (favouriteNight) == 1
                   && index.count ("night") == 2 && index.count ("exterior") == 1 && index.count ("traffic") == 1
                   && counts.size() == 4 && counts[0].first == "Exterior" && counts[1].first == "Night" && counts[1].second == 2
                   && byRating.size() == 4 && byRating[0].id == car && byRating[1].id == owl,
               "sound library tags: ratings (clamped to 5), favourites and tags (trimmed, one line, at most 64 characters, the same in any "
               "case, never empty) are kept per sound; queries show at least 3 stars (2), favourites (2), a tag in any case (2), and "
               "both; searching finds tags; tags are counted in order; sorting by rating puts 5 stars first");

        // Kept through a rescan of a changed file; a file gone takes its tags with it.
        writeWav (library.getChildFile ("Owl Hoot.wav"), 2000);
        library.getChildFile ("Rain on Roof.wav").deleteFile();
        const auto again = index.scan (formats);
        const auto owlKept = one ("owl");
        index.removeTag ({ cricket }, "NIGHT");
        const auto after = index.tagCounts();
        check (again.updated == 1 && again.removed == 1 && owlKept.rating == 4 && owlKept.favourite && owlKept.tags == juce::StringArray { "Night" }
                   && owlKept.frames == 2000 && index.count ("night") == 1 && one ("cricket").tags.isEmpty() && after.size() == 3
                   && after[0].first == "Night" && after[0].second == 1 && index.count (favourites) == 1,
               "sound library tags: a changed file read again keeps its rating, favourite and tags (and is still found by them); a "
               "removed tag goes from the sound and its search words; a file gone from its folder leaves no tags behind");

        // An index from M282 (version 2) gains ratings and favourites.
        const auto old = dir.getChildFile ("tags/Version2.sqlite");
        {
            sqlite3* raw = nullptr;
            sqlite3_open (old.getFullPathName().toRawUTF8(), &raw);
            sqlite3_exec (raw, "CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);"
                               "CREATE TABLE folders (path TEXT PRIMARY KEY);"
                               "CREATE TABLE sounds (id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, folder TEXT NOT NULL,"
                               " name TEXT NOT NULL, size INTEGER, modified INTEGER, frames INTEGER, rate REAL, channels INTEGER, bits INTEGER,"
                               " format TEXT, description TEXT, seen INTEGER DEFAULT 0,"
                               " catId TEXT, catFrom INTEGER DEFAULT 0, fileCatId TEXT, fileCatFrom INTEGER DEFAULT 0);"
                               "CREATE VIRTUAL TABLE words USING fts5 (text, tokenize='unicode61');"
                               "INSERT INTO meta VALUES ('schema', '2');"
                               "INSERT INTO sounds (path, folder, name, size, modified, frames, rate, channels, bits, format, description, catId, catFrom)"
                               " VALUES ('/nowhere/DOORWood_Creak.wav', '/nowhere', 'DOORWood_Creak', 1, 1, 1, 48000, 1, 16, 'WAV file', '', 'DOORWood', 1);"
                               "INSERT INTO words (rowid, text) VALUES (1, 'DOOR Wood Creak');",
                          nullptr, nullptr, nullptr);
            sqlite3_close (raw);
        }
        SoundLibrary::Index two (old);
        const auto opened = two.isOpen() && two.count() == 1;
        const auto id = opened ? two.find ({}).front().id : 0;
        const auto upgraded = two.setRating ({ id }, 3) && two.setFavourite ({ id }, true) && two.addTag ({ id }, "Old");
        const auto kept = opened ? two.find ({ "old" }) : std::vector<SoundLibrary::Sound>();
        check (opened && upgraded && kept.size() == 1 && kept[0].rating == 3 && kept[0].favourite && kept[0].catId == "DOORWood",
               "sound library tags: an index from before ratings (version 2) opens with its sounds and categories, and takes ratings, "
               "favourites and tags");

        // An index from M283 (version 3): names that matched a CatID only ignoring case lose that category.
        const auto names = dir.getChildFile ("Case");
        writeWav (names.getChildFile ("Wind_Chimes.wav"), 700);
        writeWav (names.getChildFile ("beep_glitch.wav"), 800);
        writeWav (names.getChildFile ("DOORWood_Creak.wav"), 900);
        const auto three = dir.getChildFile ("tags/Version3.sqlite");
        {
            SoundLibrary::Index made (three);
            made.addFolder (names);
            made.scan (formats);
        }
        {
            sqlite3* raw = nullptr;
            sqlite3_open (three.getFullPathName().toRawUTF8(), &raw);
            sqlite3_exec (raw, "UPDATE sounds SET catId = 'WIND', catFrom = 1, fileCatId = 'WIND', fileCatFrom = 1 WHERE name = 'Wind_Chimes';"
                               "UPDATE sounds SET catId = 'DOORMetl', catFrom = 3, fileCatId = 'BEEP', fileCatFrom = 1 WHERE name = 'beep_glitch';"
                               "UPDATE words SET text = text || ' WIND GENERAL' WHERE rowid = (SELECT id FROM sounds WHERE name = 'Wind_Chimes');"
                               "UPDATE meta SET value = '3' WHERE key = 'schema';",
                          nullptr, nullptr, nullptr);
            sqlite3_close (raw);
        }
        SoundLibrary::Index four (three);
        const auto named = [&four] (const char* word) { const auto f = four.find ({ word }); return f.size() == 1 ? f[0] : SoundLibrary::Sound(); };
        const auto chimes = named ("chimes"), glitch = named ("glitch"), creak = named ("creak");
        const auto generalGone = four.count ("general") == 0;
        const auto rescanned = four.scan (formats);
        const auto glitchAfter = named ("glitch");
        check (four.isOpen() && chimes.catId.isEmpty() && chimes.categoryFrom == SoundLibrary::CategoryFrom::none && generalGone
                   && glitch.catId == "DOORMetl" && glitch.categoryFrom == SoundLibrary::CategoryFrom::hand && creak.catId == "DOORWood"
                   && rescanned.updated == 2 && named ("chimes").catId.isEmpty() && glitchAfter.catId == "DOORMetl",
               "sound library categories: an index from before names matched CatIDs in their own case (version 3) opens with "
               "Wind_Chimes no longer WIND (nor searched by it), beep_glitch keeping the category set by hand, DOORWood_Creak keeping "
               "its own; the next scan reads the two again");
    }

    void testTagsView (const juce::File& dir)
    {
        SoundLibraryView view (dir.getChildFile ("tags view/Index.sqlite"));
        view.setSize (1300, 700);
        view.addFolder (dir.getChildFile ("Tagged"));
        TestUtils::runUntil ([&view] { return ! view.isScanning() && view.getSummary().contains ("Last scan"); }, 20000);
        view.setSort (SoundLibraryView::name, true);
        juce::SparseSet<int> first;
        first.addRange ({ 0, 1 });
        view.selectRows (first);
        const auto chosen = view.getSelectedFiles();
        view.setRatingOfSelection (3);
        const auto ratedKept = view.getText (0, SoundLibraryView::rating) == "3" && view.getSelectedFiles() == chosen;
        auto& asComponent = static_cast<juce::Component&> (view);
        asComponent.keyPressed (juce::KeyPress ('5', {}, '5'));
        const auto keyed = view.getText (0, SoundLibraryView::rating) == "5";
        asComponent.keyPressed (juce::KeyPress ('f', {}, 'f'));
        const auto marked = view.getText (0, SoundLibraryView::favourite).isNotEmpty();
        asComponent.keyPressed (juce::KeyPress ('f', {}, 'f'));
        const auto unmarked = view.getText (0, SoundLibraryView::favourite).isEmpty();

        // The stars and the heart, clicked.
        view.clickCell (1, SoundLibraryView::rating, 6 + 14 + 5);
        const auto twoStars = view.getText (1, SoundLibraryView::rating) == "2";
        view.clickCell (1, SoundLibraryView::rating, 6 + 14 + 5);
        const auto cleared = view.getText (1, SoundLibraryView::rating).isEmpty();
        view.clickCell (1, SoundLibraryView::favourite, 13);
        const auto hearted = view.getText (1, SoundLibraryView::favourite).isNotEmpty();
        check (ratedKept && keyed && marked && unmarked && twoStars && cleared && hearted,
               "sound library window: rating the selection keeps it selected; the 5 key rates it 5 stars, F marks and unmarks it; "
               "clicking the second star rates a sound 2, clicking it again clears it; clicking the heart marks it");

        view.addTagToSelection ("Night");
        const auto tagged = view.getText (0, SoundLibraryView::tags) == "Night"
                            && view.getTagChoices() == juce::StringArray { "Any Tag", "Night (1)" };
        view.showTag ("Night");
        const auto byTag = view.getNumRows() == 1;
        view.showTag ({});
        view.showMinRating (4);
        const auto byStars = view.getNumRows() == 1;
        view.showMinRating (0);
        view.showFavouritesOnly (true);
        const auto byHeart = view.getNumRows() == 1 && view.getSummary().startsWith ("1 of 3 sounds match");
        view.showFavouritesOnly (false);
        view.selectRows (first);
        view.removeTagFromSelection ("night");
        check (tagged && byTag && byStars && byHeart && view.getText (0, SoundLibraryView::tags).isEmpty()
                   && view.getTagChoices() == juce::StringArray { "Any Tag" } && view.getNumRows() == 3,
               "sound library window: a tag added to the selection shows in its row and in the tag filter with its count; the bar shows "
               "only that tag, at least 4 stars, or favourites (1 of 3 each); removing the tag takes it out of the filter");
    }
}

namespace
{
    /** Appends a chunk to a WAV file and corrects its RIFF size. */
    void appendChunk (const juce::File& file, const char* id, const juce::MemoryBlock& contents)
    {
        juce::MemoryBlock data;
        file.loadFileAsData (data);
        juce::MemoryOutputStream out (data, true);
        out.writeInt ((int) juce::ByteOrder::littleEndianInt (id));
        out.writeInt ((int) contents.getSize());
        out.write (contents.getData(), contents.getSize());

        if ((contents.getSize() & 1) != 0)
            out.writeByte (0);

        out.flush();
        *reinterpret_cast<juce::uint32*> (static_cast<char*> (data.getData()) + 4) = juce::ByteOrder::swapIfBigEndian ((juce::uint32) (data.getSize() - 8));
        file.replaceWithData (data.getData(), data.getSize());
    }

    juce::MemoryBlock bytesOf (const juce::File& f)
    {
        juce::MemoryBlock b;
        f.loadFileAsData (b);
        return b;
    }

    juce::MemoryBlock chunkBytes (const juce::File& f, const SoundMetadata::Chunk& c)
    {
        const auto all = bytesOf (f);
        return { static_cast<const char*> (all.getData()) + c.start, (size_t) c.size };
    }

    juce::String ixmlOf (const juce::File& f)
    {
        for (const auto& c : SoundMetadata::chunksOf (f))
            if (c.id == "iXML")
                return chunkBytes (f, c).toString();

        return {};
    }

    /** Every chunk but the iXML the same in both files, in the same order (the iXML where it was, or last). */
    bool keptButIXml (const juce::File& before, const juce::File& after)
    {
        auto a = SoundMetadata::chunksOf (before), b = SoundMetadata::chunksOf (after);
        const auto drop = [] (std::vector<SoundMetadata::Chunk>& v) { v.erase (std::remove_if (v.begin(), v.end(), [] (auto& c) { return c.id == "iXML"; }), v.end()); };
        drop (a);
        drop (b);

        if (a.empty() || a.size() != b.size())
            return false;

        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].id != b[i].id || chunkBytes (before, a[i]) != chunkBytes (after, b[i]))
                return false;

        return true;
    }

    bool noTemporaryFiles (const juce::File& folder)
    {
        return folder.findChildFiles (juce::File::findFiles, false, "*.spectrablade-partial", juce::File::FollowSymlinks::no).isEmpty();
    }

    void testMetadata (const juce::File& dir)
    {
        const auto folder = dir.getChildFile ("Write");
        const auto full = folder.getChildFile ("Creaky Old Door.wav");
        writeWav (full, 3001, 48000.0, 2, 24, "An old barn door");
        appendChunk (full, "smed", juce::MemoryBlock ("odd size", 7));
        const juce::String oldXml ("<?xml version=\"1.0\"?><BWFXML><IXML_VERSION>1.61</IXML_VERSION><PROJECT>Film</PROJECT>"
                                   "<USER><NOTE>keep this</NOTE><CATID>AIRBlow</CATID></USER></BWFXML>");
        appendChunk (full, "iXML", juce::MemoryBlock (oldXml.toRawUTF8(), oldXml.getNumBytesAsUTF8()));
        appendChunk (full, "LIST", juce::MemoryBlock ("INFOICMT\x04\0\0\0abc", 16));
        const auto original = folder.getChildFile ("original.copy");
        full.copyFileTo (original);
        std::filesystem::permissions (full.getFullPathName().toStdString(), std::filesystem::perms (0640));
        // As the system keeps them: Windows has only a read-only flag, not the owner, group and others' bits.
        const auto permsBefore = std::filesystem::status (full.getFullPathName().toStdString()).permissions();

        const auto written = SoundMetadata::write (full, { "DOORWood", { "Night", "Exterior" } });
        const auto doc = juce::parseXML (ixmlOf (full));
        const auto* user = doc != nullptr ? doc->getChildByName ("USER") : nullptr;
        const auto* aswg = doc != nullptr ? doc->getChildByName ("ASWG") : nullptr;
        const auto text = [] (const juce::XmlElement* e, const char* tag) { return e != nullptr && e->getChildByName (tag) != nullptr ? e->getChildByName (tag)->getAllSubText() : juce::String(); };
        juce::StringArray ids;

        for (const auto& c : SoundMetadata::chunksOf (full))
            ids.add (c.id);

        juce::AudioFormatManager formats;
        AudioFileIO::registerFormats (formats);
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (full));
        const auto riffSize = (juce::int64) juce::ByteOrder::littleEndianInt (static_cast<const char*> (bytesOf (full).getData()) + 4);
        check (written.isEmpty() && keptButIXml (original, full) && ids.indexOf ("iXML") == ids.indexOf ("smed") + 1 && ids[ids.size() - 1] == "LIST" && ids.contains ("data")
                   && text (user, "CATID") == "DOORWood" && text (user, "CATEGORY") == "DOORS" && text (user, "SUBCATEGORY") == "WOOD"
                   && text (user, "CATEGORYFULL") == "DOORS-WOOD" && text (user, "KEYWORDS") == "Night, Exterior" && text (user, "NOTE") == "keep this"
                   && text (doc.get(), "PROJECT") == "Film" && text (aswg, "catId") == "DOORWood" && text (aswg, "subCategory") == "WOOD"
                   && Ucs::fromIXml (ixmlOf (full)) == Ucs::byCatId ("DOORWood") && reader != nullptr && reader->lengthInSamples == 3001
                   && riffSize == full.getSize() - 8 && noTemporaryFiles (folder)
                   && std::filesystem::status (full.getFullPathName().toStdString()).permissions() == permsBefore,
               "sound library writing: a category and tags go into a WAV file's iXML (USER CATID, CATEGORY, SUBCATEGORY, CATEGORYFULL, "
               "KEYWORDS; ASWG catId, category, subCategory), replacing the CatID there and keeping the rest of the iXML; every other "
               "chunk (fmt, data, bext, an odd-sized chunk, LIST) is kept byte for byte and in its place, the RIFF size is right, the "
               "file keeps its permissions and no temporary file is left");

        // A file without iXML gets one at its end; tags alone leave the category alone.
        const auto plain = folder.getChildFile ("Plain.wav");
        writeWav (plain, 1001, 44100.0, 1, 16);
        const auto plainBefore = folder.getChildFile ("plain.copy");
        plain.copyFileTo (plainBefore);
        const auto plainWritten = SoundMetadata::write (plain, { {}, { "Room" } });
        const auto plainChunks = SoundMetadata::chunksOf (plain);
        const auto plainDoc = juce::parseXML (ixmlOf (plain));
        check (plainWritten.isEmpty() && keptButIXml (plainBefore, plain) && ! plainChunks.empty() && plainChunks.back().id == "iXML"
                   && plainDoc != nullptr && plainDoc->getChildByName ("USER") != nullptr
                   && text (plainDoc->getChildByName ("USER"), "KEYWORDS") == "Room" && text (plainDoc->getChildByName ("USER"), "CATID").isEmpty()
                   && plainDoc->getChildByName ("ASWG") == nullptr,
               "sound library writing: a WAV file without iXML gets one at its end, the rest kept byte for byte; tags alone write only "
               "KEYWORDS");

        // Refusals leave the file as it was.
        const auto unchanged = [] (const juce::File& f, const juce::MemoryBlock& before) { return bytesOf (f) == before; };
        const auto aiff = folder.getChildFile ("Sound.aiff");
        {
            juce::AiffAudioFormat format;
            std::unique_ptr<juce::OutputStream> out (aiff.createOutputStream());
            auto writer = format.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (1).withBitsPerSample (16));
            juce::AudioBuffer<float> b (1, 100);
            b.clear();
            writer->writeFromAudioSampleBuffer (b, 0, 100);
        }
        const auto rf64 = folder.getChildFile ("Big.wav");
        writeWav (rf64, 500);
        {
            auto b = bytesOf (rf64);
            std::memcpy (b.getData(), "RF64", 4);
            rf64.replaceWithData (b.getData(), b.getSize());
        }
        const auto badXml = folder.getChildFile ("Bad iXML.wav");
        writeWav (badXml, 500);
        appendChunk (badXml, "iXML", juce::MemoryBlock ("<BWFXML><USER>", 14));
        const auto nul = folder.getChildFile ("NUL iXML.wav");
        writeWav (nul, 500);
        appendChunk (nul, "iXML", juce::MemoryBlock ("<BWFXML><BWF_VERSION>\x01\0</BWF_VERSION><NOTE>after</NOTE></BWFXML>", 64));
        const auto cut = folder.getChildFile ("Cut.wav");
        writeWav (cut, 5000);
        {
            auto b = bytesOf (cut);
            cut.replaceWithData (b.getData(), b.getSize() - 100);
        }
        const auto locked = folder.getChildFile ("Locked.wav");
        writeWav (locked, 500);
        locked.setReadOnly (true);
        const auto aiffBytes = bytesOf (aiff), rf64Bytes = bytesOf (rf64), badBytes = bytesOf (badXml), cutBytes = bytesOf (cut), lockedBytes = bytesOf (locked), nulBytes = bytesOf (nul);
        const auto whyNul = SoundMetadata::write (nul, { "DOORWood", {} });
        const auto whyAiff = SoundMetadata::write (aiff, { "DOORWood", {} });
        const auto whyRf64 = SoundMetadata::write (rf64, { "DOORWood", {} });
        const auto whyBad = SoundMetadata::write (badXml, { "DOORWood", {} });
        const auto whyCut = SoundMetadata::write (cut, { "DOORWood", {} });
        const auto whyLocked = SoundMetadata::write (locked, { "DOORWood", {} });
        const auto whyNothing = SoundMetadata::write (plain, { "NOPEcat", {} });
        locked.setReadOnly (false);
        check (whyAiff.contains ("isn't a WAV") && whyRf64.contains ("RF64") && whyBad.contains ("iXML") && whyCut.contains ("past the end")
                   && whyLocked.contains ("can't be written") && whyNothing.contains ("no category or tag") && whyNul.contains ("aren't text")
                   && unchanged (nul, nulBytes)
                   && unchanged (aiff, aiffBytes) && unchanged (rf64, rf64Bytes) && unchanged (badXml, badBytes) && unchanged (cut, cutBytes)
                   && unchanged (locked, lockedBytes) && noTemporaryFiles (folder),
               "sound library writing: refused, saying why and leaving the file as it was: an AIFF file (\"" + whyAiff + "\"), an RF64 "
               "file, an iXML that isn't XML, an iXML with a NUL and a control byte inside (as some recorders write), chunks running past the end, a read-only file, and nothing (or an unknown CatID) to write");

        // What was written is what the next scan reads; the window writes the selection.
        original.deleteFile();
        plainBefore.deleteFile();
        SoundLibraryView view (dir.getChildFile ("write view/Index.sqlite"));
        view.setSize (1300, 700);
        const auto viewFolder = dir.getChildFile ("Write View");
        writeWav (viewFolder.getChildFile ("Gate Clang.wav"), 800);
        writeWav (viewFolder.getChildFile ("Gate Squeak.wav"), 900);
        writeWav (viewFolder.getChildFile ("Untouched.wav"), 1000);
        view.addFolder (viewFolder);
        TestUtils::runUntil ([&view] { return ! view.isScanning() && view.getSummary().contains ("Last scan"); }, 20000);
        view.setSort (SoundLibraryView::name, true);
        juce::SparseSet<int> gates;
        gates.addRange ({ 0, 2 });
        view.selectRows (gates);
        view.setCategoryOfSelection ("DOORMetl");
        view.selectRows (gates);
        view.addTagToSelection ("Gate");
        juce::SparseSet<int> all;
        all.addRange ({ 0, 3 });
        view.selectRows (all);
        const auto before = bytesOf (viewFolder.getChildFile ("Untouched.wav"));
        view.writeSelectionIntoFiles();
        const auto finished = TestUtils::runUntil ([&view] { return ! view.isWriting() && ! view.isScanning() && view.getSummary().contains ("Last scan"); }, 20000);
        const auto clang = Ucs::fromIXml (ixmlOf (viewFolder.getChildFile ("Gate Clang.wav")));
        const auto summary = view.getSummary();
        SoundLibrary::Index check2 (dir.getChildFile ("write view/Index.sqlite"));
        const auto squeak = check2.find ({ "squeak" });
        check (finished && clang == Ucs::byCatId ("DOORMetl") && unchanged (viewFolder.getChildFile ("Untouched.wav"), before)
                   && summary.contains ("Wrote 2 files, skipped 1 with no category or tag. Last scan") && squeak.size() == 1 && squeak[0].catId == "DOORMetl" && squeak[0].tags == juce::StringArray { "Gate" } && noTemporaryFiles (viewFolder),
               "sound library writing: the window writes the selected sounds' categories and tags on its own thread, skips one with "
               "neither (left byte for byte), rescans, and keeps the category and tags (\"" + summary.fromFirstOccurrenceOf ("Wrote", true, false).upToFirstOccurrenceOf (". Last", false, false) + "\")");
    }
}

namespace
{
    void testInsert (const juce::File& dir)
    {
        SoundLibraryView view (dir.getChildFile ("insert/Index.sqlite"));
        view.setSize (1300, 700);
        const auto folder = dir.getChildFile ("Sounds to Insert");
        writeWav (folder.getChildFile ("Bell.wav"), 4800, 48000.0, 2, 24);
        writeWav (folder.getChildFile ("Chime.wav"), 2400, 48000.0, 1, 16);
        view.addFolder (folder);
        TestUtils::runUntil ([&view] { return ! view.isScanning() && view.getSummary().contains ("Last scan"); }, 20000);
        view.setSort (SoundLibraryView::name, true);

        const auto nothing = ! view.insertSelection();
        std::vector<juce::StringArray> inserted;
        auto accept = true;
        view.onInsert = [&] (const juce::StringArray& files) { inserted.push_back (files); return accept; };
        const auto noneSelected = ! view.insertSelection() && inserted.empty();

        juce::SparseSet<int> bell;
        bell.addRange ({ 0, 1 });
        view.selectRows (bell);
        const auto byReturn = view.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        const auto whole = inserted.size() == 1 && inserted[0] == juce::StringArray { folder.getChildFile ("Bell.wav").getFullPathName() };
        accept = false;
        const auto refused = ! view.insertSelection() && inserted.size() == 2;
        accept = true;
        check (nothing && noneSelected && byReturn && whole && refused,
               "sound library insert: Return inserts the selected sound's file through the main window's hook; nothing selected (or no "
               "hook) inserts nothing, and a refused insert says so");

        // Part of the waveform selected: that part, written as a drag writes it.
        TestUtils::runUntil ([&view] { return view.getPreview().getPlayer().getLength() > 0; }, 5000);
        view.getPreview().setSelection ({ 1200, 3600 });
        inserted.clear();
        view.insertSelection();
        std::unique_ptr<juce::AudioFormatReader> part;

        if (inserted.size() == 1 && inserted[0].size() == 1)
        {
            juce::WavAudioFormat wav;
            part.reset (wav.createReaderFor (juce::File (inserted[0][0]).createInputStream().release(), true));
        }

        check (part != nullptr && part->lengthInSamples == 2400 && part->numChannels == 2 && part->bitsPerSample == 24
                   && juce::File (inserted[0][0]).getFileName().startsWith ("Bell ") && ! juce::File (inserted[0][0]).isAChildOf (folder),
               "sound library insert: with part of the waveform selected, Return inserts that part (2,400 of 4,800 frames, its "
               "channels and bits kept), written outside the library");

        // Several sounds: all of them whole, whatever the preview's selection.
        juce::SparseSet<int> both;
        both.addRange ({ 0, 2 });
        view.selectRows (both);
        view.getPreview().setSelection ({ 1200, 3600 });
        inserted.clear();
        view.insertSelection();
        check (inserted.size() == 1 && inserted[0] == juce::StringArray { folder.getChildFile ("Bell.wav").getFullPathName(),
                                                                          folder.getChildFile ("Chime.wav").getFullPathName() },
               "sound library insert: with several sounds selected, Return inserts them all whole, in the list's order");
    }
}

void runSoundLibraryTests()
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("SoundLibraryTests", {}, false);
    dir.createDirectory();
    testWords();
    testScan (dir);
    testView (dir);
    testFolders (dir);
    testPlayer (dir);
    testPart (dir);
    testPreviewMouse (dir);
    testUcsList();
    testCategories (dir);
    testCategoriesView (dir);
    testTags (dir);
    testTagsView (dir);
    testFormats (dir);
    testMetadata (dir);
    testInsert (dir);
    testLarge (dir);
    dir.deleteRecursively();
}

void runSoundLibraryOn (const juce::File& folder)
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("SoundLibraryOn", {}, false);
    juce::AudioFormatManager formats;
    AudioFileIO::registerFormats (formats);
    const auto now = [] { return juce::Time::getMillisecondCounterHiRes() / 1000.0; };
    const auto database = dir.getChildFile ("Index.sqlite");
    SoundLibrary::Index index (database);
    const auto added = index.addFolder (folder);
    check (index.isOpen() && added, "sound library on " + folder.getFileName() + ": the folder is added" + (added ? juce::String() : " (" + index.getError() + ")"));

    if (! added)
        return;

    std::vector<SoundLibrary::ScanResult> passes;
    std::vector<double> seconds;

    for (int pass = 1; pass <= 2; ++pass)
    {
        const auto started = now();
        auto said = started;
        passes.push_back (index.scan (formats, [&] (int n)
        {
            if (now() - said > 30.0)
            {
                said = now();
                std::cout << "      scan " << pass << ": " << n << " files looked at, " << juce::roundToInt (said - started) << " s" << std::endl;
            }

            return true;
        }));
        seconds.push_back (now() - started);
    }

    const auto& first = passes[0];
    const auto& second = passes[1];
    const auto indexed = index.count();
    const auto megabytes = (double) (database.getSize() + database.getSiblingFile (database.getFileName() + "-wal").getSize()) / (1024.0 * 1024.0);
    check (indexed == first.added && second.unchanged == first.added && second.added == 0 && second.updated == 0 && second.removed == 0,
           "sound library on " + folder.getFileName() + ": " + juce::String (first.added) + " sounds indexed in " + juce::String (seconds[0], 1)
               + " s (" + juce::String (first.unreadable) + " files unreadable), index " + juce::String (megabytes, 1) + " MB; scanned again with "
               + "nothing changed in " + juce::String (seconds[1], 1) + " s, every sound unchanged and none read again");

    juce::String timings;

    for (auto* q : { "door", "door creak", "explosion", "footsteps gravel", "wind", "glass break" })
    {
        const auto t0 = now();
        const auto n = index.count (q);
        const auto t1 = now();
        const auto page = index.find ({ q, SoundLibrary::Column::name, true, 0, 100 });
        const auto t2 = now();
        timings << "\"" << q << "\" " << n << " (" << juce::String ((t1 - t0) * 1000.0, 1) << " ms, first page " << juce::String ((t2 - t1) * 1000.0, 1)
                << " ms); ";
    }

    const auto t0 = now();
    const auto middle = index.find ({ {}, SoundLibrary::Column::name, true, (int) (indexed / 2), 100 });
    timings << "a page halfway down by name " << juce::String ((now() - t0) * 1000.0, 1) << " ms";
    check (! middle.empty(), "sound library on " + folder.getFileName() + ", searches: " + timings);

    // Categories: where they came from, and how fast the list's counts and a category are.
    std::map<int, juce::int64> from;
    {
        sqlite3* raw = nullptr;
        sqlite3_open_v2 (database.getFullPathName().toRawUTF8(), &raw, SQLITE_OPEN_READONLY, nullptr);
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2 (raw, "SELECT catFrom, COUNT(*) FROM sounds GROUP BY catFrom", -1, &st, nullptr);

        while (sqlite3_step (st) == SQLITE_ROW)
            from[sqlite3_column_int (st, 0)] = sqlite3_column_int64 (st, 1);

        sqlite3_finalize (st);
        sqlite3_close (raw);
    }

    const auto c0 = now();
    const auto byCategory = index.countByCategory();
    const auto c1 = now();
    const auto byCategoryDoor = index.countByCategory ("door");
    const auto c2 = now();
    SoundLibrary::Query doors;
    doors.category = "DOORS";
    const auto inDoors = index.count (doors);
    const auto c3 = now();
    const auto doorsPage = index.find (doors);
    const auto c4 = now();
    juce::int64 counted = 0;

    for (const auto& [id, n] : byCategory)
        counted += n;

    juce::ignoreUnused (byCategoryDoor);
    check (from[0] + from[1] + from[2] == indexed && counted == indexed,
           "sound library on " + folder.getFileName() + ", categories: " + juce::String (from[1]) + " from UCS names, " + juce::String (from[2])
               + " from iXML, " + juce::String (from[0]) + " none; counting by category " + juce::String ((c1 - c0) * 1000.0, 1) + " ms (for \"door\" "
               + juce::String ((c2 - c1) * 1000.0, 1) + " ms); DOORS holds " + juce::String (inDoors) + " (counted in "
               + juce::String ((c3 - c2) * 1000.0, 1) + " ms, first page " + juce::String ((c4 - c3) * 1000.0, 1) + " ms, "
               + juce::String ((int) doorsPage.size()) + " rows)");
    dir.deleteRecursively();
}

void runWriteCheckOn (const juce::File& folder)
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("WriteCheck", {}, false);
    dir.createDirectory();
    int copied = 0, written = 0, kept = 0, read = 0, refused = 0;
    juce::StringArray refusals;
    juce::int64 chunks = 0;

    for (const auto& top : folder.findChildFiles (juce::File::findDirectories, false, "*", juce::File::FollowSymlinks::no))
    {
        int taken = 0;

        for (const auto& entry : juce::RangedDirectoryIterator (top, true, "*.wav", juce::File::findFiles))
        {
            if (taken == 2)
                break;

            const auto copy = dir.getChildFile (juce::String (copied) + " " + entry.getFile().getFileName());

            if (! entry.getFile().copyFileTo (copy))
                continue;

            ++taken;
            ++copied;
            const auto before = dir.getChildFile (juce::String (copied) + ".original");
            copy.copyFileTo (before);
            const auto why = SoundMetadata::write (copy, { "DOORWood", { "Write Check", "Second Tag" } });

            if (why.isNotEmpty())
            {
                ++refused;
                refusals.addIfNotAlreadyThere (why);
                const auto untouched = bytesOf (copy) == bytesOf (before);
                kept += untouched ? 1 : 0;
            }
            else
            {
                ++written;
                const auto same = keptButIXml (before, copy);
                const auto doc = juce::parseXML (ixmlOf (copy));
                const auto* user = doc != nullptr ? doc->getChildByName ("USER") : nullptr;
                const auto* keywords = user != nullptr ? user->getChildByName ("KEYWORDS") : nullptr;
                kept += same && Ucs::fromIXml (ixmlOf (copy)) == Ucs::byCatId ("DOORWood") && keywords != nullptr
                        && keywords->getAllSubText() == "Write Check, Second Tag" ? 1 : 0;
                chunks += (juce::int64) SoundMetadata::chunksOf (copy).size();
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::AudioFormatReader> r (wav.createReaderFor (copy.createInputStream().release(), true));
                read += r != nullptr && r->lengthInSamples > 0 ? 1 : 0;
            }

            before.deleteFile();
            copy.deleteFile();
        }
    }

    check (copied > 0 && kept == copied && read == written,
           "writing into real files from " + folder.getFileName() + ": " + juce::String (copied) + " copies, " + juce::String (written)
               + " written (" + juce::String (chunks) + " chunks in all, every one but the iXML kept byte for byte, the category and tags "
               "read back, the audio readable), " + juce::String (refused) + " refused and left as they were"
               + (refusals.isEmpty() ? juce::String() : " (" + refusals.joinIntoString ("; ") + ")"));
    dir.deleteRecursively();
}
