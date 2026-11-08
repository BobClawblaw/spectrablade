#include <JuceHeader.h>
#include "../Source/WowFlutter.h"
#include "../Source/AudioEdits.h"
#include "TestUtils.h"
#include "WowFlutterTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    /** Six seconds of three steady tones (220, 330 and 1,760 Hz, with harmonics) as a machine would
        play them at a speed wandering by `wowPercent` at `wowHz`; with `vibrato`, the 330 Hz tone
        alone also wavers 2 % at 6 Hz. */
    juce::AudioBuffer<float> music (double wowPercent, double wowHz, bool vibrato)
    {
        juce::AudioBuffer<float> b (1, 6 * 48000);
        double phase[3] = { 0.0, 0.0, 0.0 };
        const double tones[3] = { 220.0, 330.0, 1760.0 };

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const auto t = i / rate;
            const auto speed = 1.0 + wowPercent / 100.0 * std::sin (juce::MathConstants<double>::twoPi * wowHz * t);
            auto v = 0.0;

            for (int k = 0; k < 3; ++k)
            {
                const auto waver = vibrato && k == 1 ? 1.0 + 0.02 * std::sin (juce::MathConstants<double>::twoPi * 6.0 * t) : 1.0;
                phase[k] += juce::MathConstants<double>::twoPi * tones[k] * speed * waver / rate;

                for (int h = 1; h <= 3; ++h)
                    v += 0.1 / h * std::sin (h * phase[k]);
            }

            b.setSample (0, i, (float) v);
        }

        return b;
    }

    /** The frequency of the strongest partial near hz in [from, from + length), by a parabola over
        the DFT's peak bins. */
    double frequencyNear (const juce::AudioBuffer<float>& b, double hz, int from, int length)
    {
        auto best = 0.0, bestPower = -1.0;
        const auto binHz = rate / length;
        auto bestBin = 0;
        std::vector<double> powers;

        for (int k = (int) (hz * 0.97 / binHz); k <= (int) (hz * 1.03 / binHz); ++k)
        {
            double re = 0.0, im = 0.0;

            for (int i = 0; i < length; ++i)
            {
                const auto w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / length);
                const auto phase = juce::MathConstants<double>::twoPi * k * i / (double) length;
                re += b.getSample (0, from + i) * w * std::cos (phase);
                im += b.getSample (0, from + i) * w * std::sin (phase);
            }

            powers.push_back (re * re + im * im);

            if (powers.back() > bestPower)
            {
                bestPower = powers.back();
                bestBin = k;
                best = (double) powers.size() - 1;
            }
        }

        const auto i = (size_t) best;

        if (i == 0 || i + 1 >= powers.size())
            return bestBin * binHz;

        const auto a = std::log (powers[i - 1]), bb = std::log (powers[i]), c = std::log (powers[i + 1]);
        return (bestBin + 0.5 * (a - c) / (a - 2.0 * bb + c)) * binHz;
    }

    /** The spread of a partial's frequency over 100 ms windows across 1 to 5 s, as a share of its mean. */
    double wobbleOf (const juce::AudioBuffer<float>& b, double hz)
    {
        std::vector<double> f;

        for (int from = 48000; from + 4800 <= 5 * 48000; from += 2400)
            f.push_back (frequencyNear (b, hz, from, 4800));

        auto mean = 0.0;

        for (auto v : f)
            mean += v;

        mean /= (double) f.size();
        auto worst = 0.0;

        for (auto v : f)
            worst = juce::jmax (worst, std::abs (v - mean) / mean);

        return worst;
    }
}

void runWowFlutterTests()
{
    std::cout << "Wow and flutter:" << std::endl;

    // Tones played with 1 % of wow at 2 Hz: the speed curve reads the wow, and corrected the tones
    // hold steady within 0.1 %; steady tones are left within 0.02 %; one tone's vibrato is not
    // taken for wow (the others stay within 0.05 %); refused settings; cancelling.
    {
        const auto wobbly = music (1.0, 2.0, false), steady = music (0.0, 2.0, false), vibrato = music (0.0, 2.0, true);
        WowFlutter::Settings s;
        const auto drift = WowFlutter::measure (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (wobbly)), 0, wobbly.getNumSamples(), rate, s);
        auto most = 0.0;

        for (auto v : drift.speed)
            most = juce::jmax (most, std::abs (v - 1.0));

        const auto corrected = readAll (*WowFlutter::correct (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (wobbly)), 0, wobbly.getNumSamples(), rate, s));
        const auto before = wobbleOf (wobbly, 1760.0), after = wobbleOf (corrected, 1760.0);
        const auto steadyOut = readAll (*WowFlutter::correct (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (steady)), 0, steady.getNumSamples(), rate, s));
        const auto steadyMoved = wobbleOf (steadyOut, 1760.0);
        const auto vibratoOut = readAll (*WowFlutter::correct (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (vibrato)), 0, vibrato.getNumSamples(), rate, s));
        const auto othersMoved = juce::jmax (wobbleOf (vibratoOut, 1760.0), wobbleOf (vibratoOut, 220.0));
        WowFlutter::Settings bad;
        bad.fastestHz = 30.0;
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (wobbly));
        const auto refused = WowFlutter::correct (*snap, 0, wobbly.getNumSamples(), rate, bad) == nullptr && WowFlutter::correct (*snap, 0, 4000, rate, s) == nullptr;
        const auto cancelled = WowFlutter::correct (*snap, 0, wobbly.getNumSamples(), rate, s, [] (float) { return false; }) == nullptr;

        std::cout << "      1 % wow at 2 Hz: the curve reads up to " << most * 100.0 << " %; the 1,760 Hz partial wobbles " << before * 100.0 << " % before, " << after * 100.0
                  << " % after; steady tones moved " << steadyMoved * 100.0 << " %; with one tone's vibrato the others moved " << othersMoved * 100.0 << " %" << std::endl;
        check (most > 0.007 && most < 0.013 && before > 0.008 && after < 0.001 && steadyMoved < 0.0002 && othersMoved < 0.0005 && refused && cancelled,
               "wow and flutter: 1 % of wow at 2 Hz on three tones reads as a speed curve wandering 0.7 to 1.3 %; corrected, a partial that wobbled "
               "over 0.8 % holds within 0.1 %; steady tones are moved under 0.02 %; one tone's vibrato is not taken for wow (the others move under "
               "0.05 %); a fastest drift past 20 Hz and a range under half a second are refused; it can be cancelled");
    }
}
