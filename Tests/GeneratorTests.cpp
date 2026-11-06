#include <JuceHeader.h>
#include "../Source/Generator.h"
#include "TestUtils.h"
#include "GeneratorTests.h"

namespace
{
    using TestUtils::check;
    constexpr double rate = 48000.0;

    /** A sine's amplitude in [from, from + n) of a channel. */
    double amplitude (const juce::AudioBuffer<float>& b, int channel, double frequency, int from, int n)
    {
        double re = 0.0, im = 0.0;

        for (int i = from; i < from + n; ++i)
        {
            const auto w = juce::MathConstants<double>::twoPi * frequency * i / rate;
            re += b.getSample (channel, i) * std::cos (w);
            im += b.getSample (channel, i) * std::sin (w);
        }

        return 2.0 * std::sqrt (re * re + im * im) / n;
    }

    /** A channel's power from lo to hi Hz, averaged over Hann-windowed 4096-sample frames. */
    double bandPower (const juce::AudioBuffer<float>& b, int channel, double lo, double hi)
    {
        constexpr int order = 12, size = 1 << order;
        juce::dsp::FFT fft (order);
        juce::dsp::WindowingFunction<float> window (size, juce::dsp::WindowingFunction<float>::hann, false);
        std::vector<float> data (2 * size);
        double sum = 0.0;

        for (int start = 0; start + size <= b.getNumSamples(); start += size)
        {
            std::fill (data.begin(), data.end(), 0.0f);
            std::copy_n (b.getReadPointer (channel, start), size, data.begin());
            window.multiplyWithWindowingTable (data.data(), size);
            fft.performFrequencyOnlyForwardTransform (data.data());

            for (int k = (int) std::ceil (lo * size / rate); k < (int) (hi * size / rate); ++k)
                sum += juce::square ((double) data[(size_t) k]);
        }

        return sum;
    }

    double db (double ratio)   { return 20.0 * std::log10 (juce::jmax (ratio, 1.0e-12)); }

    juce::String r (double v, int places = 1)   { return juce::String (v, places); }

    void testTones()
    {
        Generator::Tone sine;
        const auto b = Generator::tone (sine, 2, (int) rate, rate);
        const auto gain = juce::Decibels::decibelsToGain (-12.0);
        const auto peak = b.getMagnitude (0, 0, b.getNumSamples());
        const auto fiveMs = (int) (0.005 * rate);
        bool same = true;

        for (int i = 0; i < b.getNumSamples(); ++i)
            same = same && juce::exactlyEqual (b.getSample (0, i), b.getSample (1, i));

        check (std::abs (peak / gain - 1.0) < 1.0e-3 && std::abs (amplitude (b, 0, 1000.0, 4800, 4800) / gain - 1.0) < 1.0e-3 && same
                   && juce::exactlyEqual (b.getSample (0, 0), 0.0f) && b.getMagnitude (0, 0, fiveMs / 2) < 0.5f * (float) gain
                   && std::abs (b.getSample (0, b.getNumSamples() - 1)) < 1.0e-3f,
               "tones: a 1 kHz sine at -12 dBFS peaks at " + r (db (peak), 3) + " dBFS, the same on both channels, faded in and out over 5 ms");

        // Each wave's fundamental against its ideal: 4/pi for a square, 2/pi a sawtooth, 8/pi^2 a triangle.
        struct Expect { Generator::Wave wave; double fundamental; const char* name; };
        const Expect waves[] { { Generator::Wave::square, 4.0 / juce::MathConstants<double>::pi, "square" },
                               { Generator::Wave::sawtooth, 2.0 / juce::MathConstants<double>::pi, "sawtooth" },
                               { Generator::Wave::triangle, 8.0 / juce::square (juce::MathConstants<double>::pi), "triangle" } };
        juce::String report;
        bool right = true;

        for (auto& e : waves)
        {
            Generator::Tone t;
            t.wave = e.wave;
            t.frequency = t.endFrequency = 100.0;
            t.fadeMs = 0.0;
            const auto w = Generator::tone (t, 1, (int) rate, rate);
            const auto p = w.getMagnitude (0, 0, w.getNumSamples()) / gain;
            const auto f = amplitude (w, 0, 100.0, 0, (int) rate) / gain;
            right = right && p < 1.01 && p > 0.95 && std::abs (f / e.fundamental - 1.0) < 0.01;
            report << e.name << " peak " << r (p, 3) << " fundamental " << r (f / e.fundamental, 3) << "; ";
        }

        check (right, "tones: square, sawtooth and triangle at 100 Hz peak at the level and have the ideal fundamental (" + report.trimEnd() + ")");

        Generator::Tone saw;
        saw.wave = Generator::Wave::sawtooth;
        saw.frequency = saw.endFrequency = 5000.0;
        saw.fadeMs = 0.0;
        const auto s = Generator::tone (saw, 1, (int) rate, rate);
        const auto alias = db (amplitude (s, 0, 18000.0, 0, (int) rate) / amplitude (s, 0, 5000.0, 0, (int) rate));
        check (alias < -25.0,
               "tones: a 5 kHz sawtooth's sixth harmonic, folded back to 18 kHz, is " + r (alias) + " dB against the fundamental, against -15.6 drawn straight");

        Generator::Tone sweep;
        sweep.frequency = 100.0;
        sweep.endFrequency = 1600.0;
        const auto sw = Generator::tone (sweep, 1, (int) (4 * rate), rate);
        const auto crossings = [&sw] (double centre)
        {
            const auto from = (int) ((centre - 0.05) * rate), to = (int) ((centre + 0.05) * rate);
            int n = 0;

            for (int i = from + 1; i < to; ++i)
                n += (sw.getSample (0, i - 1) < 0.0f) != (sw.getSample (0, i) < 0.0f) ? 1 : 0;

            return n / 0.2;   // two crossings a cycle, in 0.1 s
        };
        const auto atOne = crossings (1.0), atThree = crossings (3.0);
        const auto expected = [] (double t) { return 100.0 * std::exp2 (4.0 * t * rate / (4 * rate - 1)); };
        check (std::abs (atOne / expected (1.0) - 1.0) < 0.05 && std::abs (atThree / expected (3.0) - 1.0) < 0.05,
               "tones: a sweep from 100 to 1600 Hz over 4 s is at " + r (atOne, 0) + " Hz after 1 s and " + r (atThree, 0)
                   + " Hz after 3 s (" + r (expected (1.0), 0) + " and " + r (expected (3.0), 0) + " by octaves)");

        Generator::Tone bad = {}, loud = {}, low = {};
        bad.frequency = 24000.0;
        loud.levelDb = 1.0;
        low.endFrequency = 0.5;
        check (! Generator::isValid (bad, rate) && ! Generator::isValid (loud, rate) && ! Generator::isValid (low, rate)
                   && Generator::isValid (Generator::Tone {}, rate),
               "tones: half the sample rate, above 0 dBFS and below 1 Hz are refused");
    }

