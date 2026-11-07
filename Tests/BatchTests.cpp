// Written files are compared with what the steps give on their own, bit for bit.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/AudioEdits.h"
#include "../Source/AudioFileIO.h"
#include "../Source/Batch.h"
#include "../Source/BuiltInEffects.h"
#include "../Source/Loudness.h"
#include "../Source/OfflineRender.h"
#include "../Source/SampleRateConverter.h"
#include "TestUtils.h"
#include "BatchTests.h"

namespace
{
    using TestUtils::check;

    /** A sine with noise under it, at this level, channels by frames. */
    juce::AudioBuffer<float> tone (int channels, int frames, double rate, float level, int seed)
    {
        juce::Random r (seed);
        juce::AudioBuffer<float> b (channels, frames);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, level * (0.8f * (float) std::sin (juce::MathConstants<double>::twoPi * (220.0 + 110.0 * ch) * i / rate)
                                             + 0.2f * (r.nextFloat() * 2.0f - 1.0f)));

        return b;
    }

    AudioDocument::Snapshot snap (juce::AudioBuffer<float> b)    { return AudioEdits::fromBuffer (std::move (b)); }

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    bool identical (const AudioSnapshot& a, const AudioSnapshot& b)
    {
        if (a.getNumChannels() != b.getNumChannels() || a.getNumFrames() != b.getNumFrames())
            return false;

        const auto x = readAll (a), y = readAll (b);

        for (int ch = 0; ch < x.getNumChannels(); ++ch)
            if (std::memcmp (x.getReadPointer (ch), y.getReadPointer (ch), sizeof (float) * (size_t) x.getNumSamples()) != 0)
                return false;

        return true;
    }

    juce::MemoryBlock bytesOf (const juce::File& f)
    {
        juce::MemoryBlock m;
        f.loadFileAsData (m);
        return m;
    }

    /** Runs process on a background thread, as the app does. */
    Batch::Outcome run (const juce::File& file, const Batch::Settings& settings, AudioEngine& engine, const juce::XmlElement* rack = nullptr)
    {
        Batch::Outcome out;
        std::atomic<bool> done { false };
        std::thread worker ([&] { out = Batch::process (file, settings, engine.formatManager, rack, &engine.pluginFormatManager); done = true; });
        TestUtils::runUntil ([&] { return done.load(); }, 60000);
        worker.join();
        return out;
    }
}

