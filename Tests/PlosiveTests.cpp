#include <JuceHeader.h>
#include "../Source/Plosive.h"
#include "TestUtils.h"
#include "PlosiveTests.h"

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

    /** Three seconds of a voice-like buzz (180 Hz, twelve harmonics) with a 60 ms thump of 50 Hz at
        1.0 s and another at 2.0 s, over a steady 80 Hz hum at -30 dB. */
    juce::AudioBuffer<float> speech (bool withThumps)
    {
        juce::AudioBuffer<float> b (1, 3 * 48000);

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const auto t = i / rate;
            auto v = 0.03 * std::sin (juce::MathConstants<double>::twoPi * 80.0 * t);

            for (int h = 1; h <= 12; ++h)
                v += 0.15 / h * std::sin (juce::MathConstants<double>::twoPi * 180.0 * h * t);

            for (auto at : { 1.0, 2.0 })
                if (withThumps && t >= at && t < at + 0.06)
                    v += 0.6 * std::sin (juce::MathConstants<double>::twoPi * 50.0 * (t - at)) * std::exp (-(t - at) / 0.03);

            b.setSample (0, i, (float) v);
        }

        return b;
    }

    double amplitude (const juce::AudioBuffer<float>& b, double hz, int from, int length)
    {
        double re = 0.0, im = 0.0;

        for (int i = from; i < from + length; ++i)
        {
            const auto phase = juce::MathConstants<double>::twoPi * hz * i / rate;
            re += b.getSample (0, i) * std::cos (phase);
            im += b.getSample (0, i) * std::sin (phase);
        }

        return 2.0 * std::sqrt (re * re + im * im) / length;
    }

    double rmsDb (const juce::AudioBuffer<float>& b, int from, int to)
    {
        double sum = 0.0;

        for (int i = from; i < to; ++i)
            sum += (double) b.getSample (0, i) * b.getSample (0, i);

        return 10.0 * std::log10 (juce::jmax (1.0e-20, sum / (to - from)));
    }
}

void runPlosiveTests()
{
    std::cout << "De-plosive:" << std::endl;

    // Each thump's 50 Hz comes down by about the reduction; the voice's harmonics and the steady hum
    // away from the thumps stay; a voice without thumps passes within 0.1 dB; refused settings.
    {
        const auto in = speech (true), clean = speech (false);
        Plosive::Settings s;
        s.reductionDb = 18.0;
        const auto out = readAll (*Plosive::apply (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (in)), 0, in.getNumSamples(), rate, s));
        const auto thump1 = 20.0 * std::log10 (amplitude (out, 50.0, 48000, 2880) / amplitude (in, 50.0, 48000, 2880));
        const auto thump2 = 20.0 * std::log10 (amplitude (out, 50.0, 96000, 2880) / amplitude (in, 50.0, 96000, 2880));
        auto worstHarmonic = 0.0;

        for (int h = 1; h <= 6; ++h)
            worstHarmonic = juce::jmax (worstHarmonic, std::abs (20.0 * std::log10 (amplitude (out, 180.0 * h, 60000, 24000) / amplitude (in, 180.0 * h, 60000, 24000))));

        const auto hum = 20.0 * std::log10 (amplitude (out, 80.0, 60000, 24000) / amplitude (in, 80.0, 60000, 24000));
        const auto cleanOut = readAll (*Plosive::apply (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (clean)), 0, clean.getNumSamples(), rate, s));
        const auto cleanKept = rmsDb (cleanOut, 24000, 144000) - rmsDb (clean, 24000, 144000);
        Plosive::Settings bad;
        bad.cornerHz = 30.0;
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        const auto refused = Plosive::apply (*snap, 0, 48000, rate, bad) == nullptr && Plosive::apply (*snap, 100, 100, rate, s) == nullptr;

        std::cout << "      thumps: " << thump1 << " and " << thump2 << " dB; the harmonics moved at most " << worstHarmonic << " dB, the hum " << hum << " dB; a clean voice " << cleanKept << " dB" << std::endl;
        check (thump1 < -12.0 && thump2 < -12.0 && worstHarmonic < 0.1 && std::abs (hum) < 0.1 && std::abs (cleanKept) < 0.1 && refused,
               "de-plosive: with an 18 dB reduction two 60 ms thumps of 50 Hz under a voice come down over 12 dB each, the voice's first six harmonics "
               "and the steady 80 Hz hum away from them move under 0.1 dB, a voice without thumps passes within 0.1 dB; a corner under 60 Hz and an "
               "empty range are refused");
    }
}
