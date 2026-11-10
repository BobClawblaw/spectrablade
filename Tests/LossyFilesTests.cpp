#include <JuceHeader.h>
#include "../Source/AudioFileIO.h"
#include "../Source/LossyFiles.h"
#include "../Source/AudioEdits.h"
#include "TestUtils.h"
#include "LossyFilesTests.h"

#include <map>

namespace
{
    using TestUtils::check;

    /** Two channels of sweeps, 100 Hz to 8 kHz up on the left and down on the right, at half scale. */
    juce::AudioBuffer<float> sweeps (int channels, double rate, double seconds)
    {
        const auto n = (int) (rate * seconds);
        juce::AudioBuffer<float> b (channels, n);

        for (int ch = 0; ch < channels; ++ch)
        {
            double phase = 0.0;

            for (int i = 0; i < n; ++i)
            {
                const auto t = (double) i / n;
                const auto f = 100.0 * std::pow (80.0, ch == 0 ? t : 1.0 - t);
                phase += juce::MathConstants<double>::twoPi * f / rate;
                b.setSample (ch, i, (float) (0.5 * std::sin (phase)));
            }
        }

        return b;
    }

    struct Decoded
    {
        juce::AudioBuffer<float> audio;
        double rate = 0.0;
    };

    Decoded decode (const juce::File& file, juce::AudioFormatManager& formats)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

        if (reader == nullptr)
            return {};

        Decoded d { juce::AudioBuffer<float> ((int) reader->numChannels, (int) reader->lengthInSamples), reader->sampleRate };
        reader->read (&d.audio, 0, d.audio.getNumSamples(), 0, true, true);
        return d;
    }

    /** The delay (0 to 3000 frames) at which the decoded left channel best matches the original. */
    int delayOf (const juce::AudioBuffer<float>& original, const juce::AudioBuffer<float>& decoded)
    {
        auto best = 0;
        auto bestSum = -1.0;

        for (int lag = 0; lag <= 3000; ++lag)
        {
            double sum = 0.0;

            for (int i = 4000; i < 24000 && i + lag < decoded.getNumSamples(); ++i)
                sum += (double) original.getSample (0, i) * decoded.getSample (0, i + lag);

            if (sum > bestSum)
                bestSum = sum, best = lag;
        }

        return best;
    }

    /** The signal-to-error ratio in dB of a channel once lined up, from 0.1 s to 0.1 s before the end. */
    double snr (const juce::AudioBuffer<float>& original, const juce::AudioBuffer<float>& decoded, int channel, int lag, double rate)
    {
        double signal = 0.0, error = 0.0;
        const auto margin = (int) (0.1 * rate);

        for (int i = margin; i < original.getNumSamples() - margin; ++i)
        {
            const auto o = (double) original.getSample (channel, i);
            const auto e = (double) decoded.getSample (channel, i + lag) - o;
            signal += o * o;
            error += e * e;
        }

        return 10.0 * std::log10 (signal / error);
    }

    double kbpsOf (const juce::File& file, double seconds)
    {
        return (double) file.getSize() * 8.0 / seconds / 1000.0;
    }
}