    void testNoise()
    {
        struct Expect { Generator::Colour colour; double slope; const char* name; };
        const Expect colours[] { { Generator::Colour::white, 10.0, "white" }, { Generator::Colour::pink, 0.0, "pink" },
                                 { Generator::Colour::brown, -10.0, "brown" } };
        juce::String report;
        bool right = true;

        for (auto& e : colours)
        {
            Generator::Noise n;
            n.colour = e.colour;
            n.levelDb = Generator::maxLevelDb (e.colour);
            n.fadeMs = 0.0;
            const auto b = Generator::noise (n, 2, (int) (10 * rate), rate);
            const auto level = db (b.getRMSLevel (0, 0, b.getNumSamples()));
            const auto peak = db (juce::jmax (b.getMagnitude (0, 0, b.getNumSamples()), b.getMagnitude (1, 0, b.getNumSamples())));
            const auto tilt = 10.0 * std::log10 (bandPower (b, 0, 2000.0, 4000.0) / bandPower (b, 0, 200.0, 400.0));
            double cross = 0.0;

            for (int i = 0; i < b.getNumSamples(); ++i)
                cross += (double) b.getSample (0, i) * b.getSample (1, i);

            const auto correlation = cross / (b.getNumSamples() * juce::square ((double) b.getRMSLevel (0, 0, b.getNumSamples())));
            right = right && std::abs (level - n.levelDb) < 0.01 && peak < -1.0 && std::abs (tilt - e.slope) < 1.0 && std::abs (correlation) < 0.1;
            report << e.name << " at " << r (n.levelDb, 0) << " dBFS: " << r (tilt) << " dB, peak " << r (peak) << " dBFS, correlation " << r (correlation, 3) << "; ";
        }

        check (right, "noise: 10 s at the loudest level allowed, exactly that RMS on both channels, 2-4 kHz against 200-400 Hz +10 dB for white, "
                      "0 for pink and -10 for brown, each channel its own and every peak below -1 dBFS (" + report.trimEnd() + ")");

        Generator::Noise n;
        const auto a = Generator::noise (n, 1, 4800, rate), again = Generator::noise (n, 1, 4800, rate);
        n.seed = 2;
        const auto other = Generator::noise (n, 1, 4800, rate);
        bool same = true, differs = false;

        for (int i = 0; i < 4800; ++i)
        {
            same = same && juce::exactlyEqual (a.getSample (0, i), again.getSample (0, i));
            differs = differs || ! juce::exactlyEqual (a.getSample (0, i), other.getSample (0, i));
        }

        Generator::Noise loud;
        loud.levelDb = -5.0;
        Generator::Noise pink;
        pink.colour = Generator::Colour::pink;
        pink.levelDb = -19.0;
        check (same && differs && ! Generator::isValid (loud) && ! Generator::isValid (pink),
               "noise: the same seed gives the same noise and another seed other noise; white above -6 dBFS RMS and pink above -20 are refused");
    }

    void testInsert()
    {
        juce::AudioBuffer<float> original (2, 1000);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 1000; ++i)
                original.setSample (ch, i, (float) (ch + 1) * 0.001f * (float) i);

        const auto file = AudioEdits::fromBuffer (juce::AudioBuffer<float> (original));
        Generator::Tone t;
        t.fadeMs = 0.0;
        const auto tone = Generator::tone (t, 2, 300, rate);
        const auto out = Generator::insert (*file, 400, tone);
        juce::AudioBuffer<float> got (2, (int) out->getNumFrames());
        out->read (got, 0, 0, got.getNumSamples());
        bool right = out->getNumFrames() == 1300;

        for (int ch = 0; ch < 2 && right; ++ch)
            for (int i = 0; i < 1300; ++i)
            {
                const auto want = i < 400 ? original.getSample (ch, i) : i < 700 ? tone.getSample (ch, i - 400) : original.getSample (ch, i - 300);
                right = right && juce::exactlyEqual (got.getSample (ch, i), want);
            }

        check (right && Generator::insert (*file, 0, Generator::tone (t, 1, 300, rate)) == nullptr,
               "generate: put in at a point, the generated audio is there bit for bit, with what was before it and after it unchanged; "
               "a channel count unlike the file's is refused");
    }
}

void runGeneratorTests()
{
    testTones();
    testNoise();
    testInsert();
}
