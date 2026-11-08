#include <JuceHeader.h>
#include "../Source/SpectralRecovery.h"
#include "../Source/MatchEq.h"
#include "../Source/AudioEdits.h"
#include "TestUtils.h"
#include "SpectralRecoveryTests.h"

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

    /** Four seconds of a rich buzz: 110 Hz with harmonics falling 3 dB an octave up to 20 kHz, with
        a little noise 60 dB down. */
    juce::AudioBuffer<float> rich()
    {
        juce::Random r (23);
        juce::AudioBuffer<float> b (1, 4 * 48000);

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const auto t = i / rate;
            auto v = 0.0005 * (r.nextDouble() * 2.0 - 1.0);

            for (int h = 1; h * 110.0 < 20000.0; ++h)
                v += 0.02 / std::sqrt ((double) h) * std::sin (juce::MathConstants<double>::twoPi * 110.0 * h * t + h);

            b.setSample (0, i, (float) v);
        }

        return b;
    }

    /** The audio with everything above hz taken out (an 8,192-point brick wall, overlapped). */
    juce::AudioBuffer<float> bandLimited (const juce::AudioBuffer<float>& in, double hz)
    {
        constexpr int n = 8192, hop = n / 4;
        juce::AudioBuffer<float> out (1, in.getNumSamples());
        out.clear();
        juce::dsp::FFT fft (13);
        std::vector<float> data ((size_t) (2 * n)), w ((size_t) n);

        for (int i = 0; i < n; ++i)
            w[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / n));

        for (int pos = -n + hop; pos < in.getNumSamples(); pos += hop)
        {
            std::fill (data.begin(), data.end(), 0.0f);

            for (int i = 0; i < n; ++i)
                if (pos + i >= 0 && pos + i < in.getNumSamples())
                    data[(size_t) i] = in.getSample (0, pos + i) * w[(size_t) i];

            fft.performRealOnlyForwardTransform (data.data(), true);

            for (int k = (int) (hz / (rate / n)); k <= n / 2; ++k)
                data[(size_t) (2 * k)] = data[(size_t) (2 * k + 1)] = 0.0f;

            fft.performRealOnlyInverseTransform (data.data());

            for (int i = 0; i < n; ++i)
                if (pos + i >= 0 && pos + i < in.getNumSamples())
                    out.addSample (0, pos + i, data[(size_t) i] * w[(size_t) i] / 2.0f);
        }

        return out;
    }

    double levelAt (const MatchEq::Profile& p, double hz)
    {
        return MatchEq::correctionAt (p.levelDb, hz);
    }
}

void runSpectralRecoveryTests()
{
    std::cout << "Spectral recovery:" << std::endl;

    // A rich buzz cut off at 6 kHz: the cutoff is found; rebuilt, the band from 6 to 12 kHz comes
    // back within 6 dB of the original's profile where it was over 40 dB down, the band below the
    // cutoff is left within 0.05 dB, and audio that reaches the top has no cutoff to find.
    {
        const auto original = rich();
        const auto cut = bandLimited (original, 6000.0);
        const auto cutSnap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (cut));
        const auto found = SpectralRecovery::findCutoff (*cutSnap, 0, cut.getNumSamples(), rate);
        const auto none = SpectralRecovery::findCutoff (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (original)), 0, original.getNumSamples(), rate);

        SpectralRecovery::Settings s;
        s.levelDb = 0.0;
        const auto recovered = SpectralRecovery::recover (*cutSnap, 0, cut.getNumSamples(), rate, s);

        if (recovered == nullptr)
        {
            std::cout << "      cutoff found at " << found << " Hz; recover gave nothing" << std::endl;
            check (false, "spectral recovery: recover gave nothing for a buzz cut off at 6 kHz");
            return;
        }

        const auto rebuilt = readAll (*recovered);
        const auto pOriginal = MatchEq::measure (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (original)), 0, original.getNumSamples(), rate);
        const auto pCut = MatchEq::measure (*cutSnap, 0, cut.getNumSamples(), rate);
        const auto pRebuilt = MatchEq::measure (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (rebuilt)), 0, rebuilt.getNumSamples(), rate);
        auto worstAbove = 0.0, worstBelow = 0.0, missingBefore = -200.0;

        for (auto hz : { 7000.0, 8000.0, 9500.0, 11000.0 })
        {
            worstAbove = juce::jmax (worstAbove, std::abs (levelAt (*pRebuilt, hz) - levelAt (*pOriginal, hz)));
            missingBefore = juce::jmax (missingBefore, levelAt (*pCut, hz) - levelAt (*pOriginal, hz));
        }

        for (auto hz : { 500.0, 1000.0, 2000.0, 4000.0, 5500.0 })
            worstBelow = juce::jmax (worstBelow, std::abs (levelAt (*pRebuilt, hz) - levelAt (*pCut, hz)));

        SpectralRecovery::Settings bad;
        bad.highestHz = 30000.0;
        const auto refused = SpectralRecovery::recover (*cutSnap, 0, cut.getNumSamples(), rate, bad) == nullptr && SpectralRecovery::recover (*cutSnap, 0, 1000, rate, s) == nullptr;
        const auto cancelled = SpectralRecovery::recover (*cutSnap, 0, cut.getNumSamples(), rate, s, [] (float) { return false; }) == nullptr;

        std::cout << "      cutoff found at " << found << " Hz (none in the full audio: " << none << "); from 7 to 11 kHz the cut audio was " << missingBefore
                  << " dB under the original, rebuilt it is within " << worstAbove << " dB; below the cutoff moved " << worstBelow << " dB" << std::endl;
        check (found > 5500.0 && found < 6500.0 && juce::exactlyEqual (none, 0.0) && missingBefore < -40.0 && worstAbove < 6.0 && worstBelow < 0.05 && refused && cancelled,
               "spectral recovery: a buzz cut off at 6 kHz has its cutoff found within 500 Hz (and audio reaching the top has none); the band from 7 to "
               "11 kHz, over 40 dB down, comes back within 6 dB of the original's profile; the band below the cutoff moves under 0.05 dB; a top past "
               "half the rate and a range under a window are refused; it can be cancelled");
    }
}
