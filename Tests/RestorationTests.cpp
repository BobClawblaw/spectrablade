#include <JuceHeader.h>
#include "../Source/Restoration.h"
#include "TestUtils.h"
#include "RestorationTests.h"

namespace
{
    using TestUtils::check;
    constexpr double rate = 48000.0;

    /** Two channels of sines (frequency, amplitude), seconds long. */
    AudioEdits::Snapshot tones (std::initializer_list<std::pair<double, float>> sines, double seconds)
    {
        juce::AudioBuffer<float> b (2, (int) (seconds * rate));

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            double v = 0.0;

            for (auto [f, a] : sines)
                v += a * std::sin (juce::MathConstants<double>::twoPi * f * i / rate);

            b.setSample (0, i, (float) v);
            b.setSample (1, i, (float) (0.5 * v));
        }

        return AudioEdits::fromBuffer (std::move (b));
    }

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    /** A sine's amplitude in [from, from + n) of a channel, n holding whole cycles of it. */
    double amplitude (const juce::AudioBuffer<float>& b, int channel, double frequency, int from, int n)
    {
        double re = 0.0, im = 0.0;

        for (int i = from; i < from + n; ++i)
        {
            const auto w = juce::MathConstants<double>::twoPi * frequency * i / rate;
            re += b.getSample (channel, i) * std::cos (w);
            im += b.getSample (channel, i) * std::sin (w);
        }

        return 2.0 * std::sqrt (re * re + im * im) / n;
    }

    double db (double ratio)   { return 20.0 * std::log10 (juce::jmax (ratio, 1.0e-12)); }

    void testHum()
    {
        const auto source = tones ({ { 50.0, 0.3f }, { 150.0, 0.1f }, { 1000.0, 0.2f } }, 2.0);
        const auto before = readAll (*source);
        const auto out = Restoration::removeHum (*source, 0, source->getNumFrames(), rate, {});
        const auto after = readAll (*out);
        const auto second = (int) rate;

        const auto cut50 = db (amplitude (after, 0, 50.0, second, second) / amplitude (before, 0, 50.0, second, second));
        const auto cut150 = db (amplitude (after, 0, 150.0, second, second) / amplitude (before, 0, 150.0, second, second));
        const auto kept1k = db (amplitude (after, 0, 1000.0, second, second) / amplitude (before, 0, 1000.0, second, second));
        const auto right50 = db (amplitude (after, 1, 50.0, second, second) / amplitude (before, 1, 50.0, second, second));
        check (cut50 < -40.0 && cut150 < -40.0 && right50 < -40.0 && std::abs (kept1k) < 0.1,
               "hum: 50 Hz hum and its third harmonic drop by " + juce::String (juce::roundToInt (-cut50)) + " and " + juce::String (juce::roundToInt (-cut150))
                   + " dB on both channels, while a 1 kHz tone changes by " + juce::String (kept1k, 3) + " dB");

        const auto tenth = second / 10;   // five cycles of 50 Hz
        const auto opening = db (amplitude (after, 0, 50.0, 0, tenth) / amplitude (before, 0, 50.0, 0, tenth));
        check (opening < -20.0, "hum: gone from the file's first frames too (the first 0.1 s " + juce::String (juce::roundToInt (-opening)) + " dB down)");

        const auto part = Restoration::removeHum (*source, second / 2, second * 3 / 2, rate, {});
        const auto partAfter = readAll (*part);
        bool untouched = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < before.getNumSamples(); ++i)
                if (i < second / 2 || i >= second * 3 / 2)
                    untouched = untouched && juce::exactlyEqual (partAfter.getSample (ch, i), before.getSample (ch, i));

        const auto inside = db (amplitude (partAfter, 0, 50.0, second / 2 + tenth, second / 2) / amplitude (before, 0, 50.0, second / 2 + tenth, second / 2));
        check (untouched && inside < -40.0 && part->getNumFrames() == source->getNumFrames(),
               "hum: on a selection, the audio outside it is left bit for bit and the hum inside it is taken out (" + juce::String (juce::roundToInt (-inside)) + " dB)");

        Restoration::Hum sixty;
        sixty.frequency = 60.0;
        const auto both = tones ({ { 50.0, 0.3f }, { 60.0, 0.3f } }, 2.0);
        const auto bothBefore = readAll (*both);
        const auto bothAfter = readAll (*Restoration::removeHum (*both, 0, both->getNumFrames(), rate, sixty));
        const auto cut60 = db (amplitude (bothAfter, 0, 60.0, second, second) / amplitude (bothBefore, 0, 60.0, second, second));
        const auto kept50 = db (amplitude (bothAfter, 0, 50.0, second, second) / amplitude (bothBefore, 0, 50.0, second, second));
        check (cut60 < -40.0 && std::abs (kept50) < 0.5,
               "hum: at 60 Hz it takes 60 Hz out (" + juce::String (juce::roundToInt (-cut60)) + " dB) and leaves 50 Hz within " + juce::String (std::abs (kept50), 2) + " dB");

        Restoration::Hum high;
        high.frequency = 1000.0;
        Restoration::Hum low = {}, none = {}, sharp = {};
        low.frequency = 10.0;
        none.harmonics = 0;
        sharp.q = 300.0;
        check (Restoration::humFrequencies (high, 8000.0) == std::vector<double> { 1000.0, 2000.0, 3000.0 }
                   && Restoration::humFrequencies ({}, rate).size() == 8
                   && ! Restoration::isValid (low, rate) && ! Restoration::isValid (none, rate) && ! Restoration::isValid (sharp, rate)
                   && Restoration::removeHum (*source, 100, 100, rate, {}) == nullptr,
               "hum: harmonics stop below 0.45 of the sample rate; 10 Hz, no harmonics, a Q of 300 and an empty range are refused");
    }
}

void runRestorationTests()
{
    testHum();
}