void runLossyFilesTests()
{
    using namespace LossyFiles;

    juce::AudioFormatManager formats;
    AudioFileIO::registerFormats (formats);

    juce::TemporaryFile folderTemp;
    const auto folder = folderTemp.getFile();
    folder.createDirectory();

    // Settings and sample rates.
    check (Settings {}.isValid() && Settings { Kind::aac, 256 }.isValid() && Settings { Kind::mp3, 0, true, 0 }.isValid()
               && ! Settings { Kind::mp3, 200 }.isValid() && ! Settings { Kind::aac, 0, true, 2 }.isValid()
               && ! Settings { Kind::mp3, 0, true, 10 }.isValid(),
           "lossy files: the settings offered are valid, others (a 200 kbps rate, variable AAC, quality 10) aren't");
    check (outputRate (44100.0) == 44100.0 && outputRate (48000.0) == 48000.0 && outputRate (22050.0) == 22050.0
               && outputRate (96000.0) == 48000.0 && outputRate (88200.0) == 44100.0 && outputRate (176400.0) == 44100.0
               && outputRate (192000.0) == 48000.0 && outputRate (37800.0) == 44100.0,
           "lossy files: the rates the formats hold are kept, higher ones come down to 44.1 or 48 kHz, others go up");
    check (isLossy (juce::File ("/a/b.mp3")) && isLossy (juce::File ("/a/b.M4A")) && ! isLossy (juce::File ("/a/b.wav"))
               && kindFor (juce::File ("/a/b.m4a")) == Kind::aac && kindFor (juce::File ("/a/b.mp3")) == Kind::mp3,
           "lossy files: .mp3 and .m4a are the types written");

    const auto rate = 44100.0;
    const auto seconds = 4.0;
    auto original = sweeps (2, rate, seconds);
    const auto snapshot = AudioEdits::fromBuffer (juce::AudioBuffer<float> (original));

    const auto hasBytes = [] (const juce::File& file, const char* text)
    {
        juce::MemoryBlock data;
        file.loadFileAsData (data);
        const auto* begin = (const char*) data.getData();
        const auto* end = begin + juce::jmin ((size_t) 4096, data.getSize());
        return std::search (begin, end, text, text + std::strlen (text)) != end;
    };

    /** The bytes of a .m4a file outside its audio (everything but the mdat box's contents). */
    const auto m4aOverhead = [] (const juce::File& file)
    {
        juce::MemoryBlock data;
        file.loadFileAsData (data);
        const auto* bytes = (const juce::uint8*) data.getData();

        for (size_t i = 0; i + 8 <= data.getSize();)
        {
            const auto size = (size_t) juce::ByteOrder::bigEndianInt (bytes + i);

            if (std::memcmp (bytes + i + 4, "mdat", 4) == 0)
                return (juce::int64) (data.getSize() - size + 8);

            if (size < 8)
                break;

            i += size;
        }

        return (juce::int64) data.getSize();
    };

    /** The level of a channel once lined up, as a gain fitted to the original. */
    const auto gainOf = [] (const juce::AudioBuffer<float>& o, const juce::AudioBuffer<float>& d, int lag)
    {
        double od = 0.0, oo = 0.0;

        for (int i = 4410; i < o.getNumSamples() - 4410; ++i)
        {
            od += (double) o.getSample (0, i) * d.getSample (0, i + lag);
            oo += (double) o.getSample (0, i) * o.getSample (0, i);
        }

        return od / oo;
    };

    // Noise at -12 dB peak, for the bit rates: the sweeps need far fewer bits.
    juce::AudioBuffer<float> noise (2, (int) (rate * seconds));
    juce::Random random (7);

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < noise.getNumSamples(); ++i)
            noise.setSample (ch, i, 0.25f * (random.nextFloat() * 2.0f - 1.0f));

    const auto noiseSnapshot = AudioEdits::fromBuffer (juce::AudioBuffer<float> (noise));

    // Each setting, with the least signal-to-error ratio it gives the sweeps (measured, less a little).
    // The AAC levels are the system encoder's. Apple's (macOS) gains with the rate; Windows' leaves the
    // error on these tones about 48 dB down from 192 kbps up (it spends the bits on noisier audio, its
    // band reaching 20 kHz at 320 kbps), so its own levels are held there. MP3 is read by the system's
    // decoder too, Windows' about 0.3 dB under the others at the highest rate.
   #if JUCE_WINDOWS
    constexpr double aac320 = 47.5, aac256 = 48.0, aacMono128 = 45.0, mp3At320 = 76.5;
   #else
    constexpr double aac320 = 57.5, aac256 = 50.0, aacMono128 = 56.0, mp3At320 = 77.0;
   #endif
    struct Case { const char* name; Settings settings; double snrDb; };
    const Case cases[] { { "MP3 320 kbps", { Kind::mp3, 320 }, mp3At320 }, { "MP3 192 kbps", { Kind::mp3, 192 }, 76.5 },
                         { "MP3 128 kbps", { Kind::mp3, 128 }, 63.0 }, { "MP3 V0", { Kind::mp3, 0, true, 0 }, 64.0 },
                         { "MP3 V2", { Kind::mp3, 0, true, 2 }, 37.5 }, { "MP3 V4", { Kind::mp3, 0, true, 4 }, 33.5 },
                         { "AAC 320 kbps", { Kind::aac, 320 }, aac320 }, { "AAC 256 kbps", { Kind::aac, 256 }, aac256 },
                         { "AAC 192 kbps", { Kind::aac, 192 }, 43.0 }, { "AAC 128 kbps", { Kind::aac, 128 }, 37.0 },
                         { "AAC 96 kbps", { Kind::aac, 96 }, 34.0 } };
    std::map<juce::String, double> noiseKbps;

    for (const auto& c : cases)
    {
        const auto ext = c.settings.kind == Kind::mp3 ? ".mp3" : ".m4a";
        const auto file = folder.getChildFile ("sweeps" + juce::String (ext));
        const auto noisy = folder.getChildFile ("noise" + juce::String (ext));
        const auto error = write (*snapshot, rate, file, c.settings);
        const auto noiseError = write (*noiseSnapshot, rate, noisy, c.settings);
        const auto d = decode (file, formats);

        if (d.audio.getNumSamples() == 0)
        {
            check (false, "lossy files: " + juce::String (c.name) + " is written and reads back ("
                              + (error.isNotEmpty() ? error : juce::String ("it couldn't be read")) + ")");
            continue;
        }

        const auto lag = delayOf (original, d.audio);
        const auto low = juce::jmin (snr (original, d.audio, 0, lag, rate), snr (original, d.audio, 1, lag, rate));
        const auto gain = gainOf (original, d.audio, lag);
        noiseKbps[c.name] = kbpsOf (noisy, seconds);

        check (error.isEmpty() && noiseError.isEmpty() && std::abs (d.rate - rate) < 0.5 && d.audio.getNumChannels() == 2
                   && d.audio.getNumSamples() == original.getNumSamples() && lag == 0,
               "lossy files: " + juce::String (c.name) + " reads back as the same length, from the same first sample (" + juce::String (lag)
                   + " frames late, " + juce::String (d.audio.getNumSamples() - original.getNumSamples()) + " frames over)");
        check (low >= c.snrDb && std::abs (gain - 1.0) < 0.002,
               "lossy files: " + juce::String (c.name) + " keeps the sweeps " + juce::String (low, 2) + " dB above the error, at "
                   + juce::String (gain, 5) + " of their level");

        if (c.settings.kind == Kind::mp3)
            check (hasBytes (file, "LAME") && hasBytes (file, c.settings.variable ? "Xing" : "Info"),
                   "lossy files: " + juce::String (c.name) + " starts with the frame that tells players the encoder's delay and padding");
        else
            check (m4aOverhead (file) <= 4096,
                   "lossy files: " + juce::String (c.name) + " holds " + juce::String (m4aOverhead (file)) + " bytes besides the audio");
    }

    // Read from part way in, as audition, previews and session clips read: the same samples as reading
    // the whole file gives, wherever a read starts and in whatever order (the last of each kind written).
    for (const auto* ext : { ".mp3", ".m4a" })
    {
        const auto file = folder.getChildFile ("sweeps" + juce::String (ext));
        const auto whole = decode (file, formats);
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        auto worst = 0.0f;
        juce::StringArray positions;

        for (const auto start : { 70000, 1000, 30000, 0, 30001, 95000, 2047 })
        {
            const auto count = juce::jmin (4096, whole.audio.getNumSamples() - start);

            if (reader == nullptr || count <= 0)
                continue;   // past the end of a shorter file

            juce::AudioBuffer<float> part ((int) reader->numChannels, count);
            reader->read (&part, 0, count, start, true, true);
            positions.add (juce::String (start));

            for (int ch = 0; ch < part.getNumChannels(); ++ch)
                for (int i = 0; i < count; ++i)
                    worst = juce::jmax (worst, std::abs (part.getSample (ch, i) - whole.audio.getSample (ch, start + i)));
        }

        check (reader != nullptr && positions.size() >= 5 && worst < 1.0e-4f,
               "lossy files: " + juce::String (ext).substring (1).toUpperCase() + " read from part way in (at " + positions.joinIntoString (", ")
                   + ", in that order) gives what reading it all does (at most " + juce::String (worst, 7) + " off)");
    }

    auto ratesOk = true;
    juce::StringArray rates;

    for (const auto& c : cases)
    {
        const auto kbps = noiseKbps[c.name];
        rates.add (juce::String (c.name) + " " + juce::String (kbps, 1));

        if (! c.settings.variable)
            ratesOk = ratesOk && kbps >= c.settings.kbps && kbps < c.settings.kbps * (c.settings.kind == Kind::mp3 ? 1.03 : 1.15);
    }

    ratesOk = ratesOk && noiseKbps["MP3 V0"] > noiseKbps["MP3 V2"] && noiseKbps["MP3 V2"] > noiseKbps["MP3 V4"];
    check (ratesOk, "lossy files: on noise, constant MP3 rates come within 3% over (the first frame and 4 s), AAC within 15%, "
                    "and V0 > V2 > V4: " + rates.joinIntoString (", "));

    // The same audio and settings: the same bytes.
    {
        const auto a = folder.getChildFile ("a.mp3"), b = folder.getChildFile ("b.mp3");
        const auto c = folder.getChildFile ("a.m4a"), e = folder.getChildFile ("b.m4a");
        write (*snapshot, rate, a, {});
        write (*snapshot, rate, b, {});
        write (*snapshot, rate, c, { Kind::aac, 256 });
        write (*snapshot, rate, e, { Kind::aac, 256 });
        // The .m4a header holds the time it was written, so for AAC compare the audio data.
        const auto mdat = [] (const juce::File& file)
        {
            juce::MemoryBlock data;
            file.loadFileAsData (data);
            const auto* bytes = (const char*) data.getData();
            const auto* end = bytes + data.getSize();
            const char* type = "mdat";
            const auto* at = std::search (bytes, end, type, type + 4);
            return at == end ? juce::MemoryBlock() : juce::MemoryBlock (at, (size_t) (end - at));
        };

        check (a.hasIdenticalContentTo (b) && mdat (c).getSize() > 1000 && mdat (c) == mdat (e),
               "lossy files: writing again gives the same bytes (MP3) and the same audio data (AAC)");
    }

    // 96 kHz comes down to 48, and mono stays mono.
    {
        const auto high = sweeps (2, 96000.0, 2.0);
        const auto highSnap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (high));
        const auto mono = sweeps (1, rate, 2.0);
        const auto monoSnap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (mono));

        for (auto kind : { Kind::mp3, Kind::aac })
        {
            const auto ext = kind == Kind::mp3 ? ".mp3" : ".m4a";
            const auto name = kind == Kind::mp3 ? juce::String ("MP3") : juce::String ("AAC");
            const auto f96 = folder.getChildFile ("high" + juce::String (ext));
            const auto fm = folder.getChildFile ("mono" + juce::String (ext));
            const auto e1 = write (*highSnap, 96000.0, f96, { kind, 256 });
            const auto e2 = write (*monoSnap, rate, fm, { kind, 128 });
            const auto d96 = decode (f96, formats);
            const auto dm = decode (fm, formats);
            const auto lag = delayOf (mono, dm.audio);
            const auto ratio = snr (mono, dm.audio, 0, lag, rate);

            check (e1.isEmpty() && std::abs (d96.rate - 48000.0) < 0.5 && d96.audio.getNumSamples() == 96000,
                   "lossy files: " + name + " from 96 kHz is written at 48 kHz, 2 s still 96,000 frames (" + juce::String (d96.audio.getNumSamples()) + ")");
            check (e2.isEmpty() && dm.audio.getNumChannels() == 1 && dm.audio.getNumSamples() == mono.getNumSamples() && lag == 0
                       && ratio > (kind == Kind::mp3 ? 63.5 : aacMono128),
                   "lossy files: " + name + " in mono stays one channel of the same length, " + juce::String (ratio, 2) + " dB above the error");
        }
    }

    // What is refused, and stopping part way.
    {
        const auto keep = folder.getChildFile ("keep.mp3");
        keep.replaceWithText ("old");
        const auto three = AudioEdits::fromBuffer (juce::AudioBuffer<float> (3, 1000));
        const auto e1 = write (*three, rate, keep, {});
        const auto e2 = write (*snapshot, rate, keep, { Kind::mp3, 200 });
        const auto e3 = write (*snapshot, 4000.0, keep, {});
        auto calls = 0;
        const auto e4 = write (*snapshot, rate, keep, {}, [&calls] (float) { return ++calls < 3; });
        calls = 0;
        const auto e5 = write (*snapshot, rate, keep, { Kind::aac, 128 }, [&calls] (float) { return ++calls < 3; });

        check (e1.contains ("one or two channels") && e2.isNotEmpty() && e3.contains ("too low") && e4 == "Stopped." && e5 == "Stopped."
                   && keep.loadFileAsString() == "old" && folder.getNumberOfChildFiles (juce::File::findFiles, "*keep*") == 1,
               "lossy files: three channels, unoffered settings and 4 kHz are refused, stopping part way leaves the old file, "
               "and no temporary file is left");

        juce::Array<float> seen;
        const auto e6 = write (*snapshot, rate, keep, {}, [&seen] (float f) { seen.add (f); return true; });
        check (e6.isEmpty() && seen.size() == 11 && seen.getFirst() == 0.0f && seen.getLast() < 1.0f && keep.getSize() > 1000,
               "lossy files: progress is reported each block, from 0, and the finished file replaces the old one");
    }

    folder.deleteRecursively();
}
