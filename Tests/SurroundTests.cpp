// Folded and routed samples are compared with the arithmetic done here, bit for bit.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/AudioEdits.h"
#include "../Source/AudioFileIO.h"
#include "../Source/Batch.h"
#include "../Source/Sequences.h"
#include "../Source/Surround.h"
#include "TestUtils.h"
#include "SurroundTests.h"

namespace
{
    using TestUtils::check;
    using Snapshot = AudioDocument::Snapshot;

    /** Six different signals, so a channel in the wrong place shows. */
    juce::AudioBuffer<float> fiveOneBuffer (int frames, int seed)
    {
        juce::Random r (seed);
        juce::AudioBuffer<float> b (6, frames);

        for (int ch = 0; ch < 6; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, 0.15f * (float) std::sin (0.01 * (ch + 1) * i) + 0.02f * (r.nextFloat() - 0.5f) + 0.01f * (float) ch);

        return b;
    }

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    bool sameChannel (const juce::AudioBuffer<float>& a, int chA, const juce::AudioBuffer<float>& b, int chB, int frames)
    {
        return a.getNumSamples() >= frames && b.getNumSamples() >= frames
            && std::memcmp (a.getReadPointer (chA), b.getReadPointer (chB), sizeof (float) * (size_t) frames) == 0;
    }

    bool identical (const AudioSnapshot& a, const AudioSnapshot& b)
    {
        if (a.getNumChannels() != b.getNumChannels() || a.getNumFrames() != b.getNumFrames())
            return false;

        const auto x = readAll (a), y = readAll (b);

        for (int ch = 0; ch < x.getNumChannels(); ++ch)
            if (! sameChannel (x, ch, y, ch, x.getNumSamples()))
                return false;

        return true;
    }

    bool silent (const juce::AudioBuffer<float>& b, int ch, int frames)
    {
        for (int i = 0; i < frames; ++i)
            if (b.getSample (ch, i) != 0.0f)
                return false;

        return true;
    }

    /** The WAV's fmt chunk: its format tag and, for WAVE_FORMAT_EXTENSIBLE, the speaker mask. */
    std::pair<int, juce::uint32> wavLayout (const juce::File& f)
    {
        juce::MemoryBlock m;
        f.loadFileAsData (m);
        const auto* d = static_cast<const juce::uint8*> (m.getData());

        for (size_t i = 12; i + 8 <= m.getSize();)
        {
            const auto size = juce::ByteOrder::littleEndianInt (d + i + 4);

            if (std::memcmp (d + i, "fmt ", 4) == 0 && i + 8 + 24 <= m.getSize())
            {
                const auto tag = (int) juce::ByteOrder::littleEndianShort (d + i + 8);
                return { tag, tag == 0xfffe ? juce::ByteOrder::littleEndianInt (d + i + 8 + 20) : 0u };
            }

            i += 8 + size + (size & 1);
        }

        return { 0, 0 };
    }
}

