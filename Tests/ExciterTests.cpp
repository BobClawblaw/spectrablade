#include <JuceHeader.h>
#include "../Source/Exciter.h"
#include "../Source/BuiltInEffects.h"
#include "TestUtils.h"
#include "ExciterTests.h"

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

    juce::AudioBuffer<float> sine (double hz, double amplitude, int frames = 48000)
    {
        juce::AudioBuffer<float> b (2, frames);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, (float) (amplitude * std::sin (juce::MathConstants<double>::twoPi * hz * i / rate)));

        return b;
    }

    /** A frequency's amplitude in channel 0 over the second half: a single-bin DFT. */
    double amplitude (const juce::AudioBuffer<float>& b, double hz)
    {
        double re = 0.0, im = 0.0;
        const auto from = b.getNumSamples() / 2, length = b.getNumSamples() - from;

        for (int i = from; i < from + length; ++i)
        {
            const auto phase = juce::MathConstants<double>::twoPi * hz * i / rate;
            re += b.getSample (0, i) * std::cos (phase);
            im += b.getSample (0, i) * std::sin (phase);
        }

        return 2.0 * std::sqrt (re * re + im * im) / length;
    }

    double db (double ratio)    { return 20.0 * std::log10 (std::max (1.0e-12, ratio)); }

    Exciter::Settings everyBand (Exciter::Curve curve, double driveDb, double mix = 100.0)
    {
        Exciter::Settings s;

        for (auto& b : s.bands)
            b = { driveDb, curve, mix };

        return s;
    }

    juce::AudioBuffer<float> applied (const juce::AudioBuffer<float>& in, const Exciter::Settings& s)
    {
        return readAll (*Exciter::apply (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (in)), 0, in.getNumSamples(), rate, s));
    }
}

