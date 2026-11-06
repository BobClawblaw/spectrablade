#include <JuceHeader.h>
#include "../Source/Ffmpeg.h"
#include "../Source/AudioFileIO.h"
#include "../Source/AudioEdits.h"
#include "TestUtils.h"
#include "FfmpegTests.h"

#include <map>

void runFfmpegTests()
{
    using TestUtils::check;
    const auto ffmpeg = Ffmpeg::find();

    if (ffmpeg == juce::File())
    {
        std::cout << "NOTE  ffmpeg tests skipped: no ffmpeg installed" << std::endl;
        return;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    juce::TemporaryFile folderTemp;
    const auto folder = folderTemp.getFile();
    folder.createDirectory();

    const auto rate = 44100.0;
    const int n = 88200;
    juce::AudioBuffer<float> original (2, n);

    for (int i = 0; i < n; ++i)
    {
        original.setSample (0, i, (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / rate)));
        original.setSample (1, i, (float) (0.25 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / rate)));
    }

    const auto wav = folder.getChildFile ("source.wav");
    AudioFileIO::save (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (original)), rate, wav, 32, formats);

    check (Ffmpeg::targets().size() == 4, "ffmpeg: four formats are offered for export (Opus, Apple Lossless, WavPack, AC-3)");

    // Each format written and read back. maxError is against the original, lined up; extra is the
    // frames the format adds at the end (AC-3 fills its last 1,536-frame block).
    struct Expect { double rate; int frames; double maxError; };
    const std::map<juce::String, Expect> expected { { "Opus", { 48000.0, 96000, -1.0 } }, { "Apple Lossless", { rate, n, 2.0e-7 } },
                                                    { "WavPack", { rate, n, 0.0 } }, { "AC-3", { rate, 89088, 1.0e-3 } } };

    for (const auto& t : Ffmpeg::targets())
    {
        const auto out = folder.getChildFile ("out." + t.extension);
        const auto error = Ffmpeg::encode (ffmpeg, wav, out, t, t.defaultBitRate);
        const auto loaded = AudioFileIO::load (out, formats);
        const auto& e = expected.at (t.name);
        auto worst = 0.0, level = 0.0;
        auto frames = 0;

        if (loaded.snapshot != nullptr)
        {
            frames = (int) loaded.snapshot->getNumFrames();
            juce::AudioBuffer<float> back (loaded.snapshot->getNumChannels(), frames);
            loaded.snapshot->read (back, 0, 0, frames);
            level = back.getMagnitude (0, frames / 4, frames / 2);

            if (e.maxError >= 0.0)
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i)
                        worst = juce::jmax (worst, (double) std::abs (back.getSample (ch, i) - original.getSample (ch, i)));
        }

        check (error.isEmpty() && loaded.error.isEmpty() && loaded.snapshot != nullptr && loaded.snapshot->getNumChannels() == 2
                   && std::abs (loaded.sampleRate - e.rate) < 0.5 && frames == e.frames && (e.maxError < 0.0 || worst <= e.maxError) && std::abs (level - 0.5) < 0.005,
               "ffmpeg: " + t.name + " is written and opens again: " + juce::String (loaded.sampleRate, 0) + " Hz, " + juce::String (frames)
                   + " frames, at most " + juce::String (worst, 9) + " off, peak " + juce::String (level, 4));
    }

    // Stopping, and a file ffmpeg can't read.
    {
        const auto out = folder.getChildFile ("stopped.opus");
        const auto stopped = Ffmpeg::encode (ffmpeg, wav, out, Ffmpeg::targets()[0], 128, [] { return false; });
        const auto junk = folder.getChildFile ("junk.opus");
        junk.replaceWithText ("not audio");
        const auto bad = AudioFileIO::load (junk, formats);
        check (stopped == "Stopped." && ! out.exists() && folder.getNumberOfChildFiles (juce::File::findFiles, "stopped*") == 0
                   && bad.snapshot == nullptr && bad.error.startsWith ("ffmpeg failed"),
               "ffmpeg: stopping leaves no file, and an unreadable file gives ffmpeg's error: " + bad.error);
    }

    folder.deleteRecursively();
}