namespace
{
    /** 7.1: eight channels named, folded down, saved, put together and played like 5.1's six. */
    void testSevenOne (AudioEngine& engine)
    {
        const auto frames = 30000;
        juce::Random r (8);
        juce::AudioBuffer<float> source (8, frames);

        for (int ch = 0; ch < 8; ++ch)
            for (int i = 0; i < frames; ++i)
                source.setSample (ch, i, 0.1f * (float) std::sin (0.013 * (ch + 1) * i) + 0.02f * (r.nextFloat() - 0.5f) + 0.01f * (float) ch);

        const auto sevenOne = AudioEdits::fromBuffer (juce::AudioBuffer<float> (source));

        // Each side: its front, the centre, and its rear and side surrounds, all three at -3 dB; no LFE.
        juce::AudioBuffer<float> folded (2, frames);

        for (int i = 0; i < frames; ++i)
        {
            const auto c = 0.70710678f * source.getSample (2, i);
            folded.setSample (0, i, source.getSample (0, i) + c + 0.70710678f * source.getSample (4, i) + 0.70710678f * source.getSample (6, i));
            folded.setSample (1, i, source.getSample (1, i) + c + 0.70710678f * source.getSample (5, i) + 0.70710678f * source.getSample (7, i));
        }

        juce::StringArray names;

        for (int ch = 0; ch < 8; ++ch)
            names.add (Surround::channelName (8, ch));

        const auto stereo = readAll (*AudioEdits::convertChannels (*sevenOne, 2));
        const auto mono = readAll (*AudioEdits::convertChannels (*sevenOne, 1));
        auto monoOk = mono.getNumChannels() == 1;

        for (int i = 0; monoOk && i < frames; ++i)
            monoOk = mono.getSample (0, i) == 0.5f * folded.getSample (0, i) + 0.5f * folded.getSample (1, i);

        check (names.joinIntoString (" ") == "L R C LFE Lrs Rrs Lss Rss" && Surround::channelFileName ("Mix", 8, 6) == "Mix Lss"
                   && Surround::isSurround (8) && ! Surround::isSurround (7)
                   && stereo.getNumChannels() == 2 && sameChannel (stereo, 0, folded, 0, frames) && sameChannel (stereo, 1, folded, 1, frames) && monoOk,
               "7.1: channels are named L R C LFE Lrs Rrs Lss Rss; made stereo it is each side plus the centre and its rear and side "
               "surrounds at -3 dB, LFE left out, and made mono the average of the two sides, bit for bit");

        // Saved as WAV with the 7.1 speakers and read back; eight named mono files go in 7.1 order.
        auto dir = juce::File::createTempFile ("seven");
        dir.createDirectory();
        const auto file = dir.getChildFile ("Film.wav");
        AudioFileIO::save (*sevenOne, 48000.0, file, 32, engine.formatManager);
        const auto layout = wavLayout (file);
        const auto back = AudioFileIO::load (file, engine.formatManager);
        dir.deleteRecursively();

        juce::Array<juce::File> shuffled;

        for (auto n : { "Mix Rss.wav", "Mix_LFE.wav", "Mix L.wav", "Mix-c.wav", "Mix.lrs.wav", "Mix R.wav", "Mix LSS.wav", "Mix Rrs.wav" })
            shuffled.add (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (n));

        juce::StringArray order;

        for (auto& f : Surround::orderForCombining (shuffled))
            order.add (f.getFileName());

        check (layout.first == 0xfffe && layout.second == 0x63f && back.snapshot != nullptr && identical (*back.snapshot, *sevenOne)
                   && order.joinIntoString (",") == "Mix L.wav,Mix R.wav,Mix-c.wav,Mix_LFE.wav,Mix.lrs.wav,Mix Rrs.wav,Mix LSS.wav,Mix Rss.wav",
               "7.1: a 7.1 WAV is written with speaker mask 0x" + juce::String::toHexString ((int) layout.second) + " (front left, right, "
               "centre, LFE, back left, right, side left, right: the order of its channels) and reads back bit for bit; eight files "
               "named ... L, R, C, LFE, Lrs, Rrs, Lss, Rss go in 7.1 order, whatever their order and case");

        // Played folded down on the mix, or with its six channels past L and R on the next player channels.
        AudioDocument doc;
        doc.reset (sevenOne, 48000.0, {}, 32);
        DocumentSource src (doc);
        src.prepareToPlay (512, 48000.0);
        juce::AudioBuffer<float> two (2, frames);
        src.setNextReadPosition (0);
        src.getNextAudioBlock ({ &two, 0, frames });
        juce::AudioBuffer<float> wide (36, frames);
        src.setSeparateOutputs (4);
        src.setNextReadPosition (0);
        src.getNextAudioBlock ({ &wide, 0, frames });
        auto routed = sameChannel (wide, 0, source, 0, frames) && sameChannel (wide, 1, source, 1, frames);

        for (int ch = 2; ch < 8; ++ch)
            routed = routed && sameChannel (wide, 2 + ch, source, ch, frames);

        check (sameChannel (two, 0, folded, 0, frames) && sameChannel (two, 1, folded, 1, frames) && routed,
               "7.1: a 7.1 file plays folded down on the mix, bit for bit, or as L and R on the mix and the other six channels on the next "
               "six player channels");
    }
}

