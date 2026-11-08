// Samples outside an edit, and audio a gain of 1 leaves, are compared bit for bit on purpose.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/Leveller.h"
#include "../Source/Loudness.h"
#include "TestUtils.h"
#include "LevellerTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;
    constexpr int second = (int) rate;
    constexpr float roomNoise = 0.0005f;   // uniform, about -72 dBFS RMS: far below any silence threshold

    /** Syllables: noise swelling and fading four times a second, with a short gap in each. */
    std::vector<float> syllables (int frames, juce::Random& random)
    {
        std::vector<float> v ((size_t) frames);

        for (int i = 0; i < frames; ++i)
        {
            const auto phase = std::fmod ((double) i / rate * 4.0, 1.0);
            const auto envelope = phase < 0.8 ? std::sin (juce::MathConstants<double>::pi * phase / 0.8) : 0.0;
            v[(size_t) i] = (float) (envelope * (2.0 * random.nextDouble() - 1.0));
        }

        return v;
    }

    juce::AudioBuffer<float> stereo (const std::vector<float>& x)
    {
        juce::AudioBuffer<float> b (2, (int) x.size());

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            b.setSample (0, i, x[(size_t) i]);
            b.setSample (1, i, -x[(size_t) i]);
        }

        return b;
    }

    double lufsOf (const juce::AudioBuffer<float>& b, int from, int to)
    {
        juce::AudioBuffer<float> part (b.getNumChannels(), to - from);

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            part.copyFrom (ch, 0, b, ch, from, to - from);

        return Loudness::measure (part, rate).integrated;
    }

    /** Speech passages at the loudnesses given (LUFS, measured on the stereo file), each `length`
        seconds, with `pause` seconds of room noise before, between and after them. */
    juce::AudioBuffer<float> passages (const std::vector<double>& levels, double length, double pause, std::vector<std::pair<int, int>>& spans)
    {
        juce::Random random (2024);
        const auto n = (int) (length * rate), gap = (int) (pause * rate);
        std::vector<float> x ((size_t) (gap + (int) levels.size() * (n + gap)));

        for (auto& s : x)
            s = roomNoise * (2.0f * random.nextFloat() - 1.0f);

        auto at = gap;

        for (auto want : levels)
        {
            auto speech = syllables (n, random);
            const auto measured = lufsOf (stereo (speech), 0, n);
            const auto scale = (float) std::pow (10.0, (want - measured) / 20.0);

            for (int i = 0; i < n; ++i)
                x[(size_t) (at + i)] += scale * speech[(size_t) i];

            spans.emplace_back (at, at + n);
            at += n + gap;
        }

        return stereo (x);
    }

    AudioDocument::Snapshot snapshotOf (const juce::AudioBuffer<float>& b)
    {
        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (juce::AudioBuffer<float> (b)), 0, b.getNumSamples() } };
        return std::make_shared<const AudioSnapshot> (b.getNumChannels(), std::move (segments));
    }

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    double rmsDb (const juce::AudioBuffer<float>& b, int from, int to)
    {
        double sum = 0.0;

        for (int i = from; i < to; ++i)
            sum += (double) b.getSample (0, i) * b.getSample (0, i);

        return 10.0 * std::log10 (juce::jmax (1.0e-30, sum / (to - from)));
    }

    float largestPeak (const juce::AudioBuffer<float>& b)
    {
        return juce::jmax (b.getMagnitude (0, 0, b.getNumSamples()), b.getMagnitude (1, 0, b.getNumSamples()));
    }

    void testPassages()
    {
        // Loud, quiet and middling speech, 4 s each with a second of room noise around them.
        std::vector<std::pair<int, int>> spans;
        const auto in = passages ({ -12.0, -30.0, -21.0 }, 4.0, 1.0, spans);
        const auto snapshot = snapshotOf (in);
        const auto r = Leveller::level (*snapshot, 0, snapshot->getNumFrames(), rate, {});

        if (r == nullptr)
        {
            check (false, "level speech: speech is levelled");
            return;
        }

        const auto out = readAll (*r);
        juce::StringArray before, after;
        auto spreadBefore = 0.0, worst = 0.0;
        auto low = HUGE_VAL, high = -HUGE_VAL;

        // Each passage after its first and before its last half second, where the gain has
        // settled from the one before.
        for (auto [from, to] : spans)
        {
            const auto a = lufsOf (in, from + second / 2, to - second / 2), b = lufsOf (out, from + second / 2, to - second / 2);
            before.add (juce::String (a, 1));
            after.add (juce::String (b, 1));
            low = juce::jmin (low, a);
            high = juce::jmax (high, a);
            worst = juce::jmax (worst, std::abs (b - -20.0));
        }

        spreadBefore = high - low;
        check (worst <= 1.0 && spreadBefore > 17.0,
               "level speech: passages at " + before.joinIntoString (", ") + " LUFS (" + juce::String (spreadBefore, 1)
                   + " LU apart) come out at " + after.joinIntoString (", ") + " LUFS, each within 1 LU of the -20 target");

        // The fastest change: at 6 dB a second the gains rise at most 0.3 dB a step (50 ms), never
        // over what they would be unlimited, and the quiet passage still gets near the target by
        // its end (an 18 dB swing takes 3 s, and the gain starts down early for the next passage);
        // at the largest setting the gains are those with no limit, bit for bit.
        {
            Leveller::Settings slow;
            slow.speedDbPerSecond = 6.0f;
            const auto free = Leveller::gains (*snapshot, 0, snapshot->getNumFrames(), rate, {});
            const auto held = Leveller::gains (*snapshot, 0, snapshot->getNumFrames(), rate, slow);
            auto steepest = 0.0f, over = 0.0f;
            auto same = free.has_value() && held.has_value() && free->size() == held->size();

            if (same)
                for (size_t k = 0; k < held->size(); ++k)
                {
                    over = juce::jmax (over, (*held)[k] - (*free)[k]);

                    if (k > 0)
                        steepest = juce::jmax (steepest, (*held)[k] - (*held)[k - 1]);
                }

            Leveller::Settings top;
            top.speedDbPerSecond = 30.0f;
            const auto atTop = Leveller::gains (*snapshot, 0, snapshot->getNumFrames(), rate, top);
            same = same && atTop.has_value() && *atTop == *free;
            const auto slowed = Leveller::level (*snapshot, 0, snapshot->getNumFrames(), rate, slow);
            const auto quietEnd = slowed != nullptr ? lufsOf (readAll (*slowed), spans[1].second - second / 2, spans[1].second) : 0.0;
            std::cout << "      fastest change 6 dB/s: steepest rise " << steepest << " dB a step, over the free gains by " << over
                      << " dB, the quiet passage's last half second at " << quietEnd << " LUFS" << std::endl;
            check (same && steepest <= 0.3f + 1.0e-4f && over <= 1.0e-5f && std::abs (quietEnd - -20.0) <= 1.5,
                   "level speech: with the fastest change at 6 dB a second the gains rise at most 0.3 dB a step (" + juce::String (steepest, 3)
                       + "), never over the unlimited gains, and the quiet passage's last half second is within 1.5 LU of the target ("
                       + juce::String (quietEnd, 1) + " LUFS: its 18 dB swing takes 3 s, and the gain starts down early for the next passage); "
                       "at 30 dB a second the gains are the unlimited ones bit for bit");
        }

        auto mirrored = out.getNumSamples() == in.getNumSamples();

        for (int i = 0; i < out.getNumSamples() && mirrored; ++i)
            mirrored = out.getSample (1, i) == -out.getSample (0, i);

        check (mirrored && largestPeak (out) <= std::pow (10.0f, Leveller::Settings::ceilingDb / 20.0f),
               "level speech: the same length, every channel given the same gain (a right channel that was the left's negative still is, "
               "bit for bit), and no sample above -1 dBFS (largest " + juce::String (juce::Decibels::gainToDecibels (largestPeak (out)), 2) + " dBFS)");
    }

    void testPauses()
    {
        // Two passages already at the target with 4 s of room noise between: the noise keeps
        // its level rather than being raised towards the target.
        std::vector<std::pair<int, int>> spans;
        const auto in = passages ({ -20.0, -20.0 }, 3.0, 4.0, spans);
        const auto snapshot = snapshotOf (in);
        const auto r = Leveller::level (*snapshot, 0, snapshot->getNumFrames(), rate, {});
        const auto from = spans[0].second + second, to = spans[1].first - second;
        const auto change = r != nullptr ? rmsDb (readAll (*r), from, to) - rmsDb (in, from, to) : 99.0;
        check (std::abs (change) < 1.0,
               "level speech: room noise in a 4 s pause between speech at the target keeps its level (" + juce::String (change, 2)
                   + " dB) rather than being raised to the target");

        // Nothing but room noise: no speech, so no gain, bit for bit.
        juce::Random random (5);
        std::vector<float> quiet ((size_t) (3 * second));

        for (auto& s : quiet)
            s = roomNoise * (2.0f * random.nextFloat() - 1.0f);

        const auto quietIn = snapshotOf (stereo (quiet));
        const auto quietOut = Leveller::level (*quietIn, 0, quietIn->getNumFrames(), rate, {});
        auto same = quietOut != nullptr;

        if (same)
        {
            const auto a = readAll (*quietIn), b = readAll (*quietOut);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    same = same && a.getSample (ch, i) == b.getSample (ch, i);
        }

        check (same, "level speech: a file of room noise alone, below the silence threshold, is left bit for bit as it was");
    }

    void testLimits()
    {
        // Speech far below the target is raised by the largest boost and no more.
        std::vector<std::pair<int, int>> spans;
        const auto in = passages ({ -45.0 }, 4.0, 1.0, spans);
        const auto snapshot = snapshotOf (in);
        Leveller::Settings s;
        s.silenceLufs = -60.0f;
        const auto r = Leveller::level (*snapshot, 0, snapshot->getNumFrames(), rate, s);
        const auto [from, to] = spans[0];
        const auto raised = r != nullptr ? lufsOf (readAll (*r), from + second / 2, to - second / 2) - lufsOf (in, from + second / 2, to - second / 2) : 0.0;
        check (std::abs (raised - 12.0) < 0.5, "level speech: speech at -45 LUFS for a -20 target is raised by the largest boost, 12 dB, and no more ("
                                                   + juce::String (raised, 2) + " dB)");

        // A click in quiet speech: the speech around it is raised only as far as the ceiling allows.
        std::vector<std::pair<int, int>> clickSpans;
        auto clicked = passages ({ -32.0 }, 4.0, 1.0, clickSpans);
        const auto clickAt = clickSpans[0].first + 2 * second;
        clicked.setSample (0, clickAt, 0.5f);
        clicked.setSample (1, clickAt, -0.5f);
        const auto clickIn = snapshotOf (clicked);
        const auto clickOut = Leveller::level (*clickIn, 0, clickIn->getNumFrames(), rate, {});
        const auto peak = clickOut != nullptr ? largestPeak (readAll (*clickOut)) : 1.0f;
        const auto elsewhere = clickOut != nullptr ? lufsOf (readAll (*clickOut), clickSpans[0].first + second / 2, clickSpans[0].first + second) : -99.0;
        check (peak <= std::pow (10.0f, Leveller::Settings::ceilingDb / 20.0f) && peak > 0.8f && elsewhere > -21.0,
               "level speech: a click at -6 dBFS in speech raised 12 dB comes out at " + juce::String (juce::Decibels::gainToDecibels (peak), 2)
                   + " dBFS, under the -1 dBFS ceiling, while the speech a second from it still reaches the target (" + juce::String (elsewhere, 1) + " LUFS)");
    }

    void testRangeAndRefusals()
    {
        std::vector<std::pair<int, int>> spans;
        const auto in = passages ({ -32.0, -32.0 }, 3.0, 1.0, spans);
        const auto snapshot = snapshotOf (in);
        const juce::int64 start = spans[0].first + second, end = spans[1].second - second;
        const auto r = Leveller::level (*snapshot, start, end, rate, {});
        auto outsideSame = r != nullptr, raisedInside = false;
        auto edgeStep = 0.0f;

        if (r != nullptr)
        {
            const auto out = readAll (*r);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < in.getNumSamples(); ++i)
                    if (i < start || i >= end)
                        outsideSame = outsideSame && in.getSample (ch, i) == out.getSample (ch, i);

            raisedInside = lufsOf (out, spans[1].first, (int) end) - lufsOf (in, spans[1].first, (int) end) > 6.0;
            edgeStep = juce::jmax (std::abs (out.getSample (0, (int) start) - in.getSample (0, (int) start)),
                                   std::abs (out.getSample (0, (int) end - 1) - in.getSample (0, (int) end - 1)));
        }

        check (outsideSame && raisedInside && edgeStep < 1.0e-3f,
               "level speech: a selection is levelled, the audio either side bit for bit as it was, and the gain eased in at its edges "
               "(the first and last samples within " + juce::String (edgeStep, 5) + " of what they were)");

        auto calls = 0;
        const auto stopped = Leveller::level (*snapshot, 0, snapshot->getNumFrames(), rate, {}, [&] (float) { return ++calls < 2; });
        check (stopped == nullptr && calls == 2, "level speech: stopping part way gives nothing");

        Leveller::Settings tooSlow, tooFast;
        tooSlow.speedDbPerSecond = 0.5f;
        tooFast.speedDbPerSecond = 31.0f;
        const auto speedRefused = ! Leveller::isValid (tooSlow) && ! Leveller::isValid (tooFast) && Leveller::isValid ({});
        const auto refused = speedRefused && Leveller::level (*snapshot, 10, 10, rate, {}) == nullptr
                             && ! Leveller::isValid ({ -41.0f, 12.0f, 12.0f, -50.0f }) && ! Leveller::isValid ({ -4.0f, 12.0f, 12.0f, -50.0f })
                             && ! Leveller::isValid ({ -20.0f, 31.0f, 12.0f, -50.0f }) && ! Leveller::isValid ({ -20.0f, 12.0f, -1.0f, -50.0f })
                             && ! Leveller::isValid ({ -20.0f, 12.0f, 12.0f, -19.0f }) && ! Leveller::isValid ({ -20.0f, 12.0f, 12.0f, -81.0f })
                             && ! Leveller::isValid ({ std::nanf (""), 12.0f, 12.0f, -50.0f })
                             && Leveller::isValid ({ -40.0f, 0.0f, 30.0f, -80.0f }) && Leveller::isValid ({ -5.0f, 30.0f, 0.0f, -20.0f });
        check (refused, "level speech: an empty range, and settings outside their ranges, are refused");
    }
}

void runLevellerTests()
{
    testPassages();
    testPauses();
    testLimits();
    testRangeAndRefusals();
}
