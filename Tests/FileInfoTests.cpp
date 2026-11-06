#include <JuceHeader.h>
#include "../Source/FileInfo.h"
#include "../Source/AudioFileIO.h"
#include "../Source/AudioEdits.h"
#include "../Source/LossyFiles.h"
#include "../Source/Ffmpeg.h"
#include "TestUtils.h"
#include "FileInfoTests.h"

namespace
{
    using TestUtils::check;

    FileInfo sampleInfo()
    {
        FileInfo info;
        info.title = juce::String::fromUTF8 ("Caf\xc3\xa9 at Dawn \xe2\x98\x95");
        info.artist = "The Testers";
        info.album = "Measured Things";
        info.genre = "Ambient";
        info.year = "2026";
        info.track = "3/12";
        info.comment = juce::String::fromUTF8 ("Recorded in one take. \xc3\x9c" "bung.");
        return info;
    }

    bool sameTags (const FileInfo& a, const FileInfo& b)
    {
        return a.title == b.title && a.artist == b.artist && a.album == b.album && a.genre == b.genre && a.year == b.year
            && a.track == b.track && a.comment == b.comment;
    }

    bool contains (const juce::File& file, const char* text)
    {
        juce::MemoryBlock data;
        file.loadFileAsData (data);
        const auto* begin = (const char*) data.getData();
        const auto* end = begin + data.getSize();
        return std::search (begin, end, text, text + std::strlen (text)) != end;
    }

    /** The tags ffmpeg finds in a file (its ffmetadata output), as key=value lines; empty without ffmpeg. */
    juce::StringPairArray ffmpegTags (const juce::File& ffmpeg, const juce::File& file)
    {
        juce::StringPairArray tags;

        if (ffmpeg == juce::File())
            return tags;

        juce::TemporaryFile out (".txt");
        juce::ChildProcess p;

        if (! p.start (juce::StringArray { ffmpeg.getFullPathName(), "-nostdin", "-v", "error", "-y", "-i", file.getFullPathName(),
                                           "-f", "ffmetadata", out.getFile().getFullPathName() }))
            return tags;

        p.readAllProcessOutput();

        for (const auto& line : juce::StringArray::fromLines (out.getFile().loadFileAsString()))
        {
            if (line.startsWithChar ('['))   // chapters (the markers) follow the file's own tags
                break;

            if (line.containsChar ('=') && ! line.startsWithChar (';'))
                tags.set (line.upToFirstOccurrenceOf ("=", false, false).toLowerCase(), line.fromFirstOccurrenceOf ("=", false, false));
        }

        return tags;
    }

    bool ffmpegAgrees (const juce::StringPairArray& tags, const FileInfo& info)
    {
        return tags["title"] == info.title && tags["artist"] == info.artist && tags["album"] == info.album && tags["genre"] == info.genre
            && tags["date"] == info.year && tags["track"] == info.track && tags["comment"] == info.comment;
    }
}