void runSurroundTests (AudioEngine& engine)
{
    testSevenOne (engine);

    const auto frames = 40000;
    const auto source = fiveOneBuffer (frames, 3);
    const auto fiveOne = AudioEdits::fromBuffer (juce::AudioBuffer<float> (source));

    // What a fold-down is, worked out here: each side, its centre and surround at 0.70710678.
    juce::AudioBuffer<float> folded (2, frames);

    for (int i = 0; i < frames; ++i)
    {
        const auto c = 0.70710678f * source.getSample (2, i);
        folded.setSample (0, i, source.getSample (0, i) + c + 0.70710678f * source.getSample (4, i));
        folded.setSample (1, i, source.getSample (1, i) + c + 0.70710678f * source.getSample (5, i));
    }

    // Names, fold-down and the outputs used.
    {
        juce::StringArray names;

        for (int ch = 0; ch < 6; ++ch)
            names.add (Surround::channelName (6, ch));

        juce::AudioBuffer<float> out (2, frames);
        Surround::foldDown (source.getArrayOfReadPointers(), out.getWritePointer (0), out.getWritePointer (1), frames);
        const auto bits = [] (const juce::BigInteger& b) { return b.toString (2); };

        check (names.joinIntoString (" ") == "L R C LFE Ls Rs" && Surround::channelName (2, 1) == "R" && Surround::channelName (1, 0).isEmpty()
                   && Surround::channelName (4, 3) == "4" && Surround::channelFileName ("Mix", 6, 3) == "Mix LFE" && Surround::channelFileName ("Take", 3, 2) == "Take 3"
                   && sameChannel (out, 0, folded, 0, frames) && sameChannel (out, 1, folded, 1, frames)
                   && bits (Surround::extraOutputs (6, 0, true)) == "111100" && bits (Surround::extraOutputs (6, 2, true)) == "11110000"
                   && bits (Surround::extraOutputs (10, 0, true)) == "11111100" && Surround::extraOutputs (2, 0, true).isZero()
                   && Surround::extraOutputs (6, 0, false).isZero(),
               "surround: channels are named L R C LFE Ls Rs; the fold-down is each side plus its centre and surround at -3 dB, bit for bit; "
               "past the first two, a file's channels use the outputs after the pair (5.1 on 1/2: outputs 3 to 6), at most 8 channels");
    }

    // Mono and stereo from 5.1: the fold-down, through Make Stereo and Mix to Mono, a played step and a batch.
    {
        const auto stereo = readAll (*AudioEdits::convertChannels (*fiveOne, 2));
        const auto mono = readAll (*AudioEdits::convertChannels (*fiveOne, 1));
        auto monoOk = mono.getNumChannels() == 1;

        for (int i = 0; monoOk && i < frames; ++i)
            monoOk = mono.getSample (0, i) == 0.5f * folded.getSample (0, i) + 0.5f * folded.getSample (1, i);

        const auto step = Sequences::apply ({ "stereo", {}, nullptr }, fiveOne, 0, frames, 48000.0, {}, nullptr);

        auto dir = juce::File::createTempFile ("surround");
        dir.createDirectory();
        const auto file = dir.getChildFile ("Film.wav");
        AudioFileIO::save (*fiveOne, 48000.0, file, 32, engine.formatManager);
        Batch::Settings settings;
        settings.channels = 2;
        settings.bitDepth = 32;
        Batch::Outcome outcome;
        std::thread ([&] { outcome = Batch::process (file, settings, engine.formatManager, nullptr, nullptr); }).join();
        const auto batched = AudioFileIO::load (outcome.written, engine.formatManager);

        check (stereo.getNumChannels() == 2 && sameChannel (stereo, 0, folded, 0, frames) && sameChannel (stereo, 1, folded, 1, frames) && monoOk
                   && step.snapshot != nullptr && identical (*step.snapshot, *AudioEdits::fromBuffer (juce::AudioBuffer<float> (folded)))
                   && batched.snapshot != nullptr && identical (*batched.snapshot, *step.snapshot),
               "surround: 5.1 made stereo is the fold-down, and made mono the average of its two sides, bit for bit, the same in a sequence "
               "step and in a batch (which used to keep L and R, or average all six with the LFE)");

        // Saved and read back: WAV says which speakers (L R C LFE Ls Rs), FLAC keeps the six.
        const auto layout = wavLayout (file);
        const auto back = AudioFileIO::load (file, engine.formatManager);
        const auto flac = dir.getChildFile ("Film.flac");
        AudioFileIO::save (*fiveOne, 48000.0, flac, 24, engine.formatManager);
        const auto flacBack = AudioFileIO::load (flac, engine.formatManager);
        check (layout.first == 0xfffe && layout.second == 0x3f && back.snapshot != nullptr && identical (*back.snapshot, *fiveOne)
                   && flacBack.snapshot != nullptr && flacBack.snapshot->getNumChannels() == 6 && flacBack.snapshot->getNumFrames() == frames,
               "surround: a 5.1 WAV is written as WAVE_FORMAT_EXTENSIBLE with speaker mask 0x"
                   + juce::String::toHexString ((int) layout.second) + " (front left, right, centre, LFE, back left, right) and reads back "
                   "bit for bit (32-bit float); a 24-bit FLAC keeps the six channels");
        dir.deleteRecursively();
    }

    // Splitting into mono files and putting them back.
    {
        std::vector<Snapshot> channels;
        std::vector<const AudioSnapshot*> parts;

        for (int ch = 0; ch < 6; ++ch)
        {
            channels.push_back (AudioEdits::extractChannel (*fiveOne, ch));
            parts.push_back (channels.back().get());
        }

        const auto back = AudioEdits::combineChannels (parts);
        auto eachOk = true;

        for (int ch = 0; ch < 6; ++ch)
            eachOk = eachOk && channels[(size_t) ch]->getNumChannels() == 1 && sameChannel (readAll (*channels[(size_t) ch]), 0, source, ch, frames);

        // Different lengths: as long as the longest, the others ending in silence.
        juce::AudioBuffer<float> shortOne (1, 1000);
        shortOne.clear();
        shortOne.setSample (0, 999, 0.5f);
        const auto brief = AudioEdits::fromBuffer (std::move (shortOne));
        const auto uneven = AudioEdits::combineChannels ({ brief.get(), channels[0].get() });
        const auto u = readAll (*uneven);
        auto tailSilent = true;

        for (int i = 1000; i < frames; ++i)
            tailSilent = tailSilent && u.getSample (0, i) == 0.0f;

        juce::Array<juce::File> shuffled;

        for (auto n : { "Mix Rs.wav", "Mix_LFE.wav", "Mix L.wav", "Mix-c.wav", "Mix.ls.wav", "Mix R.wav" })
            shuffled.add (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (n));

        juce::StringArray order, plain;

        for (auto& f : Surround::orderForCombining (shuffled))
            order.add (f.getFileName());

        shuffled.remove (0);

        for (auto& f : Surround::orderForCombining (shuffled))
            plain.add (f.getFileName());

        check (eachOk && back != nullptr && identical (*back, *fiveOne) && uneven->getNumChannels() == 2 && uneven->getNumFrames() == frames
                   && u.getSample (0, 999) == 0.5f && tailSilent
                   && order.joinIntoString (",") == "Mix L.wav,Mix R.wav,Mix-c.wav,Mix_LFE.wav,Mix.ls.wav,Mix Rs.wav"
                   && plain.joinIntoString (",") == "Mix L.wav,Mix R.wav,Mix-c.wav,Mix.ls.wav,Mix_LFE.wav",
               "surround: each channel saved alone is that channel, and the six put back together are the file bit for bit; files of "
               "different lengths run to the longest, the others ending in silence; six named ... L, R, C, LFE, Ls, Rs go in 5.1 "
               "order, whatever their order and case, and other sets in name order");
    }

    // Playing: folded down to the mix, or the channels past L and R to their own player channels.
    {
        AudioDocument doc;
        doc.reset (fiveOne, 48000.0, {}, 32);
        DocumentSource src (doc);
        src.prepareToPlay (512, 48000.0);

        juce::AudioBuffer<float> two (2, frames);
        src.setNextReadPosition (0);
        src.getNextAudioBlock ({ &two, 0, frames });   // longer than one read, so it is read in pieces

        juce::AudioBuffer<float> wide (36, frames);
        wide.setSample (3, 10, 1.0f);   // must be cleared
        src.setSeparateOutputs (4);
        src.setNextReadPosition (0);
        src.getNextAudioBlock ({ &wide, 0, frames });

        auto othersSilent = true;

        for (int ch : { 2, 3, 8, 9, 20, 35 })
            othersSilent = othersSilent && silent (wide, ch, frames);

        check (sameChannel (two, 0, folded, 0, frames) && sameChannel (two, 1, folded, 1, frames)
                   && sameChannel (wide, 0, source, 0, frames) && sameChannel (wide, 1, source, 1, frames)
                   && sameChannel (wide, 4, source, 2, frames) && sameChannel (wide, 5, source, 3, frames)
                   && sameChannel (wide, 6, source, 4, frames) && sameChannel (wide, 7, source, 5, frames) && othersSilent,
               "surround: a 5.1 file plays folded down on the mix, bit for bit, or as L and R on the mix and C, LFE, Ls and Rs on the "
               "next four player channels, the rest silent, in blocks longer than it reads at once (40,000 frames)");
    }

}

