#include <JuceHeader.h>
#include "../Source/ExtremeStretch.h"
#include "../Source/AudioEdits.h"
#include "TestUtils.h"
#include "ExtremeStretchTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    double rmsDb (const juce::AudioBuffer<float>& b, int channel, int start, int count)
    {
        double sum = 0.0;

        for (int i = start; i < start + count; ++i)
            sum += (double) b.getSample (channel, i) * b.getSample (channel, i);

        return juce::Decibels::gainToDecibels (std::sqrt (sum / juce::jmax (1, count)), -200.0);
    }

    /** The share of the power within `halfWidthHz` of `hz` in part of a channel, in dB against
        all of it, read in windows of 4096 samples (so a tone whose phase jumps from window to
        window of a stretch still counts as the tone). */
    double bandShareDb (const juce::AudioBuffer<float>& b, int channel, int start, int count, double hz, double halfWidthHz = 30.0)
    {
        constexpr int n = 4096;
        juce::dsp::FFT fft (12);
        std::vector<float> frame ((size_t) n * 2);
        double band = 0.0, all = 0.0;
        const auto binHz = rate / n;

        for (int at = start; at + n <= start + count; at += n)
        {
            std::fill (frame.begin(), frame.end(), 0.0f);

            for (int i = 0; i < n; ++i)
                frame[(size_t) i] = b.getSample (channel, at + i) * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / n));

            fft.performFrequencyOnlyForwardTransform (frame.data(), true);

            for (int k = 1; k < n / 2; ++k)
            {
                const auto power = (double) frame[(size_t) k] * frame[(size_t) k];
                all += power;

                if (std::abs (k * binHz - hz) <= halfWidthHz)
                    band += power;
            }
        }

        return juce::Decibels::gainToDecibels (std::sqrt (band / juce::jmax (1.0e-30, all)), -200.0);
    }

    juce::String r (double v, int decimals = 2)   { return juce::String (v, decimals); }
}

