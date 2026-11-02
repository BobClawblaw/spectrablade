#include <JuceHeader.h>
#include "../Source/GraphicEq.h"
#include "../Source/BuiltInEffects.h"
#include "TestUtils.h"
#include "GraphicEqTests.h"

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

    /** The gain in dB the gains give a sine at hz, measured on its second half second. */
    double measuredDb (const std::vector<double>& gains, double hz)
    {
        juce::AudioBuffer<float> b (1, 48000);

        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / rate));

        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (b));
        const auto out = readAll (*GraphicEq::apply (*snap, 0, 48000, rate, gains));
        return 20.0 * std::log10 (out.getRMSLevel (0, 24000, 24000) / b.getRMSLevel (0, 24000, 24000));
    }

    /** The cascade's response at hz: the active bands' responses added in dB. */
    double predictedDb (const std::vector<double>& gains, double hz)
    {
        double db = 0.0;

        for (auto& band : GraphicEq::bandsFor (gains, rate))
            db += Equalizer::responseDb (band, rate, hz);

        return db;
    }
}

void runGraphicEqTests()
{
    check (GraphicEq::centres (10).size() == 10 && GraphicEq::centres (31).size() == 31 && juce::exactlyEqual (GraphicEq::centres (31).front(), 20.0)
               && juce::exactlyEqual (GraphicEq::centres (31).back(), 20000.0) && std::abs (GraphicEq::bandQ (10) - 1.4142) < 1.0e-4
               && std::abs (GraphicEq::bandQ (31) - 4.3185) < 1.0e-4,
           "graphic EQ: 10 bands from 31.5 Hz to 16 kHz with a Q of 1.414, 31 from 20 Hz to 20 kHz with a Q of 4.318");

    // One band: its gain at its centre; less an octave (10 bands) or a third (31) away.
    {
        std::vector<double> ten (10, 0.0), thirtyOne (31, 0.0);
        ten[5] = 6.0;          // 1 kHz
        thirtyOne[17] = 6.0;   // 1 kHz
        const auto tenAt = measuredDb (ten, 1000.0), tenOctave = measuredDb (ten, 2000.0);
        const auto thirdAt = measuredDb (thirtyOne, 1000.0), thirdNext = measuredDb (thirtyOne, 1250.0), thirdOctave = measuredDb (thirtyOne, 2000.0);
        std::cout << "      graphic EQ, 1 kHz at +6: 10 bands " << tenAt << " dB at 1 kHz, " << tenOctave << " at 2 kHz; 31 bands "
                  << thirdAt << " at 1 kHz, " << thirdNext << " at 1.25 kHz, " << thirdOctave << " at 2 kHz" << std::endl;
        check (std::abs (tenAt - 6.0) < 0.02 && std::abs (thirdAt - 6.0) < 0.02 && tenOctave > 0.5 && tenOctave < 3.0 && thirdNext > 0.5 && thirdNext < 3.0
                   && thirdOctave < 0.5,
               "graphic EQ: a band at +6 dB gives +6 dB at its centre (to 0.02), between 0.5 and 3 dB at the next band's centre, and under 0.5 dB an octave off with 31 bands");
    }

    // Several bands: the measured gain is the bands' responses added (they run one after another).
    {
        std::vector<double> curve { -6.0, -3.0, 0.0, 4.0, 4.0, 4.0, -2.0, 0.0, 6.0, -12.0 };
        auto worst = 0.0;

        for (auto hz : { 50.0, 200.0, 700.0, 1000.0, 3000.0, 6000.0, 12000.0 })
            worst = juce::jmax (worst, std::abs (measuredDb (curve, hz) - predictedDb (curve, hz)));

        std::vector<double> allUp (10, 6.0);
        const auto summed = measuredDb (allUp, 1000.0);
        std::cout << "      graphic EQ: measured against the bands' responses added, worst " << worst << " dB; all ten at +6 give " << summed << " dB at 1 kHz" << std::endl;
        check (worst < 0.02, "graphic EQ: with ten bands set to a curve, sines from 50 Hz to 12 kHz are changed by the bands' responses added up (to 0.02 dB)");
    }

    // Refused: all flat, a gain past 12 dB, a band count other than 10 or 31; bands past half the rate left out.
    {
        juce::AudioBuffer<float> b (1, 1000);
        b.clear();
        const auto snap = AudioEdits::fromBuffer (std::move (b));
        std::vector<double> tooMuch (10, 0.0);
        tooMuch[0] = 12.5;
        std::vector<double> high (31, 0.0);
        high[29] = 3.0;   // 16 kHz
        high[30] = 3.0;   // 20 kHz
        check (GraphicEq::apply (*snap, 0, 1000, rate, std::vector<double> (10, 0.0)) == nullptr && GraphicEq::apply (*snap, 0, 1000, rate, tooMuch) == nullptr
                   && GraphicEq::apply (*snap, 0, 1000, rate, std::vector<double> (11, 1.0)) == nullptr
                   && GraphicEq::bandsFor (high, 32000.0).empty() && GraphicEq::bandsFor (high, 48000.0).size() == 2,
               "graphic EQ: all at 0 dB, a gain past 12 dB or 11 bands refused; at 32 kHz the 16 and 20 kHz bands (at or past half the rate) are left out");
    }

    // Live: the 31-band EQ in blocks gives the offline samples; its state and presets keep the gains.
    {
        juce::Random r (61);
        juce::AudioBuffer<float> in (2, 96000);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < in.getNumSamples(); ++i)
                in.setSample (ch, i, r.nextFloat() - 0.5f);

        BuiltInEffects::GraphicEqProcessor eq (31);
        std::vector<double> gains (31, 0.0);

        for (size_t i = 0; i < gains.size(); i += 3)
            gains[i] = (double) ((int) i % 7) - 3.0;

        eq.setGains (gains);
        eq.setPlayConfigDetails (2, 2, rate, 512);
        eq.prepareToPlay (rate, 3000);
        juce::AudioBuffer<float> live (in);
        juce::MidiBuffer midi;
        const int sizes[] = { 1, 7, 512, 3000 };

        for (int pos = 0, k = 0; pos < live.getNumSamples(); ++k)
        {
            const auto n = juce::jmin (sizes[k % 4], live.getNumSamples() - pos);
            juce::AudioBuffer<float> block (live.getArrayOfWritePointers(), 2, pos, n);
            eq.processBlock (block, midi);
            pos += n;
        }

        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        const auto offline = readAll (*GraphicEq::apply (*snap, 0, 96000, rate, eq.getGains()));
        auto same = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 96000; ++i)
                same = same && juce::exactlyEqual (live.getSample (ch, i), offline.getSample (ch, i));

        juce::MemoryBlock state;
        eq.getStateInformation (state);
        BuiltInEffects::GraphicEqProcessor copy (31), fromPreset (31);
        copy.setStateInformation (state.getData(), (int) state.getSize());
        fromPreset.setPresetValues (eq.getPresetValues());
        BuiltInEffects::GraphicEqProcessor ten (10);
        check (same && copy.getGains() == eq.getGains() && fromPreset.getGains() == eq.getGains() && ten.getNumBands() == 10
                   && eq.getPresetKey() == BuiltInEffects::graphicEq31Id && eq.getLatencySamples() == 0,
               "built-in graphic EQ: 31 bands played in blocks of 1, 7, 512 and 3,000 frames give the offline samples bit for bit; its state and its "
               "preset values restore every gain exactly; no latency");
    }
}
