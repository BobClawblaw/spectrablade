#include <JuceHeader.h>
#include "../Source/TonalBalance.h"
#include "TestUtils.h"
#include "TonalBalanceTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    /** A window's spectrum of noise with this slope in dB an octave (0 for white), the same every time. */
    std::vector<float> noiseSpectrum (double slopeDbPerOctave, int seed)
    {
        juce::Random r (seed);
        std::vector<float> white ((size_t) Analysis::fftSize);

        for (auto& v : white)
            v = (r.nextFloat() - 0.5f) * 0.5f;

        auto db = Analysis::spectrumDb (white.data(), Analysis::fftSize);

        for (size_t k = 1; k < db.size(); ++k)
            db[k] += (float) (slopeDbPerOctave * std::log2 ((double) k * rate / Analysis::fftSize / 1000.0));

        return db;
    }
}

void runTonalBalanceTests()
{
    std::cout << "Tonal balance:" << std::endl;

    // Pink noise (falling 3 dB an octave) sits within the Full mix target from 50 Hz to 10 kHz;
    // white noise (flat) leaves it, bright at the top; the targets are relative (zero mean over
    // 100 Hz to 10 kHz) and the Speech target is narrower at the ends than the Full mix.
    {
        TonalBalance::Panel panel;
        panel.setSize (420, 220);

        for (int i = 0; i < 40; ++i)   // enough windows for the average to settle
            panel.take (noiseSpectrum (-3.0, 100 + i), rate);

        const auto full = TonalBalance::targetFor (TonalBalance::Style::fullMix);
        const auto pinkShare = panel.getShareWithin();
        auto pinkMiddle = true;

        for (int i = 0; i < MatchEq::numPoints; ++i)
            if (const auto hz = MatchEq::frequencyOf (i); hz >= 50.0 && hz <= 10000.0)
                pinkMiddle = pinkMiddle && panel.getRelative()[(size_t) i] >= full.low[(size_t) i] && panel.getRelative()[(size_t) i] <= full.high[(size_t) i];

        TonalBalance::Panel white;

        for (int i = 0; i < 40; ++i)
            white.take (noiseSpectrum (0.0, 200 + i), rate);

        const auto whiteShare = white.getShareWithin();
        const auto& w = white.getRelative();
        const auto topPoint = (size_t) (MatchEq::pointsPerOctave * 9);   // 10,240 Hz
        const auto brightAtTop = w[topPoint] > full.high[topPoint];

        auto mean = 0.0;
        auto n = 0;

        for (int i = 0; i < MatchEq::numPoints; ++i)
            if (const auto hz = MatchEq::frequencyOf (i); hz >= 100.0 && hz <= 10000.0)
            {
                mean += (full.low[(size_t) i] + full.high[(size_t) i]) / 2.0;
                ++n;
            }

        mean /= n;
        const auto speech = TonalBalance::targetFor (TonalBalance::Style::speech);
        const auto lowPoint = (size_t) (MatchEq::pointsPerOctave * 1);   // 40 Hz
        const auto narrower = speech.high[lowPoint] < full.high[lowPoint] && speech.high[topPoint] < full.high[topPoint];

        panel.setStyle (TonalBalance::Style::speech);
        const auto switched = panel.getStyle() == TonalBalance::Style::speech && panel.getShareWithin() < pinkShare;

        std::cout << "      pink noise within the Full mix target: " << juce::roundToInt (pinkShare * 100.0) << " %; white noise: " << juce::roundToInt (whiteShare * 100.0)
                  << " %, " << w[topPoint] << " dB at 10 kHz against a top of " << full.high[topPoint] << std::endl;
        check (pinkMiddle && pinkShare > 0.9 && whiteShare < pinkShare && brightAtTop && std::abs (mean) < 1.0e-9 && narrower && switched,
               "tonal balance: pink noise sits within the Full mix target from 50 Hz to 10 kHz (over 90 % of the range within); white noise is outside "
               "at the top (10 kHz above the band's top) with less within; the target's mean over 100 Hz to 10 kHz is zero; the Speech target is "
               "narrower at 40 Hz and 10 kHz; changing the style to Speech puts less of the pink noise within");
    }
}
