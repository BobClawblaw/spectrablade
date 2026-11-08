#include <JuceHeader.h>
#include "../Source/TransientShaper.h"
#include "../Source/BuiltInEffects.h"
#include "TestUtils.h"
#include "TransientShaperTests.h"

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

    juce::AudioBuffer<float> applied (const juce::AudioBuffer<float>& in, const TransientShaper::Settings& s)
    {
        return readAll (*TransientShaper::apply (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (in)), 0, in.getNumSamples(), rate, s));
    }

    /** A second of hits: a 100 Hz tone at -14 dB with, every 125 ms, a 3 kHz hit at -2 dB that dies
        away over 60 ms (a 40 ms time constant); with crossovers at 200 Hz and 2 kHz, the tone has
        the low band and the hits the high, with nothing in the middle. */
    juce::AudioBuffer<float> hits()
    {
        juce::AudioBuffer<float> b (2, 48000);

        for (int i = 0; i < 48000; ++i)
        {
            const auto tone = 0.2 * std::sin (juce::MathConstants<double>::twoPi * 100.0 * i / rate);
            const auto since = i % 6000;
            const auto burst = since < 2880 ? 0.8 * std::exp (-since / 1920.0) * std::sin (juce::MathConstants<double>::twoPi * 3000.0 * i / rate) : 0.0;
            b.setSample (0, i, (float) (tone + burst));
            b.setSample (1, i, (float) (tone + burst));
        }

        return b;
    }

    double rmsDb (const juce::AudioBuffer<float>& b, int from, int to)
    {
        double sum = 0.0;

        for (int i = from; i < to; ++i)
            sum += (double) b.getSample (0, i) * b.getSample (0, i);

        return 10.0 * std::log10 (sum / (to - from));
    }

    /** Averaged over hits 1 to 7: the RMS of [offset, offset + length) frames after each hit's start. */
    double afterHits (const juce::AudioBuffer<float>& b, int offset, int length)
    {
        auto sum = 0.0;

        for (int hit = 1; hit < 8; ++hit)
            sum += rmsDb (b, hit * 6000 + offset, hit * 6000 + offset + length);

        return sum / 7.0;
    }

    /** A frequency's amplitude in channel 0 over [from, from + length). */
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