void runBatchTests (AudioEngine& engine)
{
    auto dir = juce::File::createTempFile ("batch");
    dir.createDirectory();
    auto& formats = engine.formatManager;

    // Sources: a stereo 24-bit 44.1 kHz WAV with markers and tags, a mono 16-bit 48 kHz WAV, a
    // silent one, an AIFF in a folder below, a text file, and a hidden file.
    const auto stereo = dir.getChildFile ("Song.wav"), mono = dir.getChildFile ("Voice.wav"), silent = dir.getChildFile ("Silence.wav");
    const auto sub = dir.getChildFile ("More");
    sub.createDirectory();
    const auto aiff = sub.getChildFile ("Drums.aiff");
    FileInfo info;
    info.title = "Song";
    info.artist = "Someone";
    const Markers markers { { 4410, 0, "Hit", -1 }, { 22050, 8820, "Verse", -1 } };
    AudioFileIO::save (*snap (tone (2, 88200, 44100.0, 0.3f, 1)), 44100.0, stereo, 24, formats, markers, info);
    AudioFileIO::save (*snap (tone (1, 48000, 48000.0, 0.05f, 2)), 48000.0, mono, 16, formats);
    juce::AudioBuffer<float> quiet (2, 24000);
    quiet.clear();
    AudioFileIO::save (*snap (quiet), 48000.0, silent, 32, formats);
    AudioFileIO::save (*snap (tone (2, 48000, 48000.0, 0.5f, 3)), 48000.0, aiff, 24, formats);
    dir.getChildFile ("notes.txt").replaceWithText ("not audio");
    dir.getChildFile (".hidden.wav").replaceWithText ("");
    const auto before = bytesOf (stereo);

    // Collecting: audio files only, folders searched, each once, in name order.
    {
        const auto files = Batch::collect ({ dir, stereo, dir.getChildFile ("notes.txt") }, formats);
        juce::StringArray names;

        for (auto& f : files)
            names.add (f.getFileName());

        check (names.joinIntoString (",") == "Drums.aiff,Silence.wav,Song.wav,Voice.wav",
               "batch: a folder gives its audio files and those in its folders, each once, leaving out text and hidden files ("
                   + names.joinIntoString (", ") + ")");
    }

    const auto out = dir.getChildFile ("Out");
    out.createDirectory();

    // Naming: the suffix and type; numbered when taken; never the source.
    {
        Batch::Settings s;
        s.folder = out;
        s.suffix = " (mastered)";
        s.type = "flac";
        const auto first = Batch::targetFor (stereo, s);
        first.replaceWithText ("taken");
        const auto second = Batch::targetFor (stereo, s);
        first.deleteFile();
        Batch::Settings beside;
        const auto besideSource = Batch::targetFor (stereo, beside);
        check (first.getFileName() == "Song (mastered).flac" && second.getFileName() == "Song (mastered) 2.flac"
                   && besideSource == dir.getChildFile ("Song 2.wav"),
               "batch: a file is named after its source with the suffix, numbered when the name is taken or is the source itself");
    }

    // WAV to WAV at 48 kHz, as a whole: the converter's samples, markers moved, tags kept, 24-bit kept.
    {
        Batch::Settings s;
        s.folder = out;
        s.sampleRate = 48000.0;
        s.bitDepth = 32;
        const auto o = run (stereo, s, engine);
        const auto back = AudioFileIO::load (o.written, formats);
        const auto source = AudioFileIO::load (stereo, formats);
        const auto expected = SampleRateConverter::convert (*source.snapshot, 44100.0, 48000.0);
        const auto moved = SampleRateConverter::convert (source.markers, 44100.0, 48000.0);
        check (o.error.isEmpty() && back.snapshot != nullptr && back.sampleRate == 48000.0 && identical (*back.snapshot, *expected)
                   && back.markers == moved && back.info.title == "Song" && back.info.artist == "Someone",
               "batch: converted to 48 kHz it holds the converter's samples bit for bit (as 32-bit float), its markers moved to match, its tags kept");

        Batch::Settings keep;
        keep.folder = out;
        keep.suffix = " kept";
        const auto k = run (stereo, keep, engine);
        const auto kBack = AudioFileIO::load (k.written, formats);
        // Integer files are written with TPDF dither, so the samples move by a step or two.
        float steps = -1.0f;

        if (kBack.snapshot != nullptr && kBack.snapshot->getNumFrames() == source.snapshot->getNumFrames())
        {
            const auto x = readAll (*kBack.snapshot), y = readAll (*source.snapshot);
            steps = 0.0f;

            for (int ch = 0; ch < x.getNumChannels(); ++ch)
                for (int i = 0; i < x.getNumSamples(); ++i)
                    steps = juce::jmax (steps, std::abs (x.getSample (ch, i) - y.getSample (ch, i)) * 8388608.0f);
        }

        check (k.error.isEmpty() && kBack.bitDepth == 24 && kBack.sampleRate == 44100.0 && steps >= 0.0f && steps <= 2.0f
                   && bytesOf (stereo) == before,
               "batch: with nothing asked it writes the file at its own 24 bits and 44.1 kHz, no sample moved by more than the dither ("
                   + juce::String (steps, 0) + " of 2 least significant bits at most); the source is left as it was");
    }

    // Mono, peak, loudness.
    {
        Batch::Settings s;
        s.folder = out;
        s.type = "wav";
        s.bitDepth = 32;
        s.channels = 1;
        s.level = Batch::Level::peak;
        s.peakDb = -1.0f;
        s.suffix = " peak";
        const auto o = run (stereo, s, engine);
        const auto back = AudioFileIO::load (o.written, formats);
        const auto peak = juce::Decibels::gainToDecibels (AudioEdits::peakLevel (*back.snapshot, 0, back.snapshot->getNumFrames()));
        check (o.error.isEmpty() && back.snapshot->getNumChannels() == 1 && std::abs (peak + 1.0f) < 0.001f,
               "batch: fitted to one channel and normalized, its peak is at " + juce::String (peak, 4) + " dBFS (asked -1)");

        Batch::Settings l;
        l.folder = out;
        l.bitDepth = 32;
        l.level = Batch::Level::loudness;
        l.lufs = -20.0;
        l.ceilingDbtp = -1.0;
        l.suffix = " loud";
        const auto lo = run (mono, l, engine);
        const auto loud = AudioFileIO::load (lo.written, formats);
        const auto measured = Loudness::measure (*loud.snapshot, 0, loud.snapshot->getNumFrames(), loud.sampleRate);

        // Too loud for the ceiling without a limiter; with one it gets there.
        l.lufs = -4.0;
        l.suffix = " capped";
        const auto capped = run (aiff, l, engine);
        const auto cappedBack = AudioFileIO::load (capped.written, formats);
        const auto cappedLoud = Loudness::measure (*cappedBack.snapshot, 0, cappedBack.snapshot->getNumFrames(), 48000.0);
        l.limit = true;
        l.suffix = " limited";
        const auto limited = run (aiff, l, engine);
        const auto limitedBack = AudioFileIO::load (limited.written, formats);
        const auto limitedLoud = Loudness::measure (*limitedBack.snapshot, 0, limitedBack.snapshot->getNumFrames(), 48000.0);
        check (lo.error.isEmpty() && measured.has_value() && std::abs (measured->integrated + 20.0) < 0.01 && capped.notes.size() == 1
                   && cappedLoud->integrated < -4.2 && cappedLoud->truePeak <= -0.99 && limited.error.isEmpty()
                   && std::abs (limitedLoud->integrated + 4.0) < 0.06 && limitedLoud->truePeak <= -0.95,
               "batch: matched to -20 LUFS it measures " + Loudness::format (measured->integrated, "LUFS") + "; at -4 the -1 dBTP ceiling stops it at "
                   + Loudness::format (cappedLoud->integrated, "LUFS") + ", said in the report, and through the limiter it reaches "
                   + Loudness::format (limitedLoud->integrated, "LUFS") + " (true peak " + Loudness::format (limitedLoud->truePeak, "dBTP") + ")");

        // A delivery spec as the level: brought to it, with what it measures in the notes.
        Batch::Settings d;
        d.folder = out;
        d.bitDepth = 32;
        d.level = Batch::Level::spec;
        d.specLufs = -14.0;
        d.specToleranceLu = 1.0;
        d.specCeilingDbtp = -1.0;
        d.suffix = " delivered";
        const auto delivered = run (aiff, d, engine);
        const auto deliveredBack = AudioFileIO::load (delivered.written, formats);
        const auto deliveredLoud = Loudness::measure (*deliveredBack.snapshot, 0, deliveredBack.snapshot->getNumFrames(), 48000.0);
        check (delivered.error.isEmpty() && std::abs (deliveredLoud->integrated + 14.0) < 0.06 && deliveredLoud->truePeak <= -1.0
                   && delivered.notes.joinIntoString ("|").contains ("Meet Online -14 LUFS: meets it at")
                   && Batch::report ({ delivered }).contains ("Meet Online -14 LUFS: meets it at"),
               "batch: to the -14 LUFS delivery spec it measures " + Loudness::format (deliveredLoud->integrated, "LUFS") + ", true peak "
                   + Loudness::format (deliveredLoud->truePeak, "dBTP") + ", and the report says it meets it: " + delivered.notes.joinIntoString ("; "));

        Batch::Settings q = l;
        q.suffix = " quiet";
        q.limit = false;
        const auto silentOut = run (silent, q, engine);
        s.suffix = " silent";
        const auto silentPeak = run (silent, s, engine);
        check (silentOut.error.isEmpty() && silentOut.notes.size() == 1 && silentPeak.notes.size() == 1 && silentPeak.written.existsAsFile(),
               "batch: a silent file is written unchanged, with a note that it couldn't be brought to a level");
    }

    // Types: FLAC (16-bit where the source was float), MP3, AAC; markers noted where they aren't kept.
    {
        Batch::Settings s;
        s.folder = out;
        s.type = "flac";
        s.suffix = " flac";
        const auto f = run (stereo, s, engine);
        const auto fBack = AudioFileIO::load (f.written, formats);
        const auto floatSource = out.getChildFile ("Song.wav");   // the 32-bit float one written above
        const auto f32 = run (floatSource, s, engine);
        const auto f32Back = AudioFileIO::load (f32.written, formats);

        s.type = "mp3";
        s.suffix = "";
        const auto m = run (stereo, s, engine);
        const auto mBack = AudioFileIO::load (m.written, formats);
        s.type = "aac";
        const auto a = run (stereo, s, engine);
        const auto aBack = AudioFileIO::load (a.written, formats);
        check (f.error.isEmpty() && fBack.bitDepth == 24 && fBack.info.title == "Song" && f.notes.joinIntoString (";").contains ("markers")
                   && f32Back.bitDepth == 24 && f32.notes.joinIntoString (";").contains ("32-bit float")
                   && m.error.isEmpty() && mBack.snapshot != nullptr && mBack.snapshot->getNumFrames() == 88200 && mBack.info.artist == "Someone"
                   && a.error.isEmpty() && aBack.snapshot != nullptr && aBack.snapshot->getNumFrames() == 88200,
               "batch: written as FLAC at 24 bits (a float source too, with a note), MP3 and AAC at the source's length (88,200 frames), "
               "with its tags; each says its markers aren't kept");

        if (const auto ffmpeg = Ffmpeg::find(); ffmpeg != juce::File())
        {
            s.type = "ffmpeg:libopus";
            s.ffmpegKbps = 320;
            const auto o = run (mono, s, engine);
            const auto oBack = AudioFileIO::load (o.written, formats);
            s.type = "ffmpeg:alac";
            const auto alac = run (stereo, s, engine);
            const auto alacBack = AudioFileIO::load (alac.written, formats);
            check (o.error.isEmpty() && oBack.snapshot != nullptr && oBack.snapshot->getNumFrames() > 40000 && o.written.hasFileExtension ("opus")
                       && o.notes.joinIntoString (";") == "Opus at 256 kbps, the nearest it offers"
                       && alac.error.isEmpty() && alacBack.snapshot != nullptr && alacBack.snapshot->getNumFrames() == 88200
                       && alac.written.getFileName() == "Song 2.m4a" && Batch::nameOf ("aac") == "AAC (.m4a)",
                   "batch: written through ffmpeg as Opus (asked 320 kbps, given its highest, 256, with a note; " + juce::String (oBack.snapshot != nullptr ? oBack.snapshot->getNumFrames() : 0)
                       + " frames) and Apple Lossless (" + alac.written.getFileName() + ", 88,200 frames), which read back; Apple Lossless "
                       "and AAC are both .m4a, and the second takes the next number");
        }
    }

    // The rack: the file comes out as the rack renders it on its own.
    {
        BuiltInEffects::CompressorProcessor c;
        c.setSettings ({ -30.0, 6.0, 3.0, 5.0, 80.0, 4.0 });
        juce::MemoryBlock state;
        c.getStateInformation (state);
        juce::XmlElement rack ("SPECTRABLADE_RACK");
        auto* slot = rack.createNewChildElement ("SLOT");
        juce::PluginDescription d;
        c.fillInPluginDescription (d);
        slot->addChildElement (d.createXml().release());
        slot->createNewChildElement ("STATE")->addTextElement (state.toBase64Encoding());

        Batch::Settings s;
        s.folder = out;
        s.applyRack = true;
        s.bitDepth = 32;
        s.suffix = " rack";
        const auto o = run (stereo, s, engine, &rack);
        const auto back = AudioFileIO::load (o.written, formats);
        auto direct = readAll (*AudioFileIO::load (stereo, formats).snapshot);
        juce::String error;
        std::atomic<bool> done { false };
        std::thread worker ([&] { error = OfflineRender::processThroughRack (rack, engine.pluginFormatManager, 44100.0, direct, {}); done = true; });
        TestUtils::runUntil ([&] { return done.load(); }, 60000);
        worker.join();
        const auto changed = ! identical (*back.snapshot, *AudioFileIO::load (stereo, formats).snapshot);
        check (o.error.isEmpty() && error.isEmpty() && changed && identical (*back.snapshot, *snap (std::move (direct))),
               "batch: through the effects rack (the built-in compressor) it is bit for bit what the rack renders on its own");
    }

    // What can't be done, and the report.
    {
        Batch::Settings s;
        s.folder = out;
        const auto bad = run (dir.getChildFile ("notes.txt"), s, engine);
        Batch::Settings wrong;
        wrong.type = "xyz";
        wrong.folder = dir.getChildFile ("Nowhere");
        std::vector<Batch::Outcome> outcomes { run (mono, s, engine), bad };
        const auto text = Batch::report (outcomes);
        check (bad.written == juce::File() && bad.error.isNotEmpty() && wrong.problem().isNotEmpty() && run (mono, wrong, engine).error.isNotEmpty()
                   && text.startsWith ("1 of 2 files written, 1 not.") && text.contains ("Voice.wav: written as Voice.wav")
                   && text.contains ("notes.txt: not written."),
               "batch: an unreadable file or settings that can't run write nothing and say why; the report gives each file's outcome");
    }

    // A delivery spec out of range is refused before anything runs.
    {
        Batch::Settings own;
        own.level = Batch::Level::spec;
        own.specLufs = -20.0;
        own.specToleranceLu = 1.0;
        own.specCeilingDbtp = -2.0;
        auto tight = own;
        tight.specToleranceLu = 0.05;
        auto high = own;
        high.specCeilingDbtp = 0.5;
        check (own.problem().isEmpty() && tight.problem().contains ("0.1 to 10 LU") && high.problem().contains ("-30 to 0 dBTP"),
               "batch: a custom delivery spec in range runs; a tolerance under 0.1 LU or a ceiling over 0 dBTP is refused, saying the ranges");
    }

    dir.deleteRecursively();
}
