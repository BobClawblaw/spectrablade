#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/AuditionPreview.h"
#include "TestUtils.h"
#include "AuditionTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;
    constexpr int blockSize = 256;
    constexpr int fileFrames = 9600;   // 0.2 s

    /** A stereo 32-bit float WAV of noise, different on each side. */
    juce::File writeNoise (const juce::File& file, juce::AudioBuffer<float>& written)
    {
        written.setSize (2, fileFrames);
        juce::Random random (17);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < fileFrames; ++i)
                written.setSample (ch, i, (random.nextFloat() - 0.5f) * (ch == 0 ? 0.8f : 0.4f));

        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (32)
                                                                                    .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer != nullptr)
            writer->writeFromAudioSampleBuffer (written, 0, fileFrames);

        return file;
    }

    void testPlayer (juce::AudioFormatManager& formats, const juce::File& file, const juce::AudioBuffer<float>& written)
    {
        AudioDocument document;
        Session session;
        TrackEffects inserts;
        DocumentPlayerProcessor player { document, session, inserts };
        player.prepareToPlay (rate, blockSize);
        player.audition (std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (file)));
        const auto started = player.isAuditioning();

        std::array<std::vector<float>, 4> sent;
        const auto blocks = fileFrames / blockSize + 8;

        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> out (4, blockSize);
            out.clear();
            juce::MidiBuffer midi;
            player.processBlock (out, midi);

            for (int ch = 0; ch < 4; ++ch)
                sent[(size_t) ch].insert (sent[(size_t) ch].end(), out.getReadPointer (ch), out.getReadPointer (ch) + blockSize);
        }

        auto heardError = 0.0f, mainPeak = 0.0f, afterPeak = 0.0f;

        for (size_t i = 0; i < sent[0].size(); ++i)
        {
            mainPeak = juce::jmax (mainPeak, std::abs (sent[0][i]), std::abs (sent[1][i]));

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto heard = sent[(size_t) ch + 2][i];

                if ((int) i < fileFrames)
                    heardError = juce::jmax (heardError, std::abs (heard - written.getSample (ch, (int) i)));
                else
                    afterPeak = juce::jmax (afterPeak, std::abs (heard));
            }
        }

        check (started && ! player.isAuditioning() && heardError < 1.0e-6f && mainPeak == 0.0f && afterPeak == 0.0f,
               "audition: a file plays once from its start on the second output (left and right apart, within "
                   + juce::String (heardError, 9) + "), not through the main output (the effects rack), then stops by itself");

        // Stopping part way: silence from the next block on.
        player.audition (std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (file)));
        juce::AudioBuffer<float> out (4, blockSize);
        juce::MidiBuffer midi;
        player.processBlock (out, midi);
        const auto playedFirst = out.getMagnitude (2, 0, blockSize) > 0.0f;
        player.audition (nullptr);
        out.clear();
        player.processBlock (out, midi);
        check (playedFirst && ! player.isAuditioning() && out.getMagnitude (2, 0, blockSize) == 0.0f && out.getMagnitude (3, 0, blockSize) == 0.0f,
               "audition: stopping it silences the next block");
        player.releaseResources();
    }

    void testEngine (AudioEngine& engine, const juce::File& file, const juce::File& text)
    {
        const auto refused = ! engine.auditionFile (text) && ! engine.getPlayer().isAuditioning();
        const auto played = engine.auditionFile (file) && engine.getPlayer().isAuditioning();
        const auto stopped = ! engine.auditionFile ({}) && ! engine.getPlayer().isAuditioning();
        check (refused && played && stopped, "audition: the engine plays a readable file, refuses one that isn't audio, and an empty file stops it");
    }

    void testPreview (juce::AudioFormatManager& formats, const juce::File& file, const juce::File& text)
    {
        juce::Array<juce::File> calls;
        bool playing = false;
        AuditionPreview::Player fake { [&] (const juce::File& f) { calls.add (f); playing = f != juce::File(); return playing; },
                                       [&] { return playing; } };
        std::optional<bool> remembered;

        AuditionPreview preview (formats, fake, false);
        preview.onPreviewChanged = [&] (bool on) { remembered = on; };

        const auto description = AuditionPreview::describe (formats, file);
        check (description == "WAV, 48.0 kHz, stereo, 32-bit float, 0:00.2" && AuditionPreview::describe (formats, text).isEmpty(),
               "open panel preview: a file is described by format, rate, channels, depth and length (" + description + "); a text file isn't");

        preview.selectedFileChanged (file);
        const auto quietWhenOff = ! calls.contains (file) && ! playing && preview.getDescription() == description;

        preview.setPreviewOn (true);
        const auto playsWhenTicked = calls.getLast() == file && playing && remembered == true;

        calls.clear();
        preview.selectedFileChanged (text);
        const auto stopsOnOther = calls.size() == 1 && calls[0] == juce::File() && ! playing && preview.getDescription().isEmpty();

        preview.selectedFileChanged (file);
        const auto playsNext = calls.getLast() == file && playing;

        preview.setPreviewOn (false);
        const auto stopsWhenUnticked = ! playing && remembered == false;

        preview.playOrStop();
        const auto buttonPlays = playing && calls.getLast() == file;
        preview.playOrStop();
        const auto buttonStops = ! playing;

        check (quietWhenOff && playsWhenTicked && stopsOnOther && playsNext && stopsWhenUnticked && buttonPlays && buttonStops,
               "open panel preview: unticked it plays nothing; ticked it plays each audio file as it is selected and stops on one that "
               "isn't; unticking stops it and is remembered; Play and Stop work either way");
    }
}

void runAuditionTests (AudioEngine& engine)
{
    const auto dir = juce::File::createTempFile ("audition");
    dir.createDirectory();
    juce::AudioBuffer<float> written;
    const auto file = writeNoise (dir.getChildFile ("noise.wav"), written);
    const auto text = dir.getChildFile ("notes.txt");
    text.replaceWithText ("not audio");

    testPlayer (engine.formatManager, file, written);
    testEngine (engine, file, text);
    testPreview (engine.formatManager, file, text);
    dir.deleteRecursively();
}
