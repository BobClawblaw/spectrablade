#include <JuceHeader.h>
#include "../Source/DialogueIsolation.h"
#include "../Source/AudioEdits.h"
#include "TestUtils.h"
#include "DialogueIsolationTests.h"

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

    /** Three seconds: a voice-like buzz (150 Hz, twenty harmonics falling 6 dB an octave) from 1 to
        2 s, over a steady background of noise at -30 dB and a 525 Hz tone at -24 dB throughout. */
    juce::AudioBuffer<float> scene (bool withVoice, bool withBackground)
    {
        juce::Random r (9);
        juce::AudioBuffer<float> b (1, 3 * 48000);

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const auto t = i / rate;
            auto v = 0.0;

            if (withVoice && t >= 1.0 && t < 2.0)
                for (int h = 1; h <= 20; ++h)
                    v += 0.25 / h * std::sin (juce::MathConstants<double>::twoPi * 150.0 * h * t);

            if (withBackground)
                v += 0.03 * (r.nextDouble() * 2.0 - 1.0) + 0.06 * std::sin (juce::MathConstants<double>::twoPi * 525.0 * t);

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

void runDialogueIsolationTests()
{
    std::cout << "Dialogue isolation:" << std::endl;

    // The pitch finder: a 150 Hz buzz reads 150 Hz with high voicing, noise low voicing.
    {
        const auto buzz = scene (true, false), noise = scene (false, true);
        juce::dsp::FFT fft (12);
        const auto powerOf = [&fft] (const juce::AudioBuffer<float>& b, int at)
        {
            std::vector<float> data (8192, 0.0f);

            for (int i = 0; i < 4096; ++i)
                data[(size_t) i] = b.getSample (0, at + i) * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / 4096));

            fft.performRealOnlyForwardTransform (data.data(), true);
            std::vector<float> p (2049);

            for (int k = 0; k < 2049; ++k)
                p[(size_t) k] = data[(size_t) (2 * k)] * data[(size_t) (2 * k)] + data[(size_t) (2 * k + 1)] * data[(size_t) (2 * k + 1)];

            return p;
        };
        const auto voiced = DialogueIsolation::pitchOf (powerOf (buzz, 72000), rate), unvoiced = DialogueIsolation::pitchOf (powerOf (noise, 72000), rate);
        std::cout << "      pitch: the buzz " << voiced.frequency << " Hz, voicing " << voiced.voicing << "; the background " << unvoiced.frequency << " Hz, voicing " << unvoiced.voicing << std::endl;
        check (std::abs (voiced.frequency - 150.0) < 150.0 * 0.045 && voiced.voicing > 0.8 && unvoiced.voicing < 0.55,
               "dialogue isolation: a 150 Hz buzz is found within a sixteenth of an octave with voicing over 0.8; noise with a tone has voicing under 0.55");
    }

    // The scene isolated: the background between the words (0 to 1 s, 2 to 3 s) lowered by about
    // the reduction, the voice's harmonics kept within 1.5 dB, the 525 Hz tone lowered during the
    // voice too (it is not a harmonic of 150 Hz); a clean voice passes within 0.5 dB; cancelling.
    {
        const auto in = scene (true, true);
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        DialogueIsolation::Settings s;
        s.reductionDb = 12.0f;
        const auto out = readAll (*DialogueIsolation::isolate (*snap, 0, in.getNumSamples(), rate, s));
        const auto gapBefore = rmsDb (out, 12000, 44000) - rmsDb (in, 12000, 44000), gapAfter = rmsDb (out, 100000, 136000) - rmsDb (in, 100000, 136000);
        auto worstHarmonic = 0.0;

        for (int h = 1; h <= 8; ++h)
            worstHarmonic = juce::jmax (worstHarmonic, std::abs (20.0 * std::log10 (amplitude (out, 150.0 * h, 56000, 24000) / amplitude (in, 150.0 * h, 56000, 24000))));

        const auto toneDuring = 20.0 * std::log10 (amplitude (out, 525.0, 56000, 24000) / amplitude (in, 525.0, 56000, 24000));

        const auto clean = scene (true, false);
        const auto cleanOut = readAll (*DialogueIsolation::isolate (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (clean)), 0, clean.getNumSamples(), rate, s));
        const auto cleanKept = rmsDb (cleanOut, 52000, 92000) - rmsDb (clean, 52000, 92000);

        const auto cancelled = DialogueIsolation::isolate (*snap, 0, in.getNumSamples(), rate, s, [] (float) { return false; }) == nullptr;
        DialogueIsolation::Settings bad;
        bad.reductionDb = 50.0f;
        const auto refused = DialogueIsolation::isolate (*snap, 0, in.getNumSamples(), rate, bad) == nullptr && DialogueIsolation::isolate (*snap, 100, 100, rate, s) == nullptr;

        std::cout << "      isolated: the background before the voice " << gapBefore << " dB, after " << gapAfter << "; the voice's harmonics moved at most " << worstHarmonic
                  << " dB; the 525 Hz tone during the voice " << toneDuring << " dB; a clean voice " << cleanKept << " dB" << std::endl;
        check (gapBefore < -9.0 && gapAfter < -9.0 && worstHarmonic < 1.5 && toneDuring < -6.0 && std::abs (cleanKept) < 0.5 && cancelled && refused,
               "dialogue isolation: with a 12 dB reduction the background between the words comes down over 9 dB, the voice's first eight harmonics stay "
               "within 1.5 dB, the 525 Hz tone under the voice (between its harmonics) comes down over 6 dB, a clean voice passes within 0.5 dB; it can be cancelled; a reduction "
               "past 40 dB and an empty range are refused");
    }
}
