#include <JuceHeader.h>
#include "../Source/AdaptiveDenoise.h"
#include "../Source/AudioEdits.h"
#include "TestUtils.h"
#include "AdaptiveDenoiseTests.h"

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

    /** Six seconds: noise at -40 dB for the first three and -28 dB for the last three, with a
        voice-like buzz (150 Hz, twenty harmonics) from 1 to 2 s and from 4 to 5 s. */
    juce::AudioBuffer<float> scene (bool withVoice)
    {
        juce::Random r (11);
        juce::AudioBuffer<float> b (1, 6 * 48000);

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const auto t = i / rate;
            auto v = (t < 3.0 ? 0.01 : 0.04) * (r.nextDouble() * 2.0 - 1.0);

            if (withVoice && ((t >= 1.0 && t < 2.0) || (t >= 4.0 && t < 5.0)))
                for (int h = 1; h <= 20; ++h)
                    v += 0.25 / h * std::sin (juce::MathConstants<double>::twoPi * 150.0 * h * t);

            b.setSample (0, i, (float) v);
        }

        return b;
    }

    double rmsDb (const juce::AudioBuffer<float>& b, int from, int to)
    {
        double sum = 0.0;

        for (int i = from; i < to; ++i)
            sum += (double) b.getSample (0, i) * b.getSample (0, i);

        return 10.0 * std::log10 (juce::jmax (1.0e-20, sum / (to - from)));
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
}

void runAdaptiveDenoiseTests()
{
    std::cout << "Adaptive de-noise:" << std::endl;

    // Noise that steps up 12 dB halfway: the gaps before and after each word come down by about the
    // reduction in both halves (the estimate has followed the step within two seconds), the voice's
    // harmonics stay, 0 dB leaves the audio as it was, it can be cancelled, bad settings are refused.
    {
        const auto in = scene (true);
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        AdaptiveDenoise::Settings s;
        s.reductionDb = 12.0f;
        const auto out = readAll (*AdaptiveDenoise::reduce (*snap, 0, in.getNumSamples(), rate, s));
        const auto quietGap = rmsDb (out, 110000, 140000) - rmsDb (in, 110000, 140000);        // 2.3 to 2.9 s: quiet noise, after the first word
        const auto loudGap = rmsDb (out, 254000, 284000) - rmsDb (in, 254000, 284000);         // 5.3 to 5.9 s: loud noise, after the second word
        const auto justAfterStep = rmsDb (out, 150000, 170000) - rmsDb (in, 150000, 170000);   // 3.1 to 3.5 s: the loud noise just after it began
        auto worstHarmonic = 0.0;

        for (int h = 1; h <= 8; ++h)
            for (auto from : { 52000, 196000 })
            {
                const auto moved = 20.0 * std::log10 (amplitude (out, 150.0 * h, from, 24000) / amplitude (in, 150.0 * h, from, 24000));
                worstHarmonic = juce::jmax (worstHarmonic, std::abs (moved));
            }

        AdaptiveDenoise::Settings none;
        none.reductionDb = 0.0f;
        const auto untouched = readAll (*AdaptiveDenoise::reduce (*snap, 0, in.getNumSamples(), rate, none));
        auto worstUntouched = 0.0;

        for (int i = 0; i < in.getNumSamples(); ++i)
            worstUntouched = juce::jmax (worstUntouched, (double) std::abs (untouched.getSample (0, i) - in.getSample (0, i)));

        const auto cancelled = AdaptiveDenoise::reduce (*snap, 0, in.getNumSamples(), rate, s, [] (float) { return false; }) == nullptr;
        AdaptiveDenoise::Settings bad;
        bad.trackingSeconds = 10.0f;
        const auto refused = AdaptiveDenoise::reduce (*snap, 0, in.getNumSamples(), rate, bad) == nullptr && AdaptiveDenoise::reduce (*snap, 100, 100, rate, s) == nullptr;

        std::cout << "      noise stepping up 12 dB: the quiet gap " << quietGap << " dB, the loud gap " << loudGap << ", just after the step " << justAfterStep
                  << "; the harmonics moved at most " << worstHarmonic << " dB; at 0 dB the audio within " << worstUntouched << std::endl;
        check (quietGap < -9.0 && loudGap < -9.0 && worstHarmonic < 1.5 && worstUntouched < 1.0e-5 && cancelled && refused,
               "adaptive de-noise: with noise stepping up 12 dB halfway and a 12 dB reduction, the gaps after each word come down over 9 dB in both "
               "halves (the noise followed without a print), the voice's first eight harmonics stay within 1.5 dB, 0 dB leaves the audio within 1e-5, "
               "it can be cancelled, a tracking span past 5 s and an empty range are refused");
    }
}
