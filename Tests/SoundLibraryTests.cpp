#include <JuceHeader.h>
#include "../Source/SoundLibrary.h"
#include "../Source/SoundLibraryView.h"
#include "../Source/Surround.h"
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
        fm.registerBasicFormats();
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

void runSoundLibraryTests()
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("SoundLibraryTests", {}, false);
    dir.createDirectory();
    testWords();
    testScan (dir);
    testView (dir);
    testPlayer (dir);
    testPart (dir);
    testPreviewMouse (dir);
    testLarge (dir);
    dir.deleteRecursively();
}

void runSoundLibraryOn (const juce::File& folder)
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("SoundLibraryOn", {}, false);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
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
    dir.deleteRecursively();
}

