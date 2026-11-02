#include <JuceHeader.h>
#include "../Source/Modulation.h"
#include "TestUtils.h"
#include "ModulationTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;
    constexpr int frames = 48000;

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    juce::AudioBuffer<float> sine (int channels, double hz, float level = 0.5f)
    {
        juce::AudioBuffer<float> b (channels, frames);

        for (int i = 0; i < frames; ++i)
            for (int ch = 0; ch < channels; ++ch)
                b.setSample (ch, i, level * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / rate));

        return b;
    }

    juce::AudioBuffer<float> noise (int channels, int seed)
    {
        juce::Random r (seed);
        juce::AudioBuffer<float> b (channels, frames);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, r.nextFloat() - 0.5f);

        return b;
    }

    /** The loudest sample of the second half (after everything has settled). */
    float settledPeak (const juce::AudioBuffer<float>& b, int ch)
    {
        return b.getMagnitude (ch, frames / 2, frames / 2);
    }

    template <typename Settings>
    juce::AudioBuffer<float> process (const juce::AudioBuffer<float>& in, const Settings& s)
    {
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        AudioEdits::Snapshot out;

        if constexpr (std::is_same_v<Settings, Modulation::Delay>)
            out = Modulation::sweptDelay (*snap, 0, frames, rate, s);
        else
            out = Modulation::phaser (*snap, 0, frames, rate, s);

        return out != nullptr ? readAll (*out) : juce::AudioBuffer<float>();
    }

    bool identical (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int channel = -1)
    {
        if (a.getNumSamples() != b.getNumSamples())
            return false;

        for (int ch = 0; ch < a.getNumChannels(); ++ch)
            if (channel < 0 || ch == channel)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    if (! juce::exactlyEqual (a.getSample (ch, i), b.getSample (ch, i)))
                        return false;

        return true;
    }
}

