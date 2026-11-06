// Output rendered in different block sizes is compared bit for bit on purpose.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/Scrubber.h"
#include "TestUtils.h"
#include "ScrubberTests.h"

namespace
{
    using TestUtils::check;

    constexpr double deviceRate = 48000.0;

    /** Stereo (or mono) of a 440 Hz tone at 0.5 on the left and 0.25 on the right, `seconds` long. */
    AudioDocument::Snapshot tone (double rate, double seconds, int channels = 2)
    {
        juce::AudioBuffer<float> b (channels, (int) (seconds * rate));

        for (int i = 0; i < b.getNumSamples(); ++i)
            for (int ch = 0; ch < channels; ++ch)
                b.setSample (ch, i, (float) ((ch == 0 ? 0.5 : 0.25) * std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / rate)));

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, (juce::int64) (seconds * rate) } };
        return std::make_shared<const AudioSnapshot> (channels, std::move (segments));
    }

    /** Renders `frames` device frames in blocks of `block`, moving the target before each block by
        `perFrame` audio frames for each device frame of the block (0: held still). */
    juce::AudioBuffer<float> run (Scrubber& s, const AudioSnapshot& audio, double audioRate, int frames, int block, double& target, double perFrame)
    {
        juce::AudioBuffer<float> out (2, frames);
        out.clear();

        for (int done = 0; done < frames; done += block)
        {
            const auto n = juce::jmin (block, frames - done);
            target += perFrame * n;
            s.moveTo ((juce::int64) std::llround (target));
            s.render (audio, audioRate, deviceRate, out, done, n);
        }

        return out;
    }

    /** The left channel's pitch over [from, to): rising zero crossings a second. */
    double pitchOf (const juce::AudioBuffer<float>& b, int from, int to)
    {
        int first = -1, last = -1, crossings = 0;

        for (int i = from + 1; i < to; ++i)
            if (b.getSample (0, i - 1) < 0.0f && b.getSample (0, i) >= 0.0f)
            {
                if (first < 0)
                    first = i;
                else
                    ++crossings;

                last = i;
            }

        return crossings > 0 ? crossings * deviceRate / (last - first) : 0.0;
    }

    double rmsOf (const juce::AudioBuffer<float>& b, int ch, int from, int to)
    {
        double sum = 0.0;

        for (int i = from; i < to; ++i)
            sum += (double) b.getSample (ch, i) * b.getSample (ch, i);

        return std::sqrt (sum / juce::jmax (1, to - from));
    }

    void testSpeeds()
    {
        const auto audio = tone (deviceRate, 10.0);
        const auto second = (int) deviceRate;
        juce::StringArray seen;
        auto ok = true;

        // Dragged at normal speed, twice it, and backwards: the tone at its pitch, an octave up,
        // and at its pitch again, at full level once the head has caught up.
        for (auto [perFrame, wanted, name] : { std::tuple { 1.0, 440.0, "normal" }, std::tuple { 2.0, 880.0, "twice" }, std::tuple { -1.0, 440.0, "backwards" } })
        {
            Scrubber s;
            s.prepare (256);
            auto target = 5.0 * deviceRate;
            s.start ((juce::int64) target);
            const auto out = run (s, *audio, deviceRate, 2 * second, 256, target, perFrame);
            const auto pitch = pitchOf (out, second / 2, 2 * second);
            const auto level = rmsOf (out, 0, second / 2, 2 * second) / (0.5 / std::sqrt (2.0));
            const auto behind = (target - s.getHead()) / perFrame / deviceRate;
            ok = ok && std::abs (pitch - wanted) < wanted * 0.01 && std::abs (juce::Decibels::gainToDecibels (level)) < 0.5 && behind > 0.0 && behind < 0.07;
            seen.add (juce::String (name) + " " + juce::String (pitch, 1) + " Hz at " + juce::String (juce::Decibels::gainToDecibels (level), 2)
                      + " dB, " + juce::String (behind * 1000.0, 0) + " ms behind");
        }

        check (ok, "scrubbing: dragged at normal speed the tone plays at its pitch and level, twice as fast an octave up, backwards at its pitch, "
                   "the sound a little behind the mouse (" + seen.joinIntoString ("; ") + ")");
    }

    void testStillAndFastest()
    {
        const auto audio = tone (deviceRate, 30.0);
        const auto second = (int) deviceRate;

        // Moved, then held still: the head catches up and the sound falls silent.
        Scrubber s;
        s.prepare (256);
        auto target = 2.0 * deviceRate;
        s.start ((juce::int64) target);
        const auto silentAtStart = rmsOf (run (s, *audio, deviceRate, 256, 256, target, 0.0), 0, 0, 256) == 0.0;
        run (s, *audio, deviceRate, second, 256, target, 1.0);
        const auto held = run (s, *audio, deviceRate, second, 256, target, 0.0);
        const auto quiet = rmsOf (held, 0, second / 2, second);
        check (silentAtStart && quiet < 1.0e-4 && std::abs (s.getHead() - target) < 1.0,
               "scrubbing: silent where it starts until the mouse moves, and held still the head catches up (within "
                   + juce::String (std::abs (s.getHead() - target), 3) + " frames) and falls silent (" + juce::String (juce::Decibels::gainToDecibels (quiet), 1)
                   + " dBFS after half a second)");

        // A jump of 20 s: the head goes no faster than four times normal.
        Scrubber f;
        f.prepare (256);
        auto far = 1.0 * deviceRate;
        f.start ((juce::int64) far);
        far += 20.0 * deviceRate;
        f.moveTo ((juce::int64) far);
        const auto from = 1.0 * deviceRate;   // where it started (the head is put there by the first render)
        run (f, *audio, deviceRate, second, 256, far, 0.0);
        const auto speed = (f.getHead() - from) / deviceRate;
        check (speed <= Scrubber::fastest && speed > Scrubber::fastest - 0.1,
               "scrubbing: a jump far ahead is followed at four times normal speed at most (" + juce::String (speed, 3) + " times over a second)");
    }

    void testBlocksAndStop()
    {
        const auto audio = tone (deviceRate, 10.0);
        const auto second = (int) deviceRate;

        // The same moves in blocks of 64 and of 1000: the same sound, bit for bit (the target moves
        // only before the first block here, as blocks of different sizes see moves at different times).
        juce::AudioBuffer<float> outs[2];
        int i = 0;

        for (auto block : { 64, 1000 })
        {
            Scrubber s;
            s.prepare (block);
            auto target = 3.0 * deviceRate;
            s.start ((juce::int64) target);
            s.moveTo ((juce::int64) (target + 0.7 * deviceRate));
            juce::AudioBuffer<float> out (2, second);
            out.clear();

            for (int done = 0; done < second; done += block)
                s.render (*audio, deviceRate, deviceRate, out, done, juce::jmin (block, second - done));

            outs[i++] = out;
        }

        auto same = true, heard = false;

        for (int ch = 0; ch < 2; ++ch)
            for (int k = 0; k < second; ++k)
            {
                same = same && outs[0].getSample (ch, k) == outs[1].getSample (ch, k);
                heard = heard || std::abs (outs[0].getSample (ch, k)) > 0.1f;
            }

        check (same && heard, "scrubbing: rendered in blocks of 64 and of 1000 frames, the same sound bit for bit");

        // Let go while moving: the sound fades out within 50 ms, and then nothing is left to play.
        Scrubber s;
        s.prepare (256);
        auto target = 3.0 * deviceRate;
        s.start ((juce::int64) target);
        run (s, *audio, deviceRate, second / 2, 256, target, 1.0);
        s.stop();
        auto rest = target;
        const auto after = run (s, *audio, deviceRate, second / 10, 256, rest, 0.0);
        const auto tail = rmsOf (after, 0, (int) (0.05 * deviceRate), second / 10);
        check (tail < 1.0e-4 && ! s.isSounding(), "scrubbing: let go, the sound fades out within 50 ms (" + juce::String (juce::Decibels::gainToDecibels (juce::jmax (tail, 1.0e-12)), 1)
                                                      + " dBFS after) and stops");
    }

    void testRatesAndChannels()
    {
        // A 44.1 kHz file on a 48 kHz device, dragged at its normal speed: its own pitch.
        const auto audio = tone (44100.0, 10.0);
        Scrubber s;
        s.prepare (256);
        auto target = 4.0 * 44100.0;
        s.start ((juce::int64) target);
        const auto out = run (s, *audio, 44100.0, 2 * (int) deviceRate, 256, target, 44100.0 / deviceRate);
        const auto pitch = pitchOf (out, (int) deviceRate / 2, 2 * (int) deviceRate);

        // A mono file: the same on both sides. Past either end of the file: silence.
        const auto mono = tone (deviceRate, 4.0, 1);
        Scrubber m;
        m.prepare (256);
        auto at = 1.0 * deviceRate;
        m.start ((juce::int64) at);
        const auto both = run (m, *mono, deviceRate, (int) deviceRate, 256, at, 1.0);
        auto sameSides = true;

        for (int i = 0; i < both.getNumSamples(); ++i)
            sameSides = sameSides && both.getSample (0, i) == both.getSample (1, i);

        Scrubber e;
        e.prepare (256);
        auto end = 4.0 * deviceRate;
        e.start ((juce::int64) end);
        const auto past = run (e, *mono, deviceRate, (int) deviceRate, 256, end, 1.0);
        const auto beyond = rmsOf (past, 0, (int) (0.2 * deviceRate), (int) deviceRate);

        check (std::abs (pitch - 440.0) < 4.4 && sameSides && rmsOf (both, 0, 24000, 48000) > 0.3 && beyond == 0.0 && e.getHead() <= 4.0 * deviceRate,
               "scrubbing: a 44.1 kHz file on a 48 kHz device plays at its own pitch (" + juce::String (pitch, 1) + " Hz), a mono file the same on both "
               "sides, and past the end of the file there is silence and the head stops at the end");
    }
}

void runScrubberTests()
{
    testSpeeds();
    testStillAndFastest();
    testBlocksAndStop();
    testRatesAndChannels();
}
