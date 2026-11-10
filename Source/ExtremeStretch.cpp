#include "ExtremeStretch.h"
#include "TimeStretch.h"

namespace ExtremeStretch
{
    bool isValid (const Settings& s)
    {
        return std::isfinite (s.factor) && s.factor >= shortestFactor && s.factor <= longestFactor
               && std::isfinite (s.windowSeconds) && s.windowSeconds >= shortestWindow && s.windowSeconds <= longestWindow;
    }

    int windowSize (double windowSeconds, double sampleRate)
    {
        const auto wanted = juce::jlimit (shortestWindow, longestWindow, windowSeconds) * juce::jmax (1.0, sampleRate);
        const auto order = juce::jlimit (6, 20, juce::roundToInt (std::log2 (wanted)));
        return 1 << order;
    }

    juce::AudioBuffer<float> stretch (const juce::AudioBuffer<float>& input, const Settings& settings, double sampleRate,
                                      const std::function<bool (float)>& progress)
    {
        const auto channels = input.getNumChannels();
        const auto inLength = input.getNumSamples();
        const auto factor = juce::jlimit (shortestFactor, longestFactor, settings.factor);
        const auto outLength = (int) juce::jmin ((double) std::numeric_limits<int>::max() / 2.0, std::round ((double) inLength * factor));
        juce::AudioBuffer<float> output (channels, outLength);
        output.clear();

        if (channels == 0 || inLength == 0 || outLength == 0)
            return output;

        // Windows of size n laid down every n / 2 samples of the output, taken from the input at
        // a pace `factor` times slower. The analysis window is a Hann; the synthesis window its
        // square root, whose squares sum to one across the overlap, so the level of the random-
        // phase frames (uncorrelated, so they add in power) is even. The gain puts back what the
        // analysis window took (its mean square, 3/8 for a Hann).
        const auto n = windowSize (settings.windowSeconds, sampleRate);
        const auto hop = n / 2;
        const auto order = (int) std::log2 (n);
        juce::dsp::FFT fft (order);
        std::vector<float> analysis ((size_t) n), synthesis ((size_t) n);
        double windowPower = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const auto hann = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * (i + 0.5) / n);
            analysis[(size_t) i] = (float) hann;
            synthesis[(size_t) i] = (float) std::sqrt (hann);
            windowPower += hann * hann;
        }

        const auto gain = (float) (1.0 / std::sqrt (windowPower / n));
        const auto hops = (outLength + hop - 1) / hop + 1;
        std::vector<float> frame ((size_t) n * 2), phases ((size_t) n / 2 + 1);
        juce::Random random (0x5eed5);   // the same phases every time, so a stretch repeats bit for bit

        // From one window before the start, so the first output has two windows over it as the
        // rest does.
        for (int k = -1; k < hops; ++k)
        {
            if (progress != nullptr && ! progress ((float) (k + 1) / (float) (hops + 1)))
                return {};

            const auto outAt = k * hop;
            const auto inAt = (int) std::llround ((double) outAt / factor) - hop;   // the window centred where this output lands

            for (auto& p : phases)
                p = random.nextFloat() * juce::MathConstants<float>::twoPi;

            for (int ch = 0; ch < channels; ++ch)
            {
                std::fill (frame.begin(), frame.end(), 0.0f);
                const auto* in = input.getReadPointer (ch);

                for (int i = 0; i < n; ++i)
                {
                    const auto source = inAt + i;

                    if (source >= 0 && source < inLength)
                        frame[(size_t) i] = in[source] * analysis[(size_t) i];
                }

                fft.performRealOnlyForwardTransform (frame.data(), true);

                // Each bin's magnitude with a phase of its own; the mirrored half conjugate, as a
                // real signal's spectrum is. The first and the middle bin stay real.
                for (int b = 0; b <= n / 2; ++b)
                {
                    const auto re = frame[(size_t) (2 * b)], im = frame[(size_t) (2 * b + 1)];
                    const auto magnitude = std::sqrt (re * re + im * im);
                    const auto phase = b == 0 || b == n / 2 ? 0.0f : phases[(size_t) b];
                    frame[(size_t) (2 * b)] = magnitude * std::cos (phase);
                    frame[(size_t) (2 * b + 1)] = magnitude * std::sin (phase);

                    if (b > 0 && b < n / 2)
                    {
                        frame[(size_t) (2 * (n - b))] = frame[(size_t) (2 * b)];
                        frame[(size_t) (2 * (n - b) + 1)] = -frame[(size_t) (2 * b + 1)];
                    }
                }

                fft.performRealOnlyInverseTransform (frame.data());

                auto* out = output.getWritePointer (ch);
                const auto first = juce::jmax (0, -outAt), count = juce::jmin (n, outLength - outAt);

                for (int i = first; i < count; ++i)
                    out[outAt + i] += frame[(size_t) i] * synthesis[(size_t) i] * gain;
            }
        }

        if (progress != nullptr)
            progress (1.0f);

        return output;
    }

    AudioDocument::Snapshot stretchRange (const AudioSnapshot& audio, juce::int64 start, juce::int64 end, const Settings& settings, double rate,
                                          const std::function<bool (float)>& progress)
    {
        if (! isValid (settings))
            return nullptr;

        return TimeStretch::processRange (audio, start, end, settings.factor, rate,
                                          [&] (const juce::AudioBuffer<float>& in) { return stretch (in, settings, rate, progress); });
    }
}