void runModulationTests()
{
    const auto in = noise (2, 3);

    // A fixed delay: no depth, all wet, no feedback; 1 ms is 48 frames at 48 kHz.
    {
        const Modulation::Delay fixed { 1.0, 1.0, 0.0, 0.0, 100.0, 90.0 };
        const auto out = process (in, fixed);
        auto delayed = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < frames; ++i)
                delayed = delayed && juce::exactlyEqual (out.getSample (ch, i), i < 48 ? 0.0f : in.getSample (ch, i - 48));

        auto dryOnly = Modulation::chorusDefaults();
        dryOnly.mixPercent = 0.0;
        check (delayed && identical (process (in, dryOnly), in),
               "swept delay: with no depth and all wet, the input exactly 1 ms (48 frames) late; with no mix, the input bit for bit");
    }

    // Half wet with a fixed 48-frame delay is a comb: 500 Hz (half a cycle late) cancels, 1 kHz (a whole cycle) adds up.
    {
        const Modulation::Delay comb { 1.0, 1.0, 0.0, 0.0, 50.0, 0.0 };
        const auto notch = settledPeak (process (sine (1, 500.0), comb), 0);
        const auto peak = settledPeak (process (sine (1, 1000.0), comb), 0);
        std::cout << "      comb: 500 Hz at " << notch << ", 1 kHz at " << peak << " (in 0.5)" << std::endl;
        check (notch < 1.0e-5f && std::abs (peak - 0.5f) < 1.0e-4f, "swept delay as a comb: a 1 ms delay half wet cancels 500 Hz and passes 1 kHz whole");
    }

    // The sweep and the spread: identical channels come out identical with no spread and apart with some.
    {
        juce::AudioBuffer<float> same (2, frames);
        same.copyFrom (0, 0, in, 0, 0, frames);
        same.copyFrom (1, 0, in, 0, 0, frames);
        auto narrow = Modulation::chorusDefaults();
        narrow.spreadDegrees = 0.0;
        const auto together = process (same, narrow);
        const auto apart = process (same, Modulation::chorusDefaults());
        auto channelsMatch = true;

        for (int i = 0; i < frames; ++i)
            channelsMatch = channelsMatch && juce::exactlyEqual (together.getSample (0, i), together.getSample (1, i));

        auto differ = false;

        for (int i = 0; i < frames && ! differ; ++i)
            differ = ! juce::exactlyEqual (apart.getSample (0, i), apart.getSample (1, i));

        // With depth, the delay moves: the output is not the fixed-delay output at either end of the sweep.
        auto atShortest = Modulation::chorusDefaults(), atLongest = Modulation::chorusDefaults();
        atShortest.depthMs = 0.0;
        atLongest.delayMs += atLongest.depthMs;
        atLongest.depthMs = 0.0;
        const auto swept = process (in, Modulation::chorusDefaults());
        check (channelsMatch && differ && ! identical (swept, process (in, atShortest), 0) && ! identical (swept, process (in, atLongest), 0),
               "swept delay: identical channels stay identical with no spread and part with a 90-degree spread; the delay sweeps between its ends");
    }

    // Feedback at its limit stays bounded on noise.
    {
        auto hot = Modulation::flangerDefaults();
        hot.feedback = 0.95;
        const auto out = process (in, hot);
        auto finite = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < frames; ++i)
                finite = finite && std::isfinite (out.getSample (ch, i));

        hot.feedback = -0.95;
        const auto negative = process (in, hot);
        std::cout << "      flanger at 0.95 feedback: peak " << out.getMagnitude (0, frames) << ", at -0.95: " << negative.getMagnitude (0, frames) << " (noise peaks 0.5)" << std::endl;
        check (finite && out.getMagnitude (0, frames) < 20.0f && negative.getMagnitude (0, frames) < 20.0f,
               "flanger: feedback of +-0.95 on noise stays finite and bounded");
    }

    // The phaser: all-passes alone keep a sine's level; half wet, two stages fixed at 1 kHz cancel 1 kHz.
    {
        const Modulation::Phaser allWet { 1.0, 1000.0, 1000.0, 2, 0.0, 100.0, 0.0 };
        const Modulation::Phaser half { 1.0, 1000.0, 1000.0, 2, 0.0, 50.0, 0.0 };
        const auto kept = settledPeak (process (sine (1, 300.0), allWet), 0);
        const auto notch = settledPeak (process (sine (1, 1000.0), half), 0);
        const auto passed = settledPeak (process (sine (1, 100.0), half), 0);
        std::cout << "      phaser: all-pass 300 Hz at " << kept << ", notch 1 kHz at " << notch << ", 100 Hz at " << passed << " (in 0.5)" << std::endl;
        check (std::abs (kept - 0.5f) < 1.0e-3f && notch < 1.0e-4f && passed > 0.45f,
               "phaser: all wet keeps a sine's level; half wet, two all-passes at 1 kHz cancel 1 kHz and leave 100 Hz nearly whole");

        auto hot = Modulation::Phaser {};
        hot.feedback = 0.95;
        hot.stages = 12;
        const auto out = process (in, hot);
        auto differ = false;

        for (int i = 0; i < frames && ! differ; ++i)
            differ = ! juce::exactlyEqual (out.getSample (0, i), out.getSample (1, i));

        check (out.getMagnitude (0, frames) < 20.0f && std::isfinite (out.getMagnitude (0, frames)) && differ,
               "phaser: 12 stages with 0.95 feedback on noise stay bounded; the spread sets the channels apart");
    }

    // Settings that can't be used, an empty range, and frames outside the range.
    {
        auto badStages = Modulation::Phaser {};
        badStages.stages = 3;
        auto badRange = Modulation::Phaser {};
        badRange.highHz = 100.0;
        auto badFeedback = Modulation::flangerDefaults();
        badFeedback.feedback = 1.0;
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        const auto part = Modulation::sweptDelay (*snap, 1000, 2000, rate, Modulation::chorusDefaults());
        auto outsideKept = part != nullptr;

        if (part != nullptr)
        {
            const auto b = readAll (*part);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < frames; ++i)
                    if (i < 1000 || i >= 2000)
                        outsideKept = outsideKept && juce::exactlyEqual (b.getSample (ch, i), in.getSample (ch, i));
        }

        check (Modulation::phaser (*snap, 0, frames, rate, badStages) == nullptr && Modulation::phaser (*snap, 0, frames, rate, badRange) == nullptr
                   && Modulation::sweptDelay (*snap, 0, frames, rate, badFeedback) == nullptr
                   && Modulation::sweptDelay (*snap, 500, 500, rate, Modulation::chorusDefaults()) == nullptr && outsideKept,
               "modulation: an odd stage count, a range upside down, feedback of 1 or an empty range refused; frames outside the range bit for bit");
    }
}