void runFileInfoTests()
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    juce::TemporaryFile folderTemp;
    const auto folder = folderTemp.getFile();
    folder.createDirectory();
    const auto ffmpeg = Ffmpeg::find();

    const auto rate = 48000.0;
    juce::AudioBuffer<float> audio (2, 48000);
    juce::Random random (3);

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < audio.getNumSamples(); ++i)
            audio.setSample (ch, i, 0.3f * (random.nextFloat() * 2.0f - 1.0f));

    const auto snapshot = AudioEdits::fromBuffer (juce::AudioBuffer<float> (audio));
    auto info = sampleInfo();
    info.description = "Interview, take 2";
    info.originator = "SpectraBlade";
    info.originatorReference = "SB-0001";
    info.originationDate = "2026-11-05";
    info.originationTime = "14:30:00";
    info.timeReference = 2'592'000'000;   // 15:00 at 48 kHz

    // WAV: the tags in LIST INFO and the broadcast details in bext, beside markers, the audio untouched.
    {
        const auto file = folder.getChildFile ("tagged.wav");
        Markers markers { { 1000, 1000, "Start", 1 }, { 2000, 6000, "Region", 2 } };
        const auto error = AudioFileIO::save (*snapshot, rate, file, 32, formats, markers, info);
        const auto loaded = AudioFileIO::load (file, formats);
        juce::AudioBuffer<float> back (2, 48000);

        if (loaded.snapshot != nullptr)
            loaded.snapshot->read (back, 0, 0, 48000);

        auto identical = loaded.snapshot != nullptr && loaded.snapshot->getNumFrames() == 48000;

        for (int ch = 0; identical && ch < 2; ++ch)
            identical = std::memcmp (back.getReadPointer (ch), audio.getReadPointer (ch), 48000 * sizeof (float)) == 0;

        check (error.isEmpty() && loaded.error.isEmpty() && loaded.info == info && loaded.markers.size() == 2 && identical,
               "file info: a WAV keeps every field (Unicode too) and its markers, and its audio bit for bit");
        check (contains (file, "bext") && contains (file, "INFO") && contains (file, "IPRD"),
               "file info: the WAV has a bext chunk and a LIST INFO chunk");

        if (ffmpeg != juce::File())
        {
            const auto tags = ffmpegTags (ffmpeg, file);
            // ffmpeg shows bext's description and date in place of INFO's comment and year when both are there.
            check (tags["title"] == info.title && tags["artist"] == info.artist && tags["album"] == info.album && tags["genre"] == info.genre
                       && tags["track"] == info.track && tags["comment"] == info.description && tags["date"] == info.originationDate
                       && tags["encoded_by"] == info.originator && tags["originator_reference"] == info.originatorReference
                       && tags["time_reference"] == juce::String (info.timeReference),
                   "file info: ffmpeg reads the same tags and broadcast details from the WAV (bext's description and date over INFO's comment and year)");
        }

        const auto plain = folder.getChildFile ("plain.wav");
        AudioFileIO::save (*snapshot, rate, plain, 24, formats);
        check (! contains (plain, "INFO") && ! contains (plain, "bext") && AudioFileIO::load (plain, formats).info.isEmpty(),
               "file info: a WAV saved with no info has neither chunk");
    }

    // AIFF and FLAC: the tags (not the broadcast details), the audio as it is saved without them.
    for (const auto* name : { "tagged.aif", "tagged16.aiff", "tagged.flac" })
    {
        const auto file = folder.getChildFile (name);
        const auto plainFile = folder.getChildFile (juce::String ("plain-") + name);
        const auto bits = file.getFileName().contains ("16") ? 16 : 24;
        const auto step = 1.0f / (float) (1 << (bits - 1));
        const auto error = AudioFileIO::save (*snapshot, rate, file, bits, formats, {}, info);
        const auto plainError = AudioFileIO::save (*snapshot, rate, plainFile, bits, formats);
        const auto loaded = AudioFileIO::load (file, formats);
        const auto plain = AudioFileIO::load (plainFile, formats);
        auto identical = loaded.snapshot != nullptr && plain.snapshot != nullptr && loaded.snapshot->getNumFrames() == 48000
                         && plain.snapshot->getNumFrames() == 48000;
        juce::AudioBuffer<float> a (2, 48000), b (2, 48000);

        if (identical)
        {
            loaded.snapshot->read (a, 0, 0, 48000);
            plain.snapshot->read (b, 0, 0, 48000);
        }

        // Saving dithers (randomly, by up to a step each way), so the audio is compared to the source within 2 steps.
        auto worst = 0.0f;

        for (int ch = 0; identical && ch < 2; ++ch)
            for (int i = 0; i < 48000; ++i)
                worst = juce::jmax (worst, std::abs (a.getSample (ch, i) - audio.getSample (ch, i)));

        const auto kind = juce::String (bits) + "-bit " + (file.hasFileExtension ("flac") ? "FLAC" : "AIFF");
        check (error.isEmpty() && plainError.isEmpty() && loaded.error.isEmpty() && sameTags (loaded.info, info) && ! loaded.info.hasBroadcast()
                   && plain.info.isEmpty() && identical && worst <= 2.0f * step && file.getSize() > plainFile.getSize(),
               "file info: " + kind + " keeps the tags (Unicode too) and reads back at its length, the audio within 2 steps ("
                   + juce::String (worst / step, 2) + "); saved with no info, it has none");

        // Tagging a copy of the file saved without info leaves its audio bit for bit.
        const auto copy = folder.getChildFile (juce::String ("copy-") + name);
        plainFile.copyFileTo (copy);
        const auto tagError = file.hasFileExtension ("flac") ? FileInfoIO::addFlacComments (copy, info) : FileInfoIO::appendAiffTags (copy, info);
        const auto tagged = AudioFileIO::load (copy, formats);
        auto same = tagError.isEmpty() && tagged.snapshot != nullptr && tagged.snapshot->getNumFrames() == 48000 && sameTags (tagged.info, info);
        juce::AudioBuffer<float> c (2, 48000);

        if (same)
            tagged.snapshot->read (c, 0, 0, 48000);

        for (int ch = 0; same && ch < 2; ++ch)
            same = std::memcmp (c.getReadPointer (ch), b.getReadPointer (ch), 48000 * sizeof (float)) == 0;

        check (same, "file info: tagging a " + kind + " file leaves its audio bit for bit");

        if (ffmpeg != juce::File())
        {
            const auto tags = ffmpegTags (ffmpeg, file);
            const auto trackOk = file.hasFileExtension ("flac") ? tags["track"] == "3" && tags["tracktotal"] == "12" : tags["track"] == info.track;
            juce::ChildProcess decode;
            decode.start (juce::StringArray { ffmpeg.getFullPathName(), "-nostdin", "-v", "error", "-i", file.getFullPathName(), "-f", "null", "-" });
            const auto complaints = decode.readAllProcessOutput().trim();
            check (tags["title"] == info.title && tags["artist"] == info.artist && tags["album"] == info.album && tags["genre"] == info.genre
                       && tags["date"] == info.year && tags["comment"] == info.comment && trackOk && complaints.isEmpty(),
                   "file info: ffmpeg reads the same tags from the " + kind + " (track \"" + tags["track"] + "\", total \"" + tags["tracktotal"]
                       + "\") and decodes it without a complaint" + (complaints.isEmpty() ? juce::String() : ": " + complaints));
        }
    }

    {
        const auto flac = folder.getChildFile ("tagged.flac");
        auto changed = sampleInfo();
        changed.title = "Second Title";
        changed.track = "7";
        changed.comment = {};
        const auto again = FileInfoIO::addFlacComments (flac, changed);
        const auto reloaded = AudioFileIO::load (flac, formats);
        juce::MemoryBlock data;
        flac.loadFileAsData (data);
        int comments = 0;
        const auto* bytes = (const juce::uint8*) data.getData();

        for (size_t p = 4; p + 4 <= data.getSize();)
        {
            comments += (bytes[p] & 0x7f) == 4 ? 1 : 0;

            if ((bytes[p] & 0x80) != 0)
                break;

            p += 4 + (size_t) ((bytes[p + 1] << 16) | (bytes[p + 2] << 8) | bytes[p + 3]);
        }

        check (again.isEmpty() && comments == 1 && reloaded.info.title == "Second Title" && reloaded.info.track == "7" && reloaded.info.comment.isEmpty()
                   && reloaded.snapshot != nullptr && reloaded.snapshot->getNumFrames() == 48000,
               "file info: tagging a FLAC file again replaces its Vorbis comment (one block left) and keeps its audio");

        const auto riff = folder.getChildFile ("x.wav"), aiff = folder.getChildFile ("x.aiff"), mp3 = folder.getChildFile ("x.mp3");
        check (AudioFileIO::canStoreInfo (riff) && AudioFileIO::canStoreInfo (aiff) && AudioFileIO::canStoreInfo (flac) && ! AudioFileIO::canStoreInfo (mp3)
                   && AudioFileIO::canStoreBroadcast (riff) && ! AudioFileIO::canStoreBroadcast (aiff) && ! AudioFileIO::canStoreBroadcast (flac),
               "file info: Save keeps the tags in WAV, AIFF and FLAC, and the broadcast details in WAV only");
    }

    // AIFF files written by another program: the NAME, AUTH and ANNO chunks, and an ID3 chunk in a
    // 32-bit float AIFF-C file (ffmpeg writes ID3v2.4).
    if (ffmpeg != juce::File())
    {
        const auto run = [&ffmpeg, &folder] (const juce::StringArray& options, const juce::File& out)
        {
            juce::StringArray args { ffmpeg.getFullPathName(), "-nostdin", "-v", "error", "-y", "-i", folder.getChildFile ("plain.wav").getFullPathName() };
            args.addArray (options);
            args.add (out.getFullPathName());
            juce::ChildProcess p;
            p.start (args);
            p.readAllProcessOutput();
        };

        const auto aiff = folder.getChildFile ("old-style.aiff");
        run ({ "-metadata", "title=Old Title", "-metadata", "author=Old Artist", "-metadata", "comment=Old note", "-c:a", "pcm_s16be" }, aiff);
        const auto old = AudioFileIO::load (aiff, formats);

        const auto aifc = folder.getChildFile ("id3.aifc");
        auto args = FileInfoIO::ffmpegArguments (info);
        args.addArray ({ "-c:a", "pcm_f32be", "-write_id3v2", "1", "-f", "aiff" });
        run (args, aifc);
        FileInfo fromId3;
        FileInfoIO::readAiff (aifc, fromId3);

        check (contains (aiff, "NAME") && contains (aiff, "AUTH") && ! contains (aiff, "ID3 ") && old.info.title == "Old Title"
                   && old.info.artist == "Old Artist" && old.info.comment == "Old note" && contains (aifc, "AIFC") && contains (aifc, "ID3 ")
                   && sameTags (fromId3, info),
               "file info: an AIFF file's NAME, AUTH and ANNO chunks are read, and the ID3 chunk in an AIFF-C file (both as ffmpeg writes them)");
    }

    // MP3: an ID3v2.3 tag ahead of LAME's tag frame.
    {
        const auto file = folder.getChildFile ("tagged.mp3");
        const auto error = LossyFiles::write (*snapshot, rate, file, {}, {}, info);
        const auto loaded = AudioFileIO::load (file, formats);
        check (error.isEmpty() && contains (file, "ID3") && sameTags (loaded.info, info) && ! loaded.info.hasBroadcast()
                   && loaded.snapshot != nullptr && loaded.snapshot->getNumFrames() == 48000,
               "file info: an MP3 keeps the tags in ID3v2, and still reads back at its exact length ("
                   + juce::String (loaded.snapshot != nullptr ? loaded.snapshot->getNumFrames() : -1) + " frames)");

        if (ffmpeg != juce::File())
            check (ffmpegAgrees (ffmpegTags (ffmpeg, file), info), "file info: ffmpeg reads the same tags from the MP3");

        const auto plain = folder.getChildFile ("plain.mp3");
        LossyFiles::write (*snapshot, rate, plain, {});
        check (! contains (plain, "ID3"), "file info: an MP3 with no info has no ID3 tag");
    }

    // ID3v2.4, as other programs write it: UTF-8 text and TDRC.
    {
        juce::MemoryOutputStream frames;
        const auto frame = [&frames] (const char* id, const juce::String& text)
        {
            frames.write (id, 4);
            const auto size = (int) text.getNumBytesAsUTF8() + 1;

            for (int shift : { 21, 14, 7, 0 })
                frames.writeByte ((char) ((size >> shift) & 0x7f));

            frames.writeShort (0);
            frames.writeByte (3);
            frames.write (text.toRawUTF8(), text.getNumBytesAsUTF8());
        };
        frame ("TIT2", juce::String::fromUTF8 ("Gr\xc3\xbc\xc3\x9f" "e"));
        frame ("TDRC", "2024-05-01");
        frame ("TCON", "(17)Rock");

        juce::MemoryOutputStream tag;
        tag.write ("ID3\x04\x00\x00", 6);
        const auto size = (int) frames.getDataSize();

        for (int shift : { 21, 14, 7, 0 })
            tag.writeByte ((char) ((size >> shift) & 0x7f));

        tag << frames.getMemoryBlock();
        const auto file = folder.getChildFile ("v24.mp3");
        file.replaceWithData (tag.getData(), tag.getDataSize());
        FileInfo read;
        FileInfoIO::readId3v2 (file, read);
        check (read.title == juce::String::fromUTF8 ("Gr\xc3\xbc\xc3\x9f" "e") && read.year == "2024-05-01" && read.genre == "Rock",
               "file info: ID3v2.4 tags are read (UTF-8, TDRC, a genre with its old number)");
    }

    // AAC: the tags in moov/udta/meta/ilst, in the room kept for them.
    {
        const auto file = folder.getChildFile ("tagged.m4a");
        const auto error = LossyFiles::write (*snapshot, rate, file, { LossyFiles::Kind::aac, 256 }, {}, info);
        const auto loaded = AudioFileIO::load (file, formats);
        check (error.isEmpty() && sameTags (loaded.info, info) && loaded.snapshot != nullptr && loaded.snapshot->getNumFrames() == 48000,
               "file info: an .m4a keeps the tags, and still reads back at its exact length");

        if (ffmpeg != juce::File())
            check (ffmpegAgrees (ffmpegTags (ffmpeg, file), info), "file info: ffmpeg reads the same tags from the .m4a");

        auto wordy = info;
        wordy.comment = juce::String::repeatedString ("A long comment. ", 250).trim();
        const auto longFile = folder.getChildFile ("long.m4a");
        const auto longError = LossyFiles::write (*snapshot, rate, longFile, { LossyFiles::Kind::aac, 128 }, {}, wordy);
        const auto longLoaded = AudioFileIO::load (longFile, formats);
        check (longError.isEmpty() && longLoaded.info.comment == wordy.comment && longLoaded.snapshot != nullptr
                   && longLoaded.snapshot->getNumFrames() == 48000,
               "file info: a 4,000-character comment fits in an .m4a too");
    }

    // Through ffmpeg: tags written into an export, and read from a file it opens.
    if (ffmpeg != juce::File())
    {
        const auto wav = folder.getChildFile ("plain.wav");
        const auto opus = folder.getChildFile ("tagged.opus");
        const auto error = Ffmpeg::encode (ffmpeg, wav, opus, Ffmpeg::targets()[0], 128, {}, info);
        const auto loaded = AudioFileIO::load (opus, formats);
        check (error.isEmpty() && sameTags (loaded.info, info),
               "file info: an Opus export through ffmpeg carries the tags, and they are read back when it is opened");
    }

    // FLAC files written by another program: their Vorbis comments are read.
    if (ffmpeg != juce::File())
    {
        const auto flac = folder.getChildFile ("tagged.flac");
        juce::StringArray command { ffmpeg.getFullPathName(), "-nostdin", "-v", "error", "-y", "-i", folder.getChildFile ("plain.wav").getFullPathName() };
        command.addArray (FileInfoIO::ffmpegArguments (info));
        command.addArray ({ "-c:a", "flac", flac.getFullPathName() });
        juce::ChildProcess p;
        p.start (command);
        p.readAllProcessOutput();
        const auto loaded = AudioFileIO::load (flac, formats);
        check (loaded.error.isEmpty() && sameTags (loaded.info, info), "file info: a FLAC file's Vorbis comments are read");
    }

    // The document: the info as an undo step, carried through other edits.
    {
        AudioDocument doc;
        doc.reset (snapshot, rate, {}, 32, {}, sampleInfo());
        auto changed = doc.getInfo();
        changed.title = "Renamed";
        doc.setInfo (changed, "File Info");
        const auto afterSet = doc.getInfo().title == "Renamed" && doc.isDirty() && doc.getUndoDescription() == "File Info";
        doc.apply (AudioEdits::fromBuffer (juce::AudioBuffer<float> (2, 100)), "Crop");
        const auto carried = doc.getInfo().title == "Renamed";
        doc.undo();
        doc.undo();
        check (afterSet && carried && doc.getInfo() == sampleInfo() && ! doc.isDirty(),
               "file info: changing it is an undo step, other edits carry it on, and undo brings back the old info");
    }

    folder.deleteRecursively();
}