void runExtremeStretchTests()
{
    std::cout << "Extreme stretch:" << std::endl;

    // A second of a 1 kHz tone at -12 dB on the left, with a 3 kHz tone at -24 dB beside it; the
    // right channel the same. Stretched eight times: eight seconds, at the same level, the two
    // tones at the same levels against each other, both channels alike, and the same every time.
    {
        juce::AudioBuffer<float> tones (2, 48000);

        for (int i = 0; i < 48000; ++i)
        {
            const auto x = 0.25 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / rate)
                           + 0.0625 * std::sin (juce::MathConstants<double>::twoPi * 3000.0 * i / rate);
            tones.setSample (0, i, (float) x);
            tones.setSample (1, i, (float) x);
        }

        const ExtremeStretch::Settings eight { 8.0, 0.25 };
        const auto out = ExtremeStretch::stretch (tones, eight, rate);
        const auto again = ExtremeStretch::stretch (tones, eight, rate);
        check (out.getNumChannels() == 2 && out.getNumSamples() == 8 * 48000, "extreme stretch: 8 times longer (" + juce::String (out.getNumSamples()) + " samples)");

        const auto inLevel = rmsDb (tones, 0, 0, 48000);
        const auto outLevel = rmsDb (out, 0, 48000, 6 * 48000);   // the middle six seconds
        check (std::abs (outLevel - inLevel) < 0.5, "extreme stretch: the level is kept (" + r (inLevel) + " dB in, " + r (outLevel) + " dB out)");

        const auto share1k = bandShareDb (out, 0, 48000, 6 * 48000, 1000.0), share3k = bandShareDb (out, 0, 48000, 6 * 48000, 3000.0);
        const auto inShare1k = bandShareDb (tones, 0, 0, 48000, 1000.0), inShare3k = bandShareDb (tones, 0, 0, 48000, 3000.0);
        check (share1k > -1.5 && std::abs ((share1k - share3k) - (inShare1k - inShare3k)) < 1.5,
               "extreme stretch: the pitch and colour are kept: the 1 kHz tone holds " + r (share1k, 1) + " dB of the output (" + r (inShare1k, 1)
                   + " of the input) and sits " + r (share1k - share3k, 1) + " dB over the 3 kHz tone (" + r (inShare1k - inShare3k, 1) + " in the input)");

        bool alike = true, same = true;

        for (int i = 0; i < out.getNumSamples() && alike && same; ++i)
        {
            alike = juce::exactlyEqual (out.getSample (0, i), out.getSample (1, i));
            same = juce::exactlyEqual (out.getSample (0, i), again.getSample (0, i));
        }

        check (alike && same, "extreme stretch: channels alike come out alike, and a stretch repeats bit for bit");

        // The start and the end are not silent: the first and last 50 ms carry sound.
        check (rmsDb (out, 0, 0, 2400) > outLevel - 12.0 && rmsDb (out, 0, out.getNumSamples() - 2400, 2400) > outLevel - 12.0,
               "extreme stretch: the first and last 50 ms carry sound (" + r (rmsDb (out, 0, 0, 2400)) + ", " + r (rmsDb (out, 0, out.getNumSamples() - 2400, 2400)) + " dB)");
    }

    // Settings and windows: the limits, and the window as the power of two nearest the seconds.
    check (ExtremeStretch::isValid ({ 1.0, 0.05 }) && ExtremeStretch::isValid ({ 1000.0, 2.0 }) && ! ExtremeStretch::isValid ({ 0.5, 0.25 })
               && ! ExtremeStretch::isValid ({ 1001.0, 0.25 }) && ! ExtremeStretch::isValid ({ 8.0, 0.01 }) && ! ExtremeStretch::isValid ({ 8.0, 3.0 }),
           "extreme stretch: 1 to 1000 times and a window of 0.05 to 2 s are valid, nothing beyond");
    check (ExtremeStretch::windowSize (0.25, 48000.0) == 16384 && ExtremeStretch::windowSize (0.05, 48000.0) == 2048 && ExtremeStretch::windowSize (2.0, 48000.0) == 131072
               && ExtremeStretch::windowSize (0.25, 44100.0) == 8192,
           "extreme stretch: the window is the power of two nearest the seconds (0.25 s at 48 kHz is 16384, 0.05 s 2048, 2 s 131072; 0.25 s at 44.1 kHz 8192)");

    // A factor of 1 keeps the length; a stop gives nothing; an empty input gives an empty output.
    {
        juce::AudioBuffer<float> noise (1, 24000);
        juce::Random rnd (3);

        for (int i = 0; i < 24000; ++i)
            noise.setSample (0, i, rnd.nextFloat() * 0.5f - 0.25f);

        // Noise stretched eight times holds its level across the output: no tremolo at the hop
        // rate (a lone tone wobbles instead, its few bins beating with their random phases, which
        // is the method's nature). Each 50 ms within the middle is within 2.5 dB of the whole.
        const auto eight = ExtremeStretch::stretch (noise, { 8.0, 0.1 }, rate);
        const auto level = rmsDb (eight, 0, 24000, 144000);
        double worst = 0.0;

        for (int at = 24000; at + 2400 <= 168000; at += 2400)
            worst = juce::jmax (worst, std::abs (rmsDb (eight, 0, at, 2400) - level));

        check (eight.getNumSamples() == 192000 && worst < 2.5, "extreme stretch: noise stretched 8 times holds its level, every 50 ms of the middle within 2.5 dB of the whole (worst " + r (worst) + " dB)");

        const auto one = ExtremeStretch::stretch (noise, { 1.0, 0.1 }, rate);
        const auto stopped = ExtremeStretch::stretch (noise, { 8.0, 0.1 }, rate, [] (float p) { return p < 0.5f; });
        const juce::AudioBuffer<float> empty (2, 0);
        const auto nothing = ExtremeStretch::stretch (empty, { 8.0, 0.1 }, rate);
        check (one.getNumSamples() == 24000 && std::abs (rmsDb (one, 0, 2400, 19200) - rmsDb (noise, 0, 2400, 19200)) < 1.0
                   && stopped.getNumSamples() == 0 && nothing.getNumSamples() == 0 && nothing.getNumChannels() == 2,
               "extreme stretch: 1 times keeps the length and the level (" + r (rmsDb (one, 0, 2400, 19200) - rmsDb (noise, 0, 2400, 19200)) + " dB off), a stop gives nothing, an empty input an empty output");
    }

    // A range of a file: the middle second of three stretched four times makes the file six
    // seconds, the audio before and after bit for bit where it was (the joins blended), and
    // nothing of the range's noise before its start.
    {
        juce::AudioBuffer<float> file (1, 3 * 48000);
        file.clear();
        juce::Random rnd (4);

        for (int i = 0; i < 48000; ++i)
            file.setSample (0, 48000 + i, rnd.nextFloat() * 0.5f - 0.25f);

        for (int i = 0; i < 3 * 48000; ++i)
            file.addSample (0, i, 0.1f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * i / rate));

        auto snapshot = AudioEdits::fromBuffer (juce::AudioBuffer<float> (file));
        const auto out = ExtremeStretch::stretchRange (*snapshot, 48000, 2 * 48000, { 4.0, 0.1 }, rate);
        check (out != nullptr && out->getNumFrames() == 6 * 48000, "extreme stretch of a range: a second stretched four times makes the file six seconds");

        if (out != nullptr && out->getNumFrames() == 6 * 48000)
        {
            juce::AudioBuffer<float> result (1, 6 * 48000);
            out->read (result, 0, 0, 6 * 48000);
            const auto join = (int) std::llround (0.005 * rate);
            bool before = true, after = true;

            for (int i = 0; i < 48000 - join; ++i)
                before = before && juce::exactlyEqual (result.getSample (0, i), file.getSample (0, i));

            for (int i = 2 * 48000 + join; i < 3 * 48000; ++i)
                after = after && juce::exactlyEqual (result.getSample (0, 3 * 48000 + i), file.getSample (0, i));

            const auto middle = rmsDb (result, 0, 2 * 48000, 2 * 48000), middleIn = rmsDb (file, 0, 48000, 48000);
            check (before && after && std::abs (middle - middleIn) < 1.0,
                   "extreme stretch of a range: the audio before and after is bit for bit where it was, the stretched middle at its level ("
                       + r (middle) + " against " + r (middleIn) + " dB)");
        }

        check (ExtremeStretch::stretchRange (*snapshot, 48000, 48000, { 4.0, 0.1 }, rate) == nullptr
                   && ExtremeStretch::stretchRange (*snapshot, 0, 48000, { 0.5, 0.1 }, rate) == nullptr,
               "extreme stretch of a range: an empty range or settings out of range give nothing");
    }
}
