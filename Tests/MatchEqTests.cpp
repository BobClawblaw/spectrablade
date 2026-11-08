#include <JuceHeader.h>
#include "../Source/MatchEq.h"
#include "TestUtils.h"
#include "MatchEqTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    /** Four seconds of noise, the same every time, on two channels. */
    juce::AudioBuffer<float> noise (int seed)
    {
        juce::Random r (seed);
        juce::AudioBuffer<float> b (2, 4 * 48000);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (ch, i, (r.nextFloat() - 0.5f) * 0.5f);

        return b;
    }

    AudioEdits::Snapshot snapshotOf (const juce::AudioBuffer<float>& b)
    {
        return AudioEdits::fromBuffer (juce::AudioBuffer<float> (b));
    }

    /** The profile's level at a frequency, between its points. */
    double levelAt (const MatchEq::Profile& p, double hz)
    {
        return MatchEq::correctionAt (p.levelDb, hz);
    }
}

void runMatchEqTests()
{
    std::cout << "Match EQ:" << std::endl;

    // A reference is white noise through a +6 dB low shelf at 200 Hz and a -6 dB bell at 2 kHz; the
    // sound is other white noise, flat. The correction is the shelf and the bell (with the mean
    // taken out); applied, the sound's profile comes within 1 dB of the reference's from 50 Hz to
    // 15 kHz; at 50 % it goes half way; a narrow feature is smoothed away at 2 octaves; the most
    // caps it; a reference at another rate, a short range and bad settings are refused.
    {
        const std::vector<Equalizer::Band> shape { { Equalizer::BandType::lowShelf, 200.0, 6.0, 0.7071067811865476 }, { Equalizer::BandType::peak, 2000.0, -6.0, 1.0 } };
        const auto referenceAudio = Equalizer::apply (*snapshotOf (noise (1)), 0, 4 * 48000, rate, shape);
        const auto soundAudio = snapshotOf (noise (2));
        const auto reference = MatchEq::measure (*referenceAudio, 0, 4 * 48000, rate);
        const auto sound = MatchEq::measure (*soundAudio, 0, 4 * 48000, rate);

        MatchEq::Settings s;
        const auto correction = MatchEq::correctionDb (*reference, *sound, s);
        const auto wanted = [&shape] (double hz)
        {
            auto db = 0.0;

            for (auto& b : shape)
                db += Equalizer::responseDb (b, rate, hz);

            return db;
        };

        // Against the bands' curve, as differences between frequencies (the mean is taken out).
        const auto shelf = MatchEq::correctionAt (correction, 60.0) - MatchEq::correctionAt (correction, 800.0);
        const auto bell = MatchEq::correctionAt (correction, 2000.0) - MatchEq::correctionAt (correction, 800.0);
        const auto wantedShelf = wanted (60.0) - wanted (800.0), wantedBell = wanted (2000.0) - wanted (800.0);

        const auto matched = MatchEq::apply (*soundAudio, 0, 4 * 48000, rate, *reference, s);
        const auto after = MatchEq::measure (*matched, 0, 4 * 48000, rate);
        auto worst = 0.0;

        for (auto hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 15000.0 })
        {
            // The match keeps the level, so compare shapes: each point less the 1 kHz point.
            const auto got = levelAt (*after, hz) - levelAt (*after, 1000.0), want = levelAt (*reference, hz) - levelAt (*reference, 1000.0);
            worst = juce::jmax (worst, std::abs (got - want));
        }

        // The correction's mean over 100 Hz to 10 kHz is taken out; the sound's overall level moves
        // only as its energy sits where the correction is not zero.
        auto meanCorrection = 0.0;
        auto n = 0;

        for (int i = 0; i < MatchEq::numPoints; ++i)
            if (const auto hz = MatchEq::frequencyOf (i); hz >= 100.0 && hz <= 10000.0)
            {
                meanCorrection += correction[(size_t) i];
                ++n;
            }

        meanCorrection /= n;
        const auto rmsOf = [] (const AudioSnapshot& x)
        {
            juce::AudioBuffer<float> b (x.getNumChannels(), (int) x.getNumFrames());
            x.read (b, 0, 0, b.getNumSamples());
            return 20.0 * std::log10 (b.getRMSLevel (0, 0, b.getNumSamples()));
        };
        const auto level = rmsOf (*matched) - rmsOf (*soundAudio);

        MatchEq::Settings half;
        half.amountPercent = 50.0;
        const auto halfCorrection = MatchEq::correctionDb (*reference, *sound, half);
        const auto halfShelf = MatchEq::correctionAt (halfCorrection, 60.0) - MatchEq::correctionAt (halfCorrection, 800.0);

        MatchEq::Settings broad;
        broad.smoothingOctaves = 2.0;
        const auto broadCorrection = MatchEq::correctionDb (*reference, *sound, broad);
        const auto broadBell = MatchEq::correctionAt (broadCorrection, 2000.0) - MatchEq::correctionAt (broadCorrection, 800.0);

        MatchEq::Settings capped;
        capped.mostDb = 2.0;
        const auto cappedCorrection = MatchEq::correctionDb (*reference, *sound, capped);
        auto withinCap = true;

        for (auto c : cappedCorrection)
            withinCap = withinCap && std::abs (c) <= 2.0 + 1.0e-9;

        auto otherRate = *reference;
        otherRate.sampleRate = 44100.0;
        MatchEq::Settings tooMuch;
        tooMuch.amountPercent = 150.0;
        const auto refused = MatchEq::apply (*soundAudio, 0, 4 * 48000, rate, otherRate, s) == nullptr && MatchEq::apply (*soundAudio, 0, 4000, rate, *reference, s) == nullptr
                             && MatchEq::apply (*soundAudio, 0, 4 * 48000, rate, *reference, tooMuch) == nullptr && ! MatchEq::measure (*soundAudio, 0, 4000, rate).has_value();

        std::cout << "      correction: shelf " << shelf << " dB (wanted " << wantedShelf << "), bell " << bell << " (wanted " << wantedBell << "); matched within " << worst
                  << " dB; the correction's mean over 100 Hz to 10 kHz " << meanCorrection << " dB, the sound's RMS moved " << level << " dB; at 50 % the shelf "
                  << halfShelf << "; smoothed over 2 octaves the bell " << broadBell << std::endl;
        check (std::abs (shelf - wantedShelf) < 1.0 && std::abs (bell - wantedBell) < 1.0 && worst < 1.0 && std::abs (meanCorrection) < 1.0e-9 && std::abs (level) < 2.0
                   && std::abs (halfShelf - wantedShelf / 2.0) < 0.6 && std::abs (broadBell) < std::abs (bell) / 2.0 && withinCap && refused,
               "match eq: against a reference shaped by a +6 dB shelf at 200 Hz and a -6 dB bell at 2 kHz, the correction holds the shelf and the "
               "bell within 1 dB; applied, the sound's profile comes within 1 dB of the reference's from 50 Hz to 15 kHz; the correction's mean over "
               "100 Hz to 10 kHz is zero and white noise's RMS moves under 2 dB; at 50 % the shelf goes half way; smoothed over 2 octaves the bell is "
               "more than halved; a most of 2 dB caps every point; a reference at another rate, a range under a window and an amount past 100 % are refused");
    }
}
