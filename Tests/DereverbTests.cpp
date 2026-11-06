// Samples outside an edit are compared bit for bit on purpose.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/Dereverb.h"
#include "TestUtils.h"
#include "DereverbTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;
    constexpr int second = (int) rate;

    struct Burst { int start, end; };

    /** Bursts of noise, 150 ms long every 700 ms, from 0.3 s into 6 s: sounds with gaps after them. */
    std::vector<float> dryBursts (std::vector<Burst>& bursts)
    {
        std::vector<float> dry ((size_t) (6 * second), 0.0f);
        juce::Random random (77);

        for (int s = (int) (0.3 * rate); s < 5 * second; s += (int) (0.7 * rate))
        {
            const Burst b { s, s + (int) (0.15 * rate) };
            bursts.push_back (b);

            for (int i = b.start; i < b.end; ++i)
                dry[(size_t) i] = 0.3f * (2.0f * random.nextFloat() - 1.0f);
        }

        return dry;
    }

    /** The dry sound in a room: the sound itself, plus velvet noise (a tap of +1 or -1 at a
        random place in each millisecond) dying away 60 dB over reverbSeconds, as loud in all as
        the sound itself (a direct-to-reverberant ratio of 0 dB). */
    std::vector<float> inRoom (const std::vector<float>& dry, double reverbSeconds)
    {
        juce::Random random (91);
        const auto period = (int) (rate / 1000.0);
        std::vector<std::pair<int, float>> taps;
        double energy = 0.0;

        for (int at = period; at < (int) (1.5 * reverbSeconds * rate); at += period)
        {
            const auto t = at + random.nextInt (period);
            const auto g = (float) ((random.nextBool() ? 1.0 : -1.0) * std::pow (10.0, -3.0 * t / rate / reverbSeconds));
            taps.emplace_back (t, g);
            energy += (double) g * g;
        }

        const auto scale = (float) (1.0 / std::sqrt (energy));
        auto wet = dry;

        for (auto [t, g] : taps)
            for (size_t i = (size_t) t; i < wet.size(); ++i)
                wet[i] += g * scale * dry[i - (size_t) t];

        return wet;
    }

    /** Stereo: the left channel as given, the right its negative. */
    AudioDocument::Snapshot snapshotOf (const std::vector<float>& x)
    {
        juce::AudioBuffer<float> b (2, (int) x.size());

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            b.setSample (0, i, x[(size_t) i]);
            b.setSample (1, i, -x[(size_t) i]);
        }

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, (juce::int64) x.size() } };
        return std::make_shared<const AudioSnapshot> (2, std::move (segments));
    }

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    double db (double power) { return 10.0 * std::log10 (juce::jmax (power, 1.0e-30)); }

    /** The left channel's mean power in dB over the spans given. */
    double levelOver (const juce::AudioBuffer<float>& b, const std::vector<std::pair<int, int>>& spans)
    {
        double sum = 0.0;
        juce::int64 count = 0;

        for (auto [from, to] : spans)
            for (int i = from; i < to; ++i, ++count)
                sum += (double) b.getSample (0, i) * b.getSample (0, i);

        return db (sum / (double) juce::jmax ((juce::int64) 1, count));
    }

    /** The gaps after the bursts, from 100 ms after each to the next; the bursts after their first 20 ms. */
    std::vector<std::pair<int, int>> tails (const std::vector<Burst>& bursts)
    {
        std::vector<std::pair<int, int>> spans;

        for (size_t k = 0; k + 1 < bursts.size(); ++k)
            spans.emplace_back (bursts[k].end + (int) (0.1 * rate), bursts[k + 1].start);

        return spans;
    }

    std::vector<std::pair<int, int>> sounds (const std::vector<Burst>& bursts)
    {
        std::vector<std::pair<int, int>> spans;

        for (auto& b : bursts)
            spans.emplace_back (b.start + (int) (0.02 * rate), b.end);

        return spans;
    }

    void testRoom()
    {
        std::vector<Burst> bursts;
        const auto dry = dryBursts (bursts);
        const auto wet = snapshotOf (inRoom (dry, 1.0));
        const auto before = readAll (*wet);
        const auto whole = (juce::int64) dry.size();

        std::map<double, juce::AudioBuffer<float>> after;

        for (auto set : { 0.5, 1.0, 2.0 })
            if (auto r = Dereverb::reduce (*wet, 0, whole, rate, { (float) set, 12.0f }))
                after[set] = readAll (*r);

        if (after.size() != 3)
        {
            check (false, "reduce reverb: a reverberant recording is processed");
            return;
        }

        const auto tailBefore = levelOver (before, tails (bursts)), soundBefore = levelOver (before, sounds (bursts));
        const auto lowered = [&] (double set) { return tailBefore - levelOver (after[set], tails (bursts)); };
        const auto soundChange = [&] (double set) { return levelOver (after[set], sounds (bursts)) - soundBefore; };

        check (lowered (1.0) >= 5.0 && std::abs (soundChange (1.0)) < 0.5,
               "reduce reverb: bursts of sound in a 1 s room (as loud as their reverberation) with the time set to 1 s: the reverberation "
               "after them " + juce::String (lowered (1.0), 1) + " dB lower, the bursts themselves within 0.5 dB ("
                   + juce::String (soundChange (1.0), 2) + " dB)");

        check (lowered (2.0) > lowered (1.0) && lowered (1.0) > lowered (0.5) && std::abs (soundChange (2.0)) < 0.5,
               "reduce reverb: the time set longer than the room's takes away more of its tail, shorter less (0.5 s: "
                   + juce::String (lowered (0.5), 1) + " dB, 1 s: " + juce::String (lowered (1.0), 1) + " dB, 2 s: "
                   + juce::String (lowered (2.0), 1) + " dB; the bursts within 0.5 dB at 2 s, " + juce::String (soundChange (2.0), 2) + ")");

        // The right channel was the left's negative: each channel is worked alone, and the
        // gains follow power, so it still is, sample for sample.
        auto mirrored = true;
        const auto& one = after[1.0];

        for (int i = 0; i < one.getNumSamples() && mirrored; ++i)
            mirrored = one.getSample (1, i) == -one.getSample (0, i);

        check (mirrored && one.getNumChannels() == 2 && one.getNumSamples() == before.getNumSamples(),
               "reduce reverb: the same length and channels, each channel worked alone (one the other's negative still is, bit for bit)");
    }

    void testSteadyAndRange()
    {
        // A steady tone is neither starting nor dying away: it passes.
        std::vector<float> tone ((size_t) (4 * second));

        for (size_t i = 0; i < tone.size(); ++i)
            tone[i] = 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * (double) i / rate);

        const auto toneIn = snapshotOf (tone);
        const auto toneOut = Dereverb::reduce (*toneIn, 0, toneIn->getNumFrames(), rate, { 2.0f, 30.0f });
        const auto toneSpan = std::vector<std::pair<int, int>> { { second, 3 * second } };
        const auto change = toneOut != nullptr ? levelOver (readAll (*toneOut), toneSpan) - levelOver (readAll (*toneIn), toneSpan) : 99.0;

        check (std::abs (change) < 0.1, "reduce reverb: a steady tone keeps its level, even at a 2 s time and 30 dB (" + juce::String (change, 3) + " dB)");

        // A range: audio more than a frame outside it is untouched.
        std::vector<Burst> bursts;
        const auto wet = snapshotOf (inRoom (dryBursts (bursts), 1.0));
        const auto frame = Dereverb::frameSize (rate);
        const juce::int64 start = 2 * second, end = 4 * second;
        const auto r = Dereverb::reduce (*wet, start, end, rate, { 1.0f, 12.0f });
        auto outsideSame = r != nullptr, insideChanged = false;

        if (r != nullptr)
        {
            const auto a = readAll (*wet), b = readAll (*r);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < a.getNumSamples(); ++i)
                {
                    if (i < start - frame || i >= end + frame)
                        outsideSame = outsideSame && a.getSample (ch, i) == b.getSample (ch, i);
                    else if (i >= start && i < end)
                        insideChanged = insideChanged || a.getSample (ch, i) != b.getSample (ch, i);
                }
        }

        check (outsideSame && insideChanged && frame == 2048 && Dereverb::frameSize (44100.0) == 2048 && Dereverb::frameSize (96000.0) == 4096,
               "reduce reverb: a selection changes, and the audio more than a frame (2048 samples at 48 kHz, 4096 at 96) either side of it "
               "is bit for bit as it was");

        // Stopping, empty ranges and settings out of range give nothing.
        auto calls = 0;
        const auto stopped = Dereverb::reduce (*wet, 0, wet->getNumFrames(), rate, { 1.0f, 12.0f }, [&] (float) { return ++calls < 3; });
        check (stopped == nullptr && calls == 3, "reduce reverb: stopping part way gives nothing");

        const auto refused = Dereverb::reduce (*wet, 100, 100, rate, {}) == nullptr
                             && Dereverb::reduce (*wet, 0, 1000, rate, { 0.05f, 12.0f }) == nullptr
                             && Dereverb::reduce (*wet, 0, 1000, rate, { 11.0f, 12.0f }) == nullptr
                             && Dereverb::reduce (*wet, 0, 1000, rate, { 1.0f, 31.0f }) == nullptr
                             && Dereverb::reduce (*wet, 0, 1000, rate, { 1.0f, -1.0f }) == nullptr
                             && Dereverb::reduce (*wet, 0, 1000, rate, { std::nanf (""), 12.0f }) == nullptr
                             && Dereverb::isValid ({ 0.1f, 0.0f }) && Dereverb::isValid ({ 10.0f, 30.0f });
        check (refused, "reduce reverb: an empty range, and a time outside 0.1 to 10 s or a reduction outside 0 to 30 dB, are refused");
    }
}

void runDereverbTests()
{
    testRoom();
    testSteadyAndRange();
}
