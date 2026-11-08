#include <JuceHeader.h>
#include "../Source/Breath.h"
#include "../Source/AudioEdits.h"
#include "TestUtils.h"
#include "BreathTests.h"

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

    /** Six seconds: words (a 160 Hz buzz at -20 dB, 400 ms long) every second from 0.3 s, with a
        breath (bright noise at -38 dB, 250 ms) 100 ms before the words at 2.3 s and 4.3 s, and room
        noise at -62 dB throughout. */
    juce::AudioBuffer<float> speech()
    {
        juce::Random r (13);
        juce::AudioBuffer<float> b (1, 6 * 48000);
        auto lastNoise = 0.0;

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const auto t = i / rate;
            auto v = 0.0008 * (r.nextDouble() * 2.0 - 1.0);
            const auto inWord = std::fmod (t - 0.3, 1.0) >= 0.0 && std::fmod (t - 0.3, 1.0) < 0.4 && t >= 0.3;

            if (inWord)
                for (int h = 1; h <= 10; ++h)
                    v += 0.1 / h * std::sin (juce::MathConstants<double>::twoPi * 160.0 * h * t);

            for (auto at : { 1.95, 3.95 })
                if (t >= at && t < at + 0.25)
                {
                    // Bright noise: white noise less its own last sample, a first difference.
                    const auto white = r.nextDouble() * 2.0 - 1.0;
                    v += 0.0126 * (white - lastNoise) * 0.7;
                    lastNoise = white;
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
}

void runBreathTests()
{
    std::cout << "Breath control:" << std::endl;

    // Two breaths are found where they are (within 30 ms) and nothing else; lowered by the
    // reduction; the words and the room noise untouched; a range under a tenth of a second gives nothing.
    {
        const auto in = speech();
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        Breath::Settings s;
        const auto found = Breath::find (*snap, 0, in.getNumSamples(), rate, s);
        auto placed = found.size() == 2;

        if (placed)
            placed = std::abs (found[0].start - 93600) < 1440 && std::abs (found[0].end - 105600) < 1440 && std::abs (found[1].start - 189600) < 1440 && std::abs (found[1].end - 201600) < 1440;

        const auto out = readAll (*Breath::apply (*snap, 0, in.getNumSamples(), rate, s));
        const auto breath1 = rmsDb (out, 95000, 104000) - rmsDb (in, 95000, 104000), breath2 = rmsDb (out, 191000, 200000) - rmsDb (in, 191000, 200000);
        const auto word = rmsDb (out, 112000, 128000) - rmsDb (in, 112000, 128000);
        auto room = true;

        for (int i = 40000; i < 60000; ++i)
            room = room && juce::exactlyEqual (out.getSample (0, i), in.getSample (0, i));

        const auto tooShort = Breath::find (*snap, 0, 4000, rate, s).empty() && Breath::apply (*snap, 0, 4000, rate, s) == nullptr;

        std::cout << "      breaths found: " << found.size();

        for (auto& f : found)
            std::cout << " [" << (double) f.start / rate << " to " << (double) f.end / rate << " s]";

        std::cout << "; lowered " << breath1 << " and " << breath2 << " dB; a word " << word << " dB" << std::endl;
        check (placed && breath1 < -11.0 && breath2 < -11.0 && std::abs (word) < 0.01 && room && tooShort,
               "breath control: the two breaths are found where they are (within 30 ms) and nothing else; lowered by over 11 dB of the 12; a word "
               "and the room noise between words are untouched; a range under a tenth of a second gives nothing");
    }
}
