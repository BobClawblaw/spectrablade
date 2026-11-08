#include <JuceHeader.h>
#include "../Source/Rebalance.h"
#include "../Source/AudioEdits.h"
#include "TestUtils.h"
#include "RebalanceTests.h"

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

    /** Four seconds of a stereo mix: a voice-like buzz in the middle (180 Hz, ten harmonics, from 1
        to 3 s), a bass tone in the middle (55 Hz, throughout), drum hits (20 ms bursts of noise every
        250 ms), and a pad on the sides only (steady tones at 1,100 and 1,375 Hz, left against right). */
    juce::AudioBuffer<float> mix()
    {
        juce::Random r (29);
        juce::AudioBuffer<float> b (2, 4 * 48000);

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const auto t = i / rate;
            auto mid = 0.15 * std::sin (juce::MathConstants<double>::twoPi * 55.0 * t);

            if (t >= 1.0 && t < 3.0)
                for (int h = 1; h <= 10; ++h)
                    mid += 0.1 / h * std::sin (juce::MathConstants<double>::twoPi * 180.0 * h * t);

            if (std::fmod (t, 0.25) < 0.02)
                mid += 0.3 * (r.nextDouble() * 2.0 - 1.0);

            const auto side = 0.05 * std::sin (juce::MathConstants<double>::twoPi * 1100.0 * t) + 0.05 * std::sin (juce::MathConstants<double>::twoPi * 1375.0 * t);
            b.setSample (0, i, (float) (mid + side));
            b.setSample (1, i, (float) (mid - side));
        }

        return b;
    }

    double amplitude (const juce::AudioBuffer<float>& b, double hz, int from, int length, bool mid)
    {
        double re = 0.0, im = 0.0;

        for (int i = from; i < from + length; ++i)
        {
            const auto v = 0.5 * (b.getSample (0, i) + (mid ? 1.0 : -1.0) * b.getSample (1, i));
            const auto phase = juce::MathConstants<double>::twoPi * hz * i / rate;
            re += v * std::cos (phase);
            im += v * std::sin (phase);
        }

        return 2.0 * std::sqrt (re * re + im * im) / length;
    }

    /** The hits' own power in the mid over [from, to): the mean square during the hits (the first
        20 ms of each 250 ms) less the mean square between them, in dB. */
    double hitsDb (const juce::AudioBuffer<float>& b, int from, int to)
    {
        auto during = 0.0, between = 0.0;
        auto nDuring = 0, nBetween = 0;

        for (int i = from; i < to; ++i)
        {
            const auto v = 0.5 * (b.getSample (0, i) + b.getSample (1, i));

            if (std::fmod (i / rate, 0.25) < 0.02)
            {
                during += v * v;
                ++nDuring;
            }
            else
            {
                between += v * v;
                ++nBetween;
            }
        }

        return 10.0 * std::log10 (juce::jmax (1.0e-20, during / nDuring - between / nBetween));
    }
}

