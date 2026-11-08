#include <JuceHeader.h>
#include "../Source/Ambience.h"
#include "../Source/MatchEq.h"
#include "../Source/AudioEdits.h"
#include "TestUtils.h"
#include "AmbienceTests.h"

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

    /** Six seconds: room tone (noise falling 3 dB an octave, at about -50 dB) for the first three,
        silence for the last three. */
    juce::AudioBuffer<float> room()
    {
        juce::Random r (31);
        juce::AudioBuffer<float> b (2, 6 * 48000);
        double state[2] = { 0.0, 0.0 };

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                // Pink-ish: white noise through a gentle one-pole, mixed with a little white.
                const auto white = r.nextDouble() * 2.0 - 1.0;
                state[ch] = 0.995 * state[ch] + 0.005 * white;
                b.setSample (ch, i, i < 3 * 48000 ? (float) (0.0025 * white + 0.05 * state[ch]) : 0.0f);
            }

        return b;
    }

    double levelAt (const MatchEq::Profile& p, double hz)
    {
        return MatchEq::correctionAt (p.levelDb, hz);
    }

    double rmsDb (const juce::AudioBuffer<float>& b, int from, int to)
    {
        return 20.0 * std::log10 (juce::jmax (1.0e-10f, b.getRMSLevel (0, from, to - from)));
    }
}

void runAmbienceTests()
{
    std::cout << "Ambience match:" << std::endl;

    // The print of the room tone laid under the silence: the new tone's profile within 2.5 dB of the
    // room's from 100 Hz to 10 kHz and its level within 1 dB; eased in at the edge; the same twice;
    // filling only the quiet leaves a range that already holds the tone within 1 dB, where filling
    // regardless doubles its power (+3 dB); refused settings and prints.
    {
        const auto in = room();
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        const auto print = Ambience::capture (*snap, 0, 2 * 48000, rate);
        Ambience::Settings s;
        const auto out = readAll (*Ambience::apply (*snap, 3 * 48000, 6 * 48000, rate, *print, s));
        const auto again = readAll (*Ambience::apply (*snap, 3 * 48000, 6 * 48000, rate, *print, s));
        const auto pRoom = MatchEq::measure (*snap, 0, 3 * 48000, rate);
        const auto pNew = MatchEq::measure (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (out)), (juce::int64) (3.2 * 48000), (juce::int64) (5.8 * 48000), rate);
        auto worstShape = 0.0;

        for (auto hz : { 100.0, 300.0, 1000.0, 3000.0, 10000.0 })
            worstShape = juce::jmax (worstShape, std::abs ((levelAt (*pNew, hz) - levelAt (*pNew, 1000.0)) - (levelAt (*pRoom, hz) - levelAt (*pRoom, 1000.0))));

        const auto level = rmsDb (out, (int) (3.2 * 48000), (int) (5.8 * 48000)) - rmsDb (in, 24000, (int) (2.8 * 48000));
        const auto easedIn = std::abs (out.getSample (0, 3 * 48000)) < 1.0e-4f && std::abs (out.getSample (0, 3 * 48000 + 100)) < 0.1f * std::abs (out.getSample (0, 3 * 48000 + 20000)) + 1.0e-3f;
        auto same = true;

        for (int i = 0; i < in.getNumSamples(); ++i)
            same = same && juce::exactlyEqual (out.getSample (0, i), again.getSample (0, i));

        Ambience::Settings regardless;
        regardless.fillOnlyTheQuiet = false;
        const auto overQuiet = readAll (*Ambience::apply (*snap, 0, 3 * 48000, rate, *print, s)), overRegardless = readAll (*Ambience::apply (*snap, 0, 3 * 48000, rate, *print, regardless));
        const auto quietChange = rmsDb (overQuiet, 24000, (int) (2.8 * 48000)) - rmsDb (in, 24000, (int) (2.8 * 48000));
        const auto regardlessChange = rmsDb (overRegardless, 24000, (int) (2.8 * 48000)) - rmsDb (in, 24000, (int) (2.8 * 48000));

        auto otherRate = *print;
        otherRate.sampleRate = 44100.0;
        Ambience::Settings bad;
        bad.levelDb = 20.0f;
        const auto refused = Ambience::apply (*snap, 3 * 48000, 6 * 48000, rate, otherRate, s) == nullptr && Ambience::apply (*snap, 3 * 48000, 6 * 48000, rate, *print, bad) == nullptr
                             && Ambience::apply (*snap, 100, 100, rate, *print, s) == nullptr && ! Ambience::capture (*snap, 0, 1000, rate).has_value();

        std::cout << "      the new tone: shape within " << worstShape << " dB of the room's, level " << level << " dB against it; over the room itself: only the quiet "
                  << quietChange << " dB, regardless " << regardlessChange << " dB" << std::endl;
        check (worstShape < 2.5 && std::abs (level) < 1.0 && easedIn && same && std::abs (quietChange) < 1.0 && regardlessChange > 2.0 && regardlessChange < 4.0 && refused,
               "ambience match: room tone laid under silence has a profile within 2.5 dB of the room's shape from 100 Hz to 10 kHz (the reading itself "
               "wanders by a decibel at the low end) and a level within 1 dB "
               "of it, eased in at the edge, the same twice over; over the room itself, filling only the quiet moves the level under 1 dB and filling "
               "regardless adds 3 dB; a print at another rate, a level past 12 dB, an empty range and a print under a frame are refused");
    }
}