void runSurroundDeviceTests (AudioEngine& engine)
{
    const auto fiveOne = AudioEdits::fromBuffer (fiveOneBuffer (4800, 4));

    // The engine: a 5.1 file playing to separate outputs turns on and connects outputs 3 to 6.
    if (auto* device = engine.deviceManager.getCurrentAudioDevice(); device != nullptr && engine.getDeviceOutputChannelNames().size() >= 6
                                                                       && engine.getPlayer().getMode() == DocumentPlayerProcessor::Mode::document)
    {
        auto& doc = engine.document;
        const auto before = doc.getSnapshot();
        const auto rate = doc.getSampleRate();
        const auto file = doc.getFile();
        const auto depth = doc.getSourceBitDepth();
        const auto markers = doc.getMarkers();
        const auto info = doc.getInfo();

        engine.setOutputPair (0);
        doc.reset (fiveOne, 48000.0, {}, 32);
        engine.setSurroundOutputs (true);
        TestUtils::runUntil ([&] { return engine.playsOutOf (2) && engine.playsOutOf (5); }, 5000);
        const auto on = engine.deviceManager.getCurrentAudioDevice()->getActiveOutputChannels();
        const auto connected = engine.playsOutOf (2) && engine.playsOutOf (3) && engine.playsOutOf (4) && engine.playsOutOf (5)
                               && on[2] && on[5] && ! engine.playsOutOf (6) && ! engine.playsOutOf (1);

        doc.reset (AudioEdits::fromBuffer (juce::AudioBuffer<float> (2, 100)), 48000.0, {}, 24);
        TestUtils::runUntil ([&] { return ! engine.playsOutOf (2); }, 5000);
        const auto stereoLeftOut = ! engine.playsOutOf (2) && ! engine.playsOutOf (5);

        doc.reset (fiveOne, 48000.0, {}, 32);
        engine.setSurroundOutputs (false);
        TestUtils::runUntil ([&] { return ! engine.playsOutOf (2); }, 5000);
        check (connected && stereoLeftOut && ! engine.playsOutOf (2) && ! engine.playsOutOf (5),
               "surround: on the " + juce::String (engine.getDeviceOutputChannelNames().size()) + "-output interface, a 5.1 file playing to "
               "separate outputs turns on and connects outputs 3 to 6 (and no others); a stereo file, or folding down, disconnects them");

        doc.reset (before, rate, file, depth, markers, info);
    }
}