void runRebalanceTests()
{
    std::cout << "Rebalance:" << std::endl;

    // Every stem at 0 dB gives the mix back; the voice down takes the buzz's harmonics down and
    // leaves the bass, the hits and the pad; the bass down takes the 55 Hz down; the drums down
    // take the hits down; the rest down takes the pad on the sides down; refused; cancelling.
    {
        const auto in = mix();
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        Rebalance::Settings flat, voiceDown, bassDown, drumsDown, restDown;
        voiceDown.voiceDb = -24.0;
        bassDown.bassDb = -24.0;
        drumsDown.drumsDb = -24.0;
        restDown.restDb = -24.0;
        const auto same = readAll (*Rebalance::apply (*snap, 0, in.getNumSamples(), rate, flat));
        auto worstSame = 0.0;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < in.getNumSamples(); ++i)
                worstSame = juce::jmax (worstSame, (double) std::abs (same.getSample (ch, i) - in.getSample (ch, i)));

        const auto v = readAll (*Rebalance::apply (*snap, 0, in.getNumSamples(), rate, voiceDown));
        const auto bs = readAll (*Rebalance::apply (*snap, 0, in.getNumSamples(), rate, bassDown));
        const auto d = readAll (*Rebalance::apply (*snap, 0, in.getNumSamples(), rate, drumsDown));
        const auto rs = readAll (*Rebalance::apply (*snap, 0, in.getNumSamples(), rate, restDown));
        const auto dB = [] (double a, double b) { return 20.0 * std::log10 (a / b); };
        const int from = (int) (1.5 * 48000), length = 48000;   // 1.5 to 2.5 s: the voice is on

        auto voiceHarmonics = 0.0;   // the mean change of the buzz's first six harmonics

        for (int h = 1; h <= 6; ++h)
            voiceHarmonics += dB (amplitude (v, 180.0 * h, from, length, true), amplitude (in, 180.0 * h, from, length, true)) / 6.0;

        const auto bassUnderVoiceDown = dB (amplitude (v, 55.0, from, length, true), amplitude (in, 55.0, from, length, true));
        const auto padUnderVoiceDown = dB (amplitude (v, 1100.0, from, length, false), amplitude (in, 1100.0, from, length, false));
        const auto hitsUnderVoiceDown = hitsDb (v, from, from + length) - hitsDb (in, from, from + length);
        const auto bass = dB (amplitude (bs, 55.0, from, length, true), amplitude (in, 55.0, from, length, true));
        auto voiceUnderBassDown = 0.0;

        for (int h = 2; h <= 6; ++h)
            voiceUnderBassDown += dB (amplitude (bs, 180.0 * h, from, length, true), amplitude (in, 180.0 * h, from, length, true)) / 5.0;

        const auto hits = hitsDb (d, from, from + length) - hitsDb (in, from, from + length);
        const auto bassUnderDrumsDown = dB (amplitude (d, 55.0, from, length, true), amplitude (in, 55.0, from, length, true));
        const auto pad = dB (amplitude (rs, 1100.0, from, length, false), amplitude (in, 1100.0, from, length, false));
        auto voiceUnderRestDown = 0.0;

        for (int h = 1; h <= 6; ++h)
            voiceUnderRestDown += dB (amplitude (rs, 180.0 * h, from, length, true), amplitude (in, 180.0 * h, from, length, true)) / 6.0;

        Rebalance::Settings bad;
        bad.voiceDb = 20.0;
        const auto refused = Rebalance::apply (*snap, 0, in.getNumSamples(), rate, bad) == nullptr && Rebalance::apply (*snap, 0, 1000, rate, flat) == nullptr;
        const auto cancelled = Rebalance::apply (*snap, 0, in.getNumSamples(), rate, voiceDown, [] (float) { return false; }) == nullptr;

        std::cout << "      all at 0 dB: within " << worstSame << "; voice -24: the buzz's harmonics " << voiceHarmonics << " dB, the bass " << bassUnderVoiceDown << ", the pad "
                  << padUnderVoiceDown << ", the hits " << hitsUnderVoiceDown << "; bass -24: the 55 Hz " << bass << " dB, the buzz " << voiceUnderBassDown << "; drums -24: the hits "
                  << hits << " dB, the bass " << bassUnderDrumsDown << "; rest -24: the pad " << pad << " dB, the buzz " << voiceUnderRestDown << std::endl;
        check (worstSame < 1.0e-5 && voiceHarmonics < -12.0 && std::abs (bassUnderVoiceDown) < 1.0 && std::abs (padUnderVoiceDown) < 1.0 && hitsUnderVoiceDown > -3.0
                   && bass < -15.0 && std::abs (voiceUnderBassDown) < 1.0 && hits < -8.0 && std::abs (bassUnderDrumsDown) < 1.5 && pad < -15.0 && std::abs (voiceUnderRestDown) < 1.5
                   && refused && cancelled,
               "rebalance: every stem at 0 dB gives the mix back within 1e-5; the voice at -24 dB takes a buzz's harmonics down over 12 dB and leaves the "
               "bass and the pad on the sides within 1 dB and the drum hits within 3 dB; the bass at -24 takes 55 Hz down over 15 dB and leaves the buzz "
               "within 1 dB; the drums at -24 take the hits down over 8 dB and leave the bass within 1.5 dB; the rest at -24 takes the pad down over 15 dB "
               "and leaves the buzz within 1.5 dB; a gain past 12 dB and a range under a window are refused; it can be cancelled");
    }
}
