#include <JuceHeader.h>
#include "../Source/Spill.h"
#include "../Source/AudioEdits.h"
#include "TestUtils.h"
#include "SpillTests.h"

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

    /** Twelve seconds of speech-like material: words (a 160 Hz buzz at -20 dB, 400 ms) at 0.5, 1.5,
        2.5, 3.5, 8.5, 9.5, 10.5 and 11.5 s; a gust of wind (low rumble at -26 dB, from 5 to 7 s); a
        rustle (bright noise at -30 dB, 150 ms at 4.6 s); bleed (a 700 Hz tone at -50 dB and noise
        at -56 dB, throughout). */
    juce::AudioBuffer<float> scene()
    {
        juce::Random r (17);
        juce::AudioBuffer<float> b (1, 12 * 48000);
        auto rumble = 0.0, rumbleFirst = 0.0, lastWhite = 0.0;

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const auto t = i / rate;
            auto v = 0.0032 * std::sin (juce::MathConstants<double>::twoPi * 700.0 * t) + 0.0016 * (r.nextDouble() * 2.0 - 1.0);
            const auto word = t >= 0.5 && (t < 4.0 || t >= 8.5) && std::fmod (t - 0.5, 1.0) < 0.4;

            if (word)
                for (int h = 1; h <= 10; ++h)
                    v += 0.1 / h * std::sin (juce::MathConstants<double>::twoPi * 160.0 * h * t);

            // Wind: white noise through two one-pole low-passes at 40 Hz (12 dB an octave), gusting up and down.
            rumbleFirst = 0.9948 * rumbleFirst + 0.0052 * (r.nextDouble() * 2.0 - 1.0);
            rumble = 0.9948 * rumble + 0.0052 * rumbleFirst;

            if (t >= 5.0 && t < 7.0)
                v += 60.0 * rumble * (0.6 + 0.4 * std::sin (juce::MathConstants<double>::twoPi * 0.7 * t));

            if (t >= 4.6 && t < 4.75)
            {
                const auto white = r.nextDouble() * 2.0 - 1.0;
                v += 0.025 * (white - lastWhite);
                lastWhite = white;
            }

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

void runSpillTests()
{
    std::cout << "Wind, rustle and bleed:" << std::endl;

    // Each job on its own: the gust's lows lowered, the rustle's highs lowered, the bleed between
    // words lowered; the words' harmonics kept by all three; cancelling; refused settings.
    {
        const auto in = scene();
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        Spill::Settings onlyWind, onlyRustle, onlyBleed;
        onlyWind.rustleReductionDb = onlyWind.bleedReductionDb = 0.0;
        onlyRustle.windReductionDb = onlyRustle.bleedReductionDb = 0.0;
        onlyBleed.windReductionDb = onlyBleed.rustleReductionDb = 0.0;
        const auto wind = readAll (*Spill::apply (*snap, 0, in.getNumSamples(), rate, onlyWind));
        const auto rustle = readAll (*Spill::apply (*snap, 0, in.getNumSamples(), rate, onlyRustle));
        const auto bleed = readAll (*Spill::apply (*snap, 0, in.getNumSamples(), rate, onlyBleed));

        const auto at = [] (double seconds) { return (int) std::lround (seconds * rate); };
        const auto gust = rmsDb (wind, at (5.5), at (6.5)) - rmsDb (in, at (5.5), at (6.5));   // the gust is the loudest thing there
        const auto rustleBurst = rmsDb (rustle, at (4.62), at (4.73)) - rmsDb (in, at (4.62), at (4.73));
        const auto between = rmsDb (bleed, at (1.1), at (1.35)) - rmsDb (in, at (1.1), at (1.35));   // between the words at 0.5 and 1.5 s, inside the margins kept round a word
        auto worstWord = 0.0;

        for (const auto* out : { &wind, &rustle, &bleed })
            for (int h = 1; h <= 6; ++h)
                worstWord = juce::jmax (worstWord, std::abs (20.0 * std::log10 (amplitude (*out, 160.0 * h, at (2.55), 14400) / amplitude (in, 160.0 * h, at (2.55), 14400))));

        // What each job leaves alone: wind leaves the bleed between words, rustle leaves the gust,
        // bleed leaves the rustle (it sits within the threshold? no: it is far from voice and quiet,
        // so bleed lowers it too; checked for wind and rustle only).
        const auto windLeavesBetween = rmsDb (wind, at (1.1), at (1.35)) - rmsDb (in, at (1.1), at (1.35));
        const auto rustleLeavesGust = rmsDb (rustle, at (5.5), at (6.5)) - rmsDb (in, at (5.5), at (6.5));

        const auto cancelled = Spill::apply (*snap, 0, in.getNumSamples(), rate, onlyWind, [] (float) { return false; }) == nullptr;
        Spill::Settings bad;
        bad.windCornerHz = 30.0;
        const auto refused = Spill::apply (*snap, 0, in.getNumSamples(), rate, bad) == nullptr && Spill::apply (*snap, 0, 4000, rate, onlyWind) == nullptr;

        std::cout << "      wind: the gust " << gust << " dB (the bleed between words " << windLeavesBetween << "); rustle: the burst " << rustleBurst << " dB (the gust "
                  << rustleLeavesGust << "); bleed: between words " << between << " dB; the words' harmonics moved at most " << worstWord << " dB" << std::endl;
        check (gust < -9.0 && std::abs (windLeavesBetween) < 0.5 && rustleBurst < -8.0 && std::abs (rustleLeavesGust) < 0.5 && between < -11.0 && worstWord < 0.5
                   && cancelled && refused,
               "wind, rustle and bleed: with 12 dB each, a two-second gust of rumble comes down over 9 dB while the bleed between words is left within "
               "0.5 dB; a 150 ms burst of bright noise far from the words comes down over 8 dB while the gust is left within 0.5 dB; the bleed between two "
               "words comes down over 11 dB; the words' first six harmonics move under 0.5 dB under each job; it can be cancelled; a corner under 60 Hz "
               "and a range under a tenth of a second are refused");
    }
}
