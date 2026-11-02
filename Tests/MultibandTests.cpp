#include <JuceHeader.h>
#include "../Source/Multiband.h"
#include "../Source/BuiltInEffects.h"
#include "TestUtils.h"
#include "MultibandTests.h"

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

    /** Sines at these frequencies and amplitudes added, on two channels, for a second. */
    juce::AudioBuffer<float> sines (std::initializer_list<std::pair<double, double>> parts, int frames = 48000)
    {
        juce::AudioBuffer<float> b (2, frames);

        for (int i = 0; i < frames; ++i)
        {
            double v = 0.0;

            for (auto [hz, amp] : parts)
                v += amp * std::sin (juce::MathConstants<double>::twoPi * hz * i / rate);

            b.setSample (0, i, (float) v);
            b.setSample (1, i, (float) v);
        }

        return b;
    }

    /** A frequency's amplitude in channel 0 over [from, from + length): a single-bin DFT. */
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

    double db (double ratio)    { return 20.0 * std::log10 (ratio); }

    Multiband::Compressor unity()
    {
        Multiband::Compressor c;

        for (auto& band : c.bands)
            band.ratio = 1.0;

        return c;
    }
}

void runMultibandTests()
{
    // Flat: with every band at a ratio of 1, sines anywhere come out at their level (only phase turned).
    {
        auto worst = 0.0;
        auto turned = false;

        for (auto hz : { 40.0, 150.0, 200.0, 600.0, 2000.0, 5000.0, 15000.0 })
        {
            const auto in = sines ({ { hz, 0.5 } });
            const auto out = readAll (*Multiband::compress (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (in)), 0, 48000, rate, unity()));
            worst = juce::jmax (worst, std::abs (db (amplitude (out, hz, 24000, 24000) / amplitude (in, hz, 24000, 24000))));

            // Not the same samples: the phase is turned.
            auto differs = false;

            for (int i = 24000; i < 24100 && ! differs; ++i)
                differs = std::abs (out.getSample (0, i) - in.getSample (0, i)) > 1.0e-3f;

            turned = turned || differs;
        }

        std::cout << "      multiband at ratio 1: worst level change " << worst << " dB" << std::endl;
        check (worst < 0.01 && turned, "multiband compressor: with every band at a ratio of 1, sines from 40 Hz to 15 kHz (the crossovers included) keep their level within 0.01 dB");
    }

    // Each band on its own frequencies: only the low band compressing turns down 100 Hz, not 5 kHz.
    {
        auto c = unity();
        c.bands[0] = { -40.0, 10.0, 0.0, 5.0, 50.0, 0.0 };
        const auto in = sines ({ { 100.0, 0.4 }, { 5000.0, 0.1 } });
        const auto out = readAll (*Multiband::compress (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (in)), 0, 48000, rate, c));
        const auto lowChange = db (amplitude (out, 100.0, 24000, 24000) / amplitude (in, 100.0, 24000, 24000));
        const auto highChange = db (amplitude (out, 5000.0, 24000, 24000) / amplitude (in, 5000.0, 24000, 24000));

        auto h = unity();
        h.bands[2] = c.bands[0];
        const auto outHigh = readAll (*Multiband::compress (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (in)), 0, 48000, rate, h));
        const auto lowKept = db (amplitude (outHigh, 100.0, 24000, 24000) / amplitude (in, 100.0, 24000, 24000));
        const auto highDown = db (amplitude (outHigh, 5000.0, 24000, 24000) / amplitude (in, 5000.0, 24000, 24000));

        std::cout << "      multiband, low band compressing: 100 Hz " << lowChange << " dB, 5 kHz " << highChange
                  << " dB; high band compressing: 100 Hz " << lowKept << " dB, 5 kHz " << highDown << " dB" << std::endl;
        check (lowChange < -15.0 && std::abs (highChange) < 0.1 && std::abs (lowKept) < 0.1 && highDown < -10.0,
               "multiband compressor: a band compresses only its own frequencies: the low band turns a loud 100 Hz down by over 15 dB, leaving "
               "5 kHz within 0.1 dB, and the high band the reverse");
    }

    // Refused settings.
    {
        const auto snap = AudioEdits::fromBuffer (sines ({ { 1000.0, 0.5 } }, 1000));
        auto close = unity();
        close.highCrossover = 300.0;   // under an octave above 200 Hz
        auto loud = unity();
        loud.outputDb = 30.0;
        check (Multiband::compress (*snap, 0, 1000, rate, close) == nullptr && Multiband::compress (*snap, 0, 1000, rate, loud) == nullptr
                   && Multiband::compress (*snap, 500, 500, rate, unity()) == nullptr,
               "multiband compressor: crossovers under an octave apart, an output past 24 dB, an empty range refused");
    }

    // Live: in blocks of 1, 7, 512 and 3,000 frames give the offline samples bit for bit; state and presets keep the settings.
    {
        juce::Random r (17);
        juce::AudioBuffer<float> in (2, 96000);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < in.getNumSamples(); ++i)
                in.setSample (ch, i, (r.nextFloat() - 0.5f) * (i % 20000 < 8000 ? 1.0f : 0.1f));

        const auto runLive = [&in] (BuiltInEffects::BuiltInEffect& effect)
        {
            effect.setPlayConfigDetails (2, 2, rate, 512);
            effect.prepareToPlay (rate, 3000);
            juce::AudioBuffer<float> live (in);
            juce::MidiBuffer midi;
            const int sizes[] = { 1, 7, 512, 3000 };

            for (int pos = 0, k = 0; pos < live.getNumSamples(); ++k)
            {
                const auto n = juce::jmin (sizes[k % 4], live.getNumSamples() - pos);
                juce::AudioBuffer<float> block (live.getArrayOfWritePointers(), 2, pos, n);
                effect.processBlock (block, midi);
                pos += n;
            }

            return live;
        };
        const auto same = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    if (! juce::exactlyEqual (a.getSample (ch, i), b.getSample (ch, i)))
                        return false;

            return true;
        };

        Multiband::Compressor m;
        m.lowCrossover = 150.0;
        m.highCrossover = 3000.0;
        m.bands[0] = { -30.0, 3.0, 6.0, 20.0, 200.0, 2.0 };
        m.bands[1] = { -25.0, 2.0, 6.0, 10.0, 100.0, 0.0 };
        m.bands[2] = { -35.0, 6.0, 6.0, 1.0, 50.0, 3.0 };
        m.outputDb = -1.5;
        BuiltInEffects::MultibandProcessor multiband;
        multiband.setSettings (m);
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        const auto multibandSame = same (runLive (multiband), readAll (*Multiband::compress (*snap, 0, 96000, rate, multiband.getSettings())));

        juce::MemoryBlock state;
        multiband.getStateInformation (state);
        BuiltInEffects::MultibandProcessor copy, fromPreset;
        copy.setStateInformation (state.getData(), (int) state.getSize());
        fromPreset.setPresetValues (multiband.getPresetValues());
        const auto values = [] (const Multiband::Compressor& c)
        {
            std::vector<double> v { c.lowCrossover, c.highCrossover, c.outputDb };

            for (auto& b : c.bands)
                v.insert (v.end(), { b.thresholdDb, b.ratio, b.kneeDb, b.attackMs, b.releaseMs, b.makeupDb });

            return v;
        };
        const auto kept = values (copy.getSettings()) == values (multiband.getSettings()) && values (fromPreset.getSettings()) == values (multiband.getSettings());

        // Crossovers set too close are moved an octave apart live, as the offline effect would refuse them.
        auto close = m;
        close.highCrossover = 200.0;
        BuiltInEffects::MultibandProcessor squeezed;
        squeezed.setSettings (close);
        squeezed.setPlayConfigDetails (2, 2, rate, 512);
        squeezed.prepareToPlay (rate, 512);
        const auto apart = juce::exactlyEqual (squeezed.getSettings().highCrossover, 300.0) && Multiband::isValid (squeezed.getSettings(), rate);

        // The rack window: the panel shows the effect's settings, a change on it reaches the effect, and one at the effect reaches it.
        auto windowWorks = false;
        {
            juce::ScopedJuceInitialiser_GUI gui;
            std::unique_ptr<juce::AudioProcessorEditor> made (multiband.createEditor());

            // Within a part in 10^5: the parameters hold floats, and the sliders snap to their steps.
            const auto near = [&values] (const Multiband::Compressor& a, const Multiband::Compressor& b)
            {
                const auto x = values (a), y = values (b);

                for (size_t i = 0; i < x.size(); ++i)
                    if (std::abs (x[i] - y[i]) > 1.0e-5 * juce::jmax (1.0, std::abs (y[i])))
                        return false;

                return true;
            };

            if (auto* editor = dynamic_cast<BuiltInEffects::MultibandEditor*> (made.get()))
            {
                const auto showsEffect = near (editor->getPanel().getSettings(), multiband.getParameterSettings());
                auto untouched = multiband.getParameterSettings();
                editor->getPanel().onChange();   // nothing moved: no parameter may change
                const auto leftAlone = values (untouched) == values (multiband.getParameterSettings());
                auto moved = m;
                moved.bands[1].ratio = 8.0;
                moved.highCrossover = 4000.0;
                editor->getPanel().setSettings (moved);
                editor->getPanel().onChange();
                const auto reachedEffect = near (multiband.getParameterSettings(), moved);
                multiband.setSettings (m);
                editor->refresh();
                windowWorks = showsEffect && leftAlone && reachedEffect && near (editor->getPanel().getSettings(), m);
            }

            made.reset();
        }

        check (windowWorks, "built-in multiband compressor: its window shows the effect's settings, a change on it reaches the effect (and "
                            "no change leaves every parameter as it was), and one made at the effect (automation, a preset) shows in it");

        // The window's readings: what the core ended on, the same input run through them offline-style.
        auto readingsRight = false;
        {
            Multiband::CompressorCore mc;
            mc.prepare (2, rate);
            mc.set (multiband.getSettings());
            juce::AudioBuffer<float> a (in);
            mc.process (a.getArrayOfWritePointers(), 2, a.getNumSamples());
            runLive (multiband);

            readingsRight = multiband.getReductionDb() == mc.getReductionDb() && mc.getReductionDb()[2] < 0.0;
            multiband.reset();
            readingsRight = readingsRight && multiband.getReductionDb() == std::array<double, 3> {};
        }

        check (readingsRight, "built-in multiband compressor: the reductions its window shows are what the core ended the last block on, "
                              "and zero after a reset");

        check (multibandSame && kept && apart && multiband.getLatencySamples() == 0,
               "built-in multiband compressor: played in blocks of 1, 7, 512 and 3,000 frames it gives the offline samples bit for bit; state "
               "and preset values restore every setting; crossovers set too close are moved an octave apart; no latency");
    }
}