void runTransientShaperTests()
{
    std::cout << "Transient shaper:" << std::endl;

    // Attack up lifts the first moments of each hit; sustain up lifts what follows; a steady tone and
    // every gain at 0 pass flat; the bands act on their own.
    {
        const auto in = hits();
        TransientShaper::Settings flat;
        flat.lowCrossover = 200.0;
        flat.highCrossover = 2000.0;
        auto attack = flat, sustain = flat, highOnly = flat;

        for (auto& b : attack.bands)
            b.attackDb = 6.0;

        for (auto& b : sustain.bands)
            b.sustainDb = 6.0;

        highOnly.bands[2].attackDb = 6.0;

        const auto plain = applied (in, flat), punched = applied (in, attack), held = applied (in, sustain), highs = applied (in, highOnly);
        const auto firstPlain = afterHits (plain, 0, 48), firstPunched = afterHits (punched, 0, 48);              // the first ms of each hit
        const auto tailOf = [] (const juce::AudioBuffer<float>& b)                                               // the hit's 3 kHz, 30 to 60 ms after it
        {
            auto sum = 0.0;

            for (int hit = 1; hit < 8; ++hit)
                sum += 20.0 * std::log10 (amplitude (b, 3000.0, hit * 6000 + 1440, 1440));

            return sum / 7.0;
        };
        const auto toneOf = [] (const juce::AudioBuffer<float>& b)                                               // the 100 Hz tone, 90 to 120 ms after
        {
            auto sum = 0.0;

            for (int hit = 1; hit < 8; ++hit)
                sum += 20.0 * std::log10 (amplitude (b, 100.0, hit * 6000 + 4320, 1440));

            return sum / 7.0;
        };
        const auto tailPlain = tailOf (plain), tailHeld = tailOf (held), tailPunched = tailOf (punched);
        const auto tonePlain = toneOf (plain), toneHeld = toneOf (held);
        const auto flatness = rmsDb (plain, 6000, 48000) - rmsDb (in, 6000, 48000);
        const auto burstLift = 20.0 * std::log10 (amplitude (highs, 3000.0, 6000, 48) / amplitude (plain, 3000.0, 6000, 48));
        const auto toneKept = 20.0 * std::log10 (amplitude (highs, 100.0, 9000, 2400) / amplitude (plain, 100.0, 9000, 2400));

        std::cout << "      attack +6: the first ms of a hit " << firstPlain << " -> " << firstPunched << " dB, its tail " << tailPlain << " -> " << tailPunched
                  << "; sustain +6: the tail 30-60 ms after " << tailPlain << " -> " << tailHeld << ", the tone 90-120 ms after " << tonePlain << " -> " << toneHeld
                  << "; flat " << flatness << " dB; the high band alone: the 3 kHz hit " << burstLift << " dB, the 100 Hz tone " << toneKept << " dB" << std::endl;
        check (firstPunched > firstPlain + 3.0 && std::abs (tailPunched - tailPlain) < 0.5 && tailHeld > tailPlain + 2.0 && std::abs (toneHeld - tonePlain) < 0.6
                   && std::abs (flatness) < 0.01 && burstLift > 3.0 && std::abs (toneKept) < 0.1,
               "transient shaper: attack +6 dB lifts the first millisecond of each hit over 3 dB and leaves its tail (30 to 60 ms after) within 0.5 dB; "
               "sustain +6 dB lifts the tail over 2 dB and moves the steady tone in the band below by under 0.6 dB (the middle band, which holds only what "
               "leaks through the crossovers' skirts, lifts the tone's leak with the hit's); every gain at 0 passes within 0.01 dB; attack on the "
               "high band alone lifts the 3 kHz hit over 3 dB and leaves the 100 Hz tone within 0.1 dB");
    }

    // Live, in uneven blocks: the offline samples bit for bit; no latency; state and presets; refused settings.
    {
        const auto in = hits();
        TransientShaper::Settings s;
        s.lowCrossover = 150.0;
        s.highCrossover = 2500.0;
        s.bands = {{ { 4.0, -3.0 }, { 6.0, 2.0 }, { -2.0, 5.0 } }};
        s.outputDb = -1.0;

        BuiltInEffects::TransientShaperProcessor live;
        live.setSettings (s);
        live.setPlayConfigDetails (2, 2, rate, 512);
        live.prepareToPlay (rate, 3000);
        juce::AudioBuffer<float> played (in);
        juce::MidiBuffer midi;
        const int sizes[] = { 1, 7, 512, 3000 };

        for (int pos = 0, k = 0; pos < 48000; ++k)
        {
            const auto n = juce::jmin (sizes[k % 4], 48000 - pos);
            juce::AudioBuffer<float> block (played.getArrayOfWritePointers(), 2, pos, n);
            live.processBlock (block, midi);
            pos += n;
        }

        const auto offline = applied (in, live.getSettings());
        auto same = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 48000; ++i)
                same = same && juce::exactlyEqual (played.getSample (ch, i), offline.getSample (ch, i));

        juce::MemoryBlock state;
        live.getStateInformation (state);
        BuiltInEffects::TransientShaperProcessor copy, fromPreset;
        copy.setStateInformation (state.getData(), (int) state.getSize());
        fromPreset.setPresetValues (live.getPresetValues());
        const auto values = [] (const TransientShaper::Settings& x)
        {
            std::vector<double> v { x.lowCrossover, x.highCrossover, x.outputDb };

            for (auto& b : x.bands)
                v.insert (v.end(), { b.attackDb, b.sustainDb });

            return v;
        };
        const auto kept = values (copy.getSettings()) == values (live.getSettings()) && values (fromPreset.getSettings()) == values (live.getSettings());

        auto tooMuch = s;
        tooMuch.bands[0].attackDb = 15.0;
        auto close = s;
        close.highCrossover = 200.0;
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        const auto refused = TransientShaper::apply (*snap, 0, 48000, rate, tooMuch) == nullptr && TransientShaper::apply (*snap, 0, 48000, rate, close) == nullptr
                             && TransientShaper::apply (*snap, 100, 100, rate, s) == nullptr;

        check (same && live.getLatencySamples() == 0 && kept && refused,
               "built-in transient shaper: played in blocks of 1, 7, 512 and 3,000 frames it gives the offline samples bit for bit; no latency; its state "
               "and presets restore every setting; a gain past 12 dB, crossovers under an octave apart and an empty range are refused");
    }
}
