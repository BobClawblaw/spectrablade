// Samples outside an edit are compared bit for bit on purpose.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioEdits.h"
#include "../Source/NoiseReduction.h"
#include "TestUtils.h"
#include "NoiseTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;
    constexpr double twoPi = juce::MathConstants<double>::twoPi;
    constexpr double toneHz = 85 * rate / 4096;   // on a bin: 996.1 Hz
    constexpr int second = (int) rate;

    double toDb (double gain)                     { return 20.0 * std::log10 (juce::jmax (gain, 1.0e-30)); }

    /** Stereo, 8 s: white noise at -40 dBFS RMS throughout, and a -12 dBFS tone from 4 s on. */
    juce::AudioBuffer<float> toneInNoise()
    {
        juce::AudioBuffer<float> b (2, 8 * second);
        juce::Random random (1234);
        const auto noise = std::pow (10.0, -40.0 / 20.0) * std::sqrt (3.0);   // uniform: RMS = a / sqrt 3

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (ch, i, (float) (noise * (2.0 * random.nextDouble() - 1.0)
                                             + (i >= 4 * second ? 0.25 * std::sin (twoPi * toneHz * i / rate) : 0.0)));

        return b;
    }

    AudioDocument::Snapshot snapshotOf (const juce::AudioBuffer<float>& b)
    {
        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (juce::AudioBuffer<float> (b)), 0, b.getNumSamples() } };
        return std::make_shared<const AudioSnapshot> (b.getNumChannels(), std::move (segments));
    }

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    double rms (const juce::AudioBuffer<float>& b, int start, int count)
    {
        double sum = 0.0;

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = start; i < start + count; ++i)
                sum += (double) b.getSample (ch, i) * b.getSample (ch, i);

        return std::sqrt (sum / (count * b.getNumChannels()));
    }

    /** Amplitude of the tone in channel 0 over a whole number of its periods. */
    double toneAmplitude (const juce::AudioBuffer<float>& b, int start, int count)
    {
        double re = 0.0, im = 0.0;

        for (int i = start; i < start + count; ++i)
        {
            re += b.getSample (0, i) * std::cos (twoPi * toneHz * i / rate);
            im += b.getSample (0, i) * std::sin (twoPi * toneHz * i / rate);
        }

        return 2.0 * std::sqrt (re * re + im * im) / count;
    }
}

