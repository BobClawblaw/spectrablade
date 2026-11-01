// Editing, undo, file round trips and offline rendering, checked sample by sample.

// Samples are compared bit for bit on purpose.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioEdits.h"
#include "../Source/AudioEngine.h"
#include "../Source/AudioFileIO.h"
#include "../Source/OfflineRender.h"
#include "TestUtils.h"
#include "DocumentTests.h"

namespace
{
    using TestUtils::check;
    using AudioEdits::Snapshot;

    constexpr double rate = 48000.0;

    /** 3 s of deterministic stereo test signal: different tones per channel plus a little noise. */
    juce::AudioBuffer<float> makeSignal (int frames = 3 * 48000)
    {
        juce::AudioBuffer<float> b (2, frames);
        juce::Random random (1234);

        for (int i = 0; i < frames; ++i)
        {
            const auto t = (double) i / rate;
            b.setSample (0, i, (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * t)) + random.nextFloat() * 0.01f);
            b.setSample (1, i, (float) (0.25 * std::sin (2.0 * juce::MathConstants<double>::pi * 660.0 * t)) - random.nextFloat() * 0.01f);
        }

        return b;
    }

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    /** Largest absolute difference; infinity if the shapes differ. */
    float maxDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
            return std::numeric_limits<float>::infinity();

        float worst = 0.0f;

        for (int ch = 0; ch < a.getNumChannels(); ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
                worst = juce::jmax (worst, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));

        return worst;
    }

    bool identical (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        return maxDifference (a, b) == 0.0f;
    }

    /** Frames [start, end) of a buffer. */
    juce::AudioBuffer<float> range (const juce::AudioBuffer<float>& b, int start, int end)
    {
        juce::AudioBuffer<float> out (b.getNumChannels(), end - start);

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            out.copyFrom (ch, 0, b, ch, start, end - start);

        return out;
    }

    juce::AudioBuffer<float> concat (std::initializer_list<juce::AudioBuffer<float>> parts)
    {
        int total = 0;

        for (auto& p : parts)
            total += p.getNumSamples();

        juce::AudioBuffer<float> out (parts.begin()->getNumChannels(), total);
        int pos = 0;

        for (auto& p : parts)
        {
            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                out.copyFrom (ch, pos, p, ch, 0, p.getNumSamples());

            pos += p.getNumSamples();
        }

        return out;
    }

    //==============================================================================
    void testEditsAndUndo()
    {
        const auto signal = makeSignal();
        const auto n = signal.getNumSamples();
        AudioDocument doc;
        doc.reset (AudioEdits::fromBuffer (juce::AudioBuffer<float> (signal)), rate, {}, 24);
        const auto a = n / 3, b = 2 * n / 3;

        // Cut the middle third, then paste it back: bit-identical to the original.
        auto clip = AudioEdits::copy (*doc.getSnapshot(), a, b, rate);
        doc.apply (AudioEdits::remove (*doc.getSnapshot(), a, b), "Cut");
        check (identical (readAll (*doc.getSnapshot()), concat ({ range (signal, 0, a), range (signal, b, n) })), "cut removes exactly the range");
        check (doc.isDirty(), "an edit makes the document dirty");

        doc.apply (AudioEdits::replaceWithClip (*doc.getSnapshot(), a, a, clip), "Paste");
        check (identical (readAll (*doc.getSnapshot()), signal), "paste puts it back bit-identically");

        doc.undo();
        doc.undo();
        check (identical (readAll (*doc.getSnapshot()), signal) && ! doc.isDirty(), "undo returns to the original and clean state");
        doc.redo();
        check (doc.getNumFrames() == n - (b - a) && doc.getUndoDescription() == "Cut", "redo reapplies the cut");

        // A new edit after undo discards the redo branch.
        doc.undo();
        doc.apply (AudioEdits::silence (*doc.getSnapshot(), 0, 10), "Silence");
        check (! doc.canRedo(), "a new edit clears redo");

        // Several edits leave many segments; a crop across them must still read the right frames.
        doc.reset (AudioEdits::fromBuffer (juce::AudioBuffer<float> (signal)), rate, {}, 24);
        auto expected = signal;

        for (int k = 0; k < 6; ++k)
        {
            const auto s = 1000 + k * 7919, e = s + 3001;
            doc.apply (AudioEdits::invert (*doc.getSnapshot(), s, e), "Invert");

            for (int ch = 0; ch < 2; ++ch)
                expected.applyGain (ch, s, e - s, -1.0f);
        }

        doc.apply (AudioEdits::crop (*doc.getSnapshot(), 500, 60000), "Crop");
        check (identical (readAll (*doc.getSnapshot()), range (expected, 500, 60000)), "crop across many segments is exact");

        // Peak tables agree with a brute-force scan, across segment and chunk boundaries.
        const auto snap = doc.getSnapshot();
        const auto all = readAll (*snap);
        juce::Random random (99);
        bool peaksOk = true;

        for (int t = 0; t < 200 && peaksOk; ++t)
        {
            const auto s = random.nextInt (all.getNumSamples() - 2);
            const auto e = s + 1 + random.nextInt (juce::jmin (20000, all.getNumSamples() - s - 1));

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto brute = juce::FloatVectorOperations::findMinAndMax (all.getReadPointer (ch, s), e - s);
                peaksOk = peaksOk && snap->getPeak (ch, s, e) == brute;
            }
        }

        check (peaksOk, "peak tables match a brute-force min/max (200 random ranges)");
    }

    void testEffects()
    {
        const auto signal = makeSignal();
        const auto n = signal.getNumSamples();
        const auto original = AudioEdits::fromBuffer (juce::AudioBuffer<float> (signal));

        auto doubled = readAll (*AudioEdits::gain (*original, 0, n, juce::Decibels::gainToDecibels (2.0f)));
        auto twice = signal;
        twice.applyGain (2.0f);
        check (maxDifference (doubled, twice) < 1.0e-6f, "gain +6.02 dB doubles every sample");

        auto normalized = AudioEdits::normalize (*original, 0, n, -6.0f);
        check (std::abs (AudioEdits::peakLevel (*normalized, 0, n) - juce::Decibels::decibelsToGain (-6.0f)) < 1.0e-6f,
               "normalize sets the peak to -6 dBFS");
        check (AudioEdits::normalize (*AudioEdits::silence (*original, 0, n), 0, n, -1.0f) == nullptr, "normalize refuses silence");

        auto reversedTwice = AudioEdits::reverse (*AudioEdits::reverse (*original, 100, 5000), 100, 5000);
        check (identical (readAll (*reversedTwice), signal), "reverse twice is the identity");

        // DC offset: +0.25 on a square wave of +-0.5, and a constant -0.1, each centred on 0 exactly;
        // outside the range nothing changes.
        juce::AudioBuffer<float> offset (2, 1000);

        for (int i = 0; i < 1000; ++i)
        {
            offset.setSample (0, i, (i % 2 == 0 ? 0.5f : -0.5f) + 0.25f);
            offset.setSample (1, i, -0.1f);
        }

        const auto centred = readAll (*AudioEdits::removeDcOffset (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (offset)), 100, 900));
        auto dcExact = true;

        for (int i = 0; i < 1000; ++i)
        {
            const auto inside = i >= 100 && i < 900;
            dcExact = dcExact && juce::exactlyEqual (centred.getSample (0, i), inside ? (i % 2 == 0 ? 0.5f : -0.5f) : offset.getSample (0, i))
                      && juce::exactlyEqual (centred.getSample (1, i), inside ? 0.0f : -0.1f);
        }

        check (dcExact, "remove DC offset: each channel's average over the range taken out, exactly; nothing outside it");

        // Swapping: left and right exchanged inside the range only; a mono file is left as it is.
        const auto swapped = readAll (*AudioEdits::swapChannels (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (offset)), 100, 900));
        auto swapExact = true;

        for (int i = 0; i < 1000; ++i)
        {
            const auto inside = i >= 100 && i < 900;
            swapExact = swapExact && juce::exactlyEqual (swapped.getSample (0, i), offset.getSample (inside ? 1 : 0, i))
                        && juce::exactlyEqual (swapped.getSample (1, i), offset.getSample (inside ? 0 : 1, i));
        }

        check (swapExact, "swap channels: left and right exchanged inside the range, unchanged outside it");

        juce::AudioBuffer<float> monoIn (1, 1000);
        monoIn.copyFrom (0, 0, offset, 0, 0, 1000);
        const auto monoOut = readAll (*AudioEdits::swapChannels (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (monoIn)), 0, 1000));
        check (monoOut.getNumChannels() == 1 && identical (monoOut, monoIn), "swap channels: a mono file comes back unchanged");

        // Inserting silence: 500 zero frames at 300, everything after moved on exactly.
        const auto padded = readAll (*AudioEdits::insertSilence (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (offset)), 300, 500));
        auto padExact = padded.getNumSamples() == 1500 && padded.getNumChannels() == 2;

        for (int ch = 0; padExact && ch < 2; ++ch)
            for (int i = 0; i < 1500; ++i)
                padExact = padExact && juce::exactlyEqual (padded.getSample (ch, i), i < 300 ? offset.getSample (ch, i)
                                                                                     : i < 800 ? 0.0f
                                                                                               : offset.getSample (ch, i - 500));

        check (padExact, "insert silence: the length of zeros at the frame, what was after it moved on exactly");

        // The audible span: zeros and a quiet 0.0005 hiss around frames 100k to 200k, found exactly
        // across block boundaries; on the second channel only at the end; nothing in pure silence.
        juce::AudioBuffer<float> padding (2, 300000);
        padding.clear();

        for (int i = 0; i < 300000; ++i)
            padding.setSample (0, i, i % 2 == 0 ? 0.0005f : -0.0005f);

        padding.setSample (0, 100000, 0.5f);
        padding.setSample (1, 200000, -0.5f);
        const auto threshold = juce::Decibels::decibelsToGain (-60.0f);
        const auto span = AudioEdits::audibleRange (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (padding)), threshold);
        check (span && span->getStart() == 100000 && span->getEnd() == 200001,
               "audible range: the first and last frame above the level on any channel, across blocks");

        juce::AudioBuffer<float> hush (2, 70000);
        hush.clear();
        check (! AudioEdits::audibleRange (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (hush)), threshold),
               "audible range: nothing in pure silence");

        // Channel conversion over more than one block: left 0.5 and right 0.25 average to 0.375,
        // and mono copied to two channels is the same on both.
        juce::AudioBuffer<float> pair (2, 70000);

        for (int i = 0; i < 70000; ++i)
        {
            pair.setSample (0, i, 0.5f);
            pair.setSample (1, i, 0.25f);
        }

        const auto mixed = AudioEdits::convertChannels (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (pair)), 1);
        const auto mixedOut = readAll (*mixed);
        auto mixExact = mixed->getNumChannels() == 1 && mixed->getNumFrames() == 70000;

        for (int i = 0; mixExact && i < 70000; ++i)
            mixExact = juce::exactlyEqual (mixedOut.getSample (0, i), 0.375f);

        check (mixExact, "mix to mono: every frame the channels' average, over more than one block");

        const auto spread = readAll (*AudioEdits::convertChannels (*mixed, 2));
        auto spreadExact = spread.getNumChannels() == 2 && spread.getNumSamples() == 70000;

        for (int i = 0; spreadExact && i < 70000; ++i)
            spreadExact = juce::exactlyEqual (spread.getSample (0, i), 0.375f) && juce::exactlyEqual (spread.getSample (1, i), 0.375f);

        check (spreadExact, "make stereo: the one channel on both");

        AudioDocument converted;
        converted.reset (AudioEdits::fromBuffer (juce::AudioBuffer<float> (pair)), rate, {}, 24);
        converted.apply (AudioEdits::convertChannels (*converted.getSnapshot(), 1), "Mix to Mono");
        const auto monoAfter = converted.getNumChannels() == 1;
        converted.undo();
        check (monoAfter && converted.getNumChannels() == 2 && identical (readAll (*converted.getSnapshot()), pair),
               "mix to mono in a document: one channel, and undo brings both back bit-identically");

        auto faded = readAll (*AudioEdits::fadeIn (*original, 1000, 2000));
        check (faded.getSample (0, 1000) == 0.0f && faded.getSample (0, 1999) == signal.getSample (0, 1999)
                   && faded.getSample (0, 999) == signal.getSample (0, 999),
               "fade in starts at zero, ends at full level and leaves the rest alone");

        auto silenced = readAll (*AudioEdits::silence (*original, 10, 20));
        check (silenced.getSample (1, 9) == signal.getSample (1, 9) && silenced.getSample (1, 10) == 0.0f
                   && silenced.getSample (1, 19) == 0.0f && silenced.getSample (1, 20) == signal.getSample (1, 20),
               "silence touches exactly its range");

        // A mono clip pasted into stereo lands on both channels.
        juce::AudioBuffer<float> mono (1, 100);
        for (int i = 0; i < 100; ++i)
            mono.setSample (0, i, (float) i / 100.0f);

        AudioClip monoClip { 1, rate, AudioEdits::fromBuffer (juce::AudioBuffer<float> (mono))->getSegments() };
        auto pasted = readAll (*AudioEdits::replaceWithClip (*original, 0, 0, monoClip));
        check (pasted.getNumSamples() == n + 100 && pasted.getSample (0, 50) == 0.5f && pasted.getSample (1, 50) == 0.5f
                   && pasted.getSample (0, 100) == signal.getSample (0, 0),
               "mono paste into stereo goes to both channels");

        auto down = AudioEdits::matchChannels (signal, 1);
        check (std::abs (down.getSample (0, 7) - 0.5f * (signal.getSample (0, 7) + signal.getSample (1, 7))) < 1.0e-7f,
               "stereo to mono averages the channels");
    }

    void testFiles (AudioEngine& engine)
    {
        const auto signal = makeSignal();
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (signal));
        const juce::TemporaryFile wavFloat (".wav"), wav24 (".wav"), wav16 (".wav"), flac (".flac");

        struct Case { const juce::TemporaryFile* file; int bits; float tolerance; };

        // Float is exact. Integer formats allow 1 LSB of dither, half an LSB of rounding, and up to
        // |sample| LSB because integers are written scaled by 2^(n-1) - 1 but read back as / 2^(n-1).
        const auto peak = AudioEdits::peakLevel (*snap, 0, snap->getNumFrames());
        auto lsbs = [peak] (int bits) { return (1.5f + peak) / (float) (1 << (bits - 1)); };

        for (auto c : { Case { &wavFloat, 32, 0.0f },
                        Case { &wav24, 24, lsbs (24) },
                        Case { &wav16, 16, lsbs (16) },
                        Case { &flac, 24, lsbs (24) } })
        {
            const auto& file = c.file->getFile();
            const auto error = AudioFileIO::save (*snap, rate, file, c.bits, engine.formatManager);
            const auto loaded = AudioFileIO::load (file, engine.formatManager);
            const auto diff = loaded.snapshot != nullptr ? maxDifference (readAll (*loaded.snapshot), signal)
                                                         : std::numeric_limits<float>::infinity();

            check (error.isEmpty() && loaded.error.isEmpty() && loaded.sampleRate == rate && loaded.bitDepth == c.bits
                       && diff <= c.tolerance,
                   file.getFileExtension() + " " + juce::String (c.bits) + "-bit round trip (max error "
                       + juce::String (diff * (c.bits == 32 ? 1.0f : (float) (1 << (c.bits - 1))), 2)
                       + (c.bits == 32 ? "" : " LSB") + ")");
        }

        check (AudioFileIO::save (*snap, rate, wav16.getFile().withFileExtension ("xyz"), 16, engine.formatManager).isNotEmpty(),
               "saving to an unknown extension reports an error");
    }

    void testPlaybackSource()
    {
        AudioDocument doc;
        juce::AudioBuffer<float> mono (1, 1000);

        for (int i = 0; i < 1000; ++i)
            mono.setSample (0, i, (float) (i + 1));

        doc.reset (AudioEdits::fromBuffer (std::move (mono)), rate, {}, 24);
        DocumentSource source (doc);
        source.setNextReadPosition (100);
        source.setStopFrame (150);

        juce::AudioBuffer<float> out (2, 64);
        source.getNextAudioBlock (juce::AudioSourceChannelInfo (&out, 0, 64));

        check (out.getSample (0, 0) == 101.0f && out.getSample (1, 0) == 101.0f && out.getSample (0, 49) == 150.0f
                   && out.getSample (0, 50) == 0.0f && out.getSample (1, 63) == 0.0f && source.getNextReadPosition() == 164,
               "playback reads from the cursor, doubles mono and stops at the range end");

        // An edit while playing is picked up on the next block.
        doc.apply (AudioEdits::silence (*doc.getSnapshot(), 0, 1000), "Silence");
        source.setNextReadPosition (0);
        source.setStopFrame (-1);
        source.getNextAudioBlock (juce::AudioSourceChannelInfo (&out, 0, 64));
        check (out.getMagnitude (0, 64) == 0.0f, "playback follows edits");
    }

    void testOfflineRender (AudioEngine& engine)
    {
        const auto signal = makeSignal (48000);
        juce::XmlElement emptyRack ("SPECTRABLADE_RACK");
        auto audio = signal;
        const auto error = OfflineRender::processThroughRack (emptyRack, engine.pluginFormatManager, rate, audio, {});
        check (error.isEmpty() && identical (audio, signal), "rendering through an empty rack changes nothing");

        // Apple's AUDelay, created and run off the message thread as in the app.
        juce::AudioPluginFormat* au = nullptr;

        for (auto* f : engine.pluginFormatManager.getFormats())
            if (f->getName() == "AudioUnit")
                au = f;

        juce::OwnedArray<juce::PluginDescription> types;

        if (au != nullptr)
            au->findAllTypesForFile (types, "AudioUnit:Effects/aufx,dely,appl");

        if (types.isEmpty())
        {
            check (false, "Apple AUDelay is available for the render test");
            return;
        }

        juce::XmlElement rack ("SPECTRABLADE_RACK");
        auto* slot = rack.createNewChildElement ("SLOT");
        slot->addChildElement (types[0]->createXml().release());

        auto* bypassed = rack.createNewChildElement ("SLOT");
        bypassed->setAttribute ("bypassed", true);
        bypassed->addChildElement (types[0]->createXml().release());

        audio = signal;
        std::atomic<bool> done { false };
        juce::String renderError;
        float lastProgress = 0.0f;

        std::thread worker ([&]
        {
            renderError = OfflineRender::processThroughRack (rack, engine.pluginFormatManager, rate, audio,
                                                             [&] (float p) { lastProgress = p; return true; });
            done = true;
        });

        TestUtils::runUntil ([&] { return done.load(); }, 60000);
        worker.join();

        bool finite = true;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            for (int i = 0; i < audio.getNumSamples(); ++i)
                finite = finite && std::isfinite (audio.getSample (ch, i));

        check (renderError.isEmpty() && audio.getNumSamples() == signal.getNumSamples() && audio.getNumChannels() == 2
                   && finite && maxDifference (audio, signal) > 0.01f && lastProgress == 1.0f,
               "rendering through AUDelay (bypassed slot skipped) keeps length and changes the sound"
                   + (renderError.isEmpty() ? juce::String() : ": " + renderError));

        // Let the instances be destroyed on the message thread.
        TestUtils::runUntil ([] { return false; }, 200);
    }
}

void runDocumentTests (AudioEngine& engine)
{
    testEditsAndUndo();
    testEffects();
    testFiles (engine);
    testPlaybackSource();
    testOfflineRender (engine);
}