void runExciterTests()
{
    std::cout << "Exciter:" << std::endl;

    // The oversampler: an impulse lands latency frames late, DC passes at unity, the filter passes
    // 0.8 of the original Nyquist within 0.5 dB and stops 1.2 of it by over 60 dB.
    {
        Oversampler4 over;
        const auto identity = [] (double x) { return x; };
        auto peakAt = -1;
        auto peak = 0.0, lastOut = 0.0;

        for (int i = 0; i < 200; ++i)
        {
            const auto out = over.tick (i == 0 ? 1.0 : 0.0, identity);

            if (std::abs (out) > peak)
            {
                peak = std::abs (out);
                peakAt = i;
            }
        }

        over.reset();

        for (int i = 0; i < 400; ++i)
            lastOut = over.tick (1.0, identity);

        const auto pass = Oversampler4::responseDb (0.8 * 0.125), stop = Oversampler4::responseDb (1.2 * 0.125);
        std::cout << "      oversampler: impulse peak " << peak << " at " << peakAt << " (latency " << Oversampler4::latency << "), DC " << lastOut
                  << ", at 0.8 Nyquist " << pass << " dB, at 1.2 Nyquist " << stop << " dB" << std::endl;
        check (peakAt == Oversampler4::latency && std::abs (lastOut - 1.0) < 1.0e-6 && pass > -0.5 && stop < -60.0,
               "oversampler: an impulse comes out at its latency (40 frames), DC passes at unity within 1e-6, 0.8 of Nyquist passes within 0.5 dB "
               "and 1.2 of it is over 60 dB down");
    }

    // The curves: slope 1 at zero, bounded; Tube adds a 2nd harmonic, Tape only odd ones, Transistor
    // more of the higher odd ones than Tape.
    {
        auto slopes = true, bounded = true;

        for (auto c : { Exciter::Curve::tube, Exciter::Curve::tape, Exciter::Curve::transistor })
        {
            slopes = slopes && std::abs (Exciter::shape (c, 1.0e-4) / 1.0e-4 - 1.0) < 1.0e-6 && std::abs (Exciter::shape (c, -1.0e-4) / -1.0e-4 - 1.0) < 1.0e-6;
            bounded = bounded && std::abs (Exciter::shape (c, 50.0)) <= 1.0 && std::abs (Exciter::shape (c, -50.0)) <= 1.0;
        }

        const auto in = sine (1000.0, 0.5);
        const auto tube = applied (in, everyBand (Exciter::Curve::tube, 18.0));
        const auto tape = applied (in, everyBand (Exciter::Curve::tape, 18.0));
        const auto transistor = applied (in, everyBand (Exciter::Curve::transistor, 18.0));
        const auto h = [] (const juce::AudioBuffer<float>& b, int n) { return db (amplitude (b, 1000.0 * n) / amplitude (b, 1000.0)); };
        std::cout << "      harmonics (dB below the fundamental): tube 2nd " << h (tube, 2) << ", 3rd " << h (tube, 3) << "; tape 2nd " << h (tape, 2) << ", 3rd " << h (tape, 3)
                  << ", 5th " << h (tape, 5) << "; transistor 2nd " << h (transistor, 2) << ", 3rd " << h (transistor, 3) << ", 5th " << h (transistor, 5) << std::endl;
        check (slopes && bounded && h (tube, 2) > -30.0 && h (tape, 2) < -80.0 && h (tape, 3) > -30.0 && h (transistor, 2) < -80.0
                   && h (transistor, 5) > h (tape, 5),
               "exciter curves: each has a slope of 1 at zero and stays within 1; driven 18 dB, Tube's 2nd harmonic is within 30 dB of the fundamental, "
               "Tape's 2nd is over 80 dB down with its 3rd within 30 dB, Transistor's 2nd is over 80 dB down and its 5th stronger than Tape's");
    }

    // Aliasing: a 15 kHz sine driven 12 dB into Tape makes a 45 kHz 3rd harmonic that would fold to
    // 3 kHz; oversampled, little of it comes back, where shaping at the rate itself folds it all.
    // (Driven far harder, the 13th harmonic at 195 kHz folds to 3 kHz even at four times the rate.)
    {
        const auto in = sine (15000.0, 0.5);
        const auto out = applied (in, everyBand (Exciter::Curve::tape, 12.0));
        juce::AudioBuffer<float> naive (in);
        const auto g = std::pow (10.0, 12.0 / 20.0);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < naive.getNumSamples(); ++i)
                naive.setSample (ch, i, (float) (std::tanh (naive.getSample (ch, i) * g) / g));

        const auto folded = db (amplitude (out, 3000.0) / amplitude (out, 15000.0)), naiveFolded = db (amplitude (naive, 3000.0) / amplitude (naive, 15000.0));
        std::cout << "      aliasing: the 3rd harmonic of 15 kHz folded to 3 kHz " << folded << " dB below the fundamental oversampled, " << naiveFolded << " shaped at the rate" << std::endl;
        check (folded < -50.0 && naiveFolded > -25.0,
               "exciter: a 15 kHz sine driven 12 dB into Tape puts its 3rd harmonic (45 kHz) folded to 3 kHz over 50 dB below the fundamental "
               "(shaped at the rate itself it would be within 25 dB)");
    }

    // Mix 0 leaves each band as it was (the split adds back flat), the result where the audio was.
    {
        const auto s = everyBand (Exciter::Curve::transistor, 30.0, 0.0);
        auto worst = 0.0;

        for (auto hz : { 60.0, 500.0, 5000.0, 12000.0 })
        {
            const auto in = sine (hz, 0.5);
            const auto out = applied (in, s);
            worst = juce::jmax (worst, std::abs (db (amplitude (out, hz) / amplitude (in, hz))));
        }

        std::cout << "      mix 0: the level changed at most " << worst << " dB" << std::endl;
        check (worst < 0.01, "exciter: at a mix of 0 every band passes as it was, sines from 60 Hz to 12 kHz within 0.01 dB");
    }

    // Live, in uneven blocks, late by the latency: the offline samples bit for bit; the latency
    // reported; state and presets; refused settings.
    {
        juce::Random r (5);
        juce::AudioBuffer<float> in (2, 48000);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 48000; ++i)
                in.setSample (ch, i, (r.nextFloat() - 0.5f) * 0.6f);

        Exciter::Settings s;
        s.lowCrossover = 150.0;
        s.highCrossover = 2500.0;
        s.bands[0] = { 6.0, Exciter::Curve::tape, 60.0 };
        s.bands[1] = { 12.0, Exciter::Curve::tube, 80.0 };
        s.bands[2] = { 18.0, Exciter::Curve::transistor, 40.0 };
        s.outputDb = -2.0;

        BuiltInEffects::ExciterProcessor live;
        live.setSettings (s);
        live.setPlayConfigDetails (2, 2, rate, 512);
        live.prepareToPlay (rate, 3000);
        juce::AudioBuffer<float> played (2, 48000 + Exciter::Core::latency);
        played.clear();

        for (int ch = 0; ch < 2; ++ch)
            played.copyFrom (ch, 0, in, ch, 0, 48000);

        juce::MidiBuffer midi;
        const int sizes[] = { 1, 7, 512, 3000 };

        for (int pos = 0, k = 0; pos < played.getNumSamples(); ++k)
        {
            const auto n = juce::jmin (sizes[k % 4], played.getNumSamples() - pos);
            juce::AudioBuffer<float> block (played.getArrayOfWritePointers(), 2, pos, n);
            live.processBlock (block, midi);
            pos += n;
        }

        const auto offline = applied (in, live.getSettings());
        auto same = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 48000; ++i)
                same = same && juce::exactlyEqual (played.getSample (ch, i + Exciter::Core::latency), offline.getSample (ch, i));

        juce::MemoryBlock state;
        live.getStateInformation (state);
        BuiltInEffects::ExciterProcessor copy, fromPreset;
        copy.setStateInformation (state.getData(), (int) state.getSize());
        fromPreset.setPresetValues (live.getPresetValues());
        const auto values = [] (const Exciter::Settings& x)
        {
            std::vector<double> v { x.lowCrossover, x.highCrossover, x.outputDb };

            for (auto& b : x.bands)
                v.insert (v.end(), { b.driveDb, (double) b.curve, b.mixPercent });

            return v;
        };
        const auto kept = values (copy.getSettings()) == values (live.getSettings()) && values (fromPreset.getSettings()) == values (live.getSettings());

        auto tooMuch = s;
        tooMuch.bands[1].driveDb = 40.0;
        auto close = s;
        close.highCrossover = 200.0;
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        const auto refused = Exciter::apply (*snap, 0, 48000, rate, tooMuch) == nullptr && Exciter::apply (*snap, 0, 48000, rate, close) == nullptr
                             && Exciter::apply (*snap, 100, 100, rate, s) == nullptr && juce::exactlyEqual (Exciter::spaced (close, rate).highCrossover, 300.0);

        check (same && live.getLatencySamples() == Exciter::Core::latency && kept && refused,
               "built-in exciter: played in blocks of 1, 7, 512 and 3,000 frames it gives the offline samples bit for bit, late by its 40 frames of "
               "latency, which it reports; its state and presets restore every setting; a drive past 36 dB, crossovers under an octave apart and an "
               "empty range are refused, and spaced moves the crossovers apart");
    }
}