void runNoiseTests()
{
    const auto input = toneInNoise();
    const auto snapshot = snapshotOf (input);

    check (! NoiseReduction::capture (*snapshot, 0, NoiseReduction::frameSize - 1, rate).has_value(),
           "noise print: refuses less than one frame");

    const auto print = NoiseReduction::capture (*snapshot, 0, 2 * second, rate);
    check (print.has_value() && print->power.size() == 2, "noise print: captured from 2 s of noise");

    if (! print.has_value())
        return;

    // Whole file, 12 dB.
    {
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const auto out = NoiseReduction::reduce (*snapshot, 0, snapshot->getNumFrames(), rate, *print, {});
        const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
        check (out != nullptr && out->getNumFrames() == snapshot->getNumFrames(), "noise reduction: keeps the length");

        const auto result = readAll (*out);

        // Measured where the print was not taken.
        const auto before = rms (input, 2 * second + 4096, second);
        const auto after = rms (result, 2 * second + 4096, second);
        const auto lowered = toDb (before / after);
        check (lowered > 11.0 && lowered < 12.5, "noise reduction: noise lowered by " + juce::String (lowered, 2) + " dB (asked 12)");

        const auto toneBefore = toneAmplitude (input, 5 * second, 85 * 4800);
        const auto toneAfter = toneAmplitude (result, 5 * second, 85 * 4800);
        const auto toneChange = toDb (toneAfter / toneBefore);
        check (std::abs (toneChange) < 0.1, "noise reduction: the tone keeps its level (" + juce::String (toneChange, 3) + " dB)");

        // What is left besides the tone, where the tone plays: the noise is lowered there too.
        juce::AudioBuffer<float> residual (result);
        juce::AudioBuffer<float> residualIn (input);

        for (int i = 0; i < residual.getNumSamples(); ++i)
        {
            const auto tone = (float) (0.25 * std::sin (twoPi * toneHz * i / rate)) * (i >= 4 * second ? 1.0f : 0.0f);

            for (int ch = 0; ch < 2; ++ch)
            {
                residual.setSample (ch, i, residual.getSample (ch, i) - tone);
                residualIn.setSample (ch, i, residualIn.getSample (ch, i) - tone);
            }
        }

        const auto underTone = toDb (rms (residualIn, 5 * second, 2 * second) / rms (residual, 5 * second, 2 * second));
        check (underTone > 6.0, "noise reduction: noise under the tone lowered by " + juce::String (underTone, 2) + " dB");

        std::cout << "      (8 s of stereo in " << std::llround (ms) << " ms)" << std::endl;
    }

    // A deeper reduction reaches its floor too.
    {
        NoiseReduction::Settings deep;
        deep.reductionDb = 24.0f;
        const auto result = readAll (*NoiseReduction::reduce (*snapshot, 0, snapshot->getNumFrames(), rate, *print, deep));
        const auto lowered = toDb (rms (input, 2 * second + 4096, second) / rms (result, 2 * second + 4096, second));
        check (lowered > 22.0 && lowered < 24.5, "noise reduction: noise lowered by " + juce::String (lowered, 2) + " dB (asked 24)");
        check (std::abs (toDb (toneAmplitude (result, 5 * second, 85 * 4800) / toneAmplitude (input, 5 * second, 85 * 4800))) < 0.1,
               "noise reduction: at 24 dB the tone still keeps its level");
    }

    // 0 dB: gains are all one, so the audio comes back.
    {
        NoiseReduction::Settings none;
        none.reductionDb = 0.0f;
        const auto result = readAll (*NoiseReduction::reduce (*snapshot, 0, snapshot->getNumFrames(), rate, *print, none));
        float worst = 0.0f;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < result.getNumSamples(); ++i)
                worst = juce::jmax (worst, std::abs (result.getSample (ch, i) - input.getSample (ch, i)));

        check (toDb (worst) < -120.0, "noise reduction: 0 dB gives the input back (worst difference " + juce::String (toDb (worst), 1) + " dB)");
    }

    // Part of the file: audio more than a frame outside the range is untouched.
    {
        const auto result = readAll (*NoiseReduction::reduce (*snapshot, 3 * second, 6 * second, rate, *print, {}));
        bool untouched = true;

        for (int ch = 0; ch < 2; ++ch)
        {
            for (int i = 0; i < result.getNumSamples(); ++i)
                if (i < 3 * second - NoiseReduction::frameSize || i >= 6 * second + NoiseReduction::frameSize)
                    untouched = untouched && result.getSample (ch, i) == input.getSample (ch, i);
        }

        check (untouched, "noise reduction: audio outside the range is bit-identical");

        const auto inside = toDb (rms (input, 3 * second + 4096, second / 2) / rms (result, 3 * second + 4096, second / 2));
        check (inside > 11.0, "noise reduction: the range itself is reduced (" + juce::String (inside, 2) + " dB)");
    }

    // A mono print is spread over both channels.
    {
        juce::AudioBuffer<float> mono (1, 2 * second);
        mono.copyFrom (0, 0, input, 0, 0, 2 * second);
        const auto monoPrint = NoiseReduction::capture (*snapshotOf (mono), 0, 2 * second, rate);
        const auto result = readAll (*NoiseReduction::reduce (*snapshot, 0, snapshot->getNumFrames(), rate, *monoPrint, {}));
        const auto lowered = toDb (rms (input, 2 * second + 4096, second) / rms (result, 2 * second + 4096, second));
        check (lowered > 11.0, "noise reduction: a mono print works on stereo (" + juce::String (lowered, 2) + " dB)");
    }

    // Cancelling.
    {
        int calls = 0;
        const auto out = NoiseReduction::reduce (*snapshot, 0, snapshot->getNumFrames(), rate, *print, {},
                                                 [&] (float) { return ++calls < 2; });
        check (out == nullptr, "noise reduction: can be cancelled");
    }

    // Speed on a longer file: 5 minutes of stereo.
    {
        juce::AudioBuffer<float> longer (2, 300 * second);

        for (int part = 0; part < 300 / 8; ++part)
            for (int ch = 0; ch < 2; ++ch)
                longer.copyFrom (ch, part * 8 * second, input, ch, 0, 8 * second);

        const auto big = snapshotOf (longer);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const auto out = NoiseReduction::reduce (*big, 0, big->getNumFrames(), rate, *print, {});
        const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
        check (out != nullptr, "noise reduction: 5 minutes of stereo in " + juce::String (ms / 1000.0, 2) + " s");
    }
}
