#include "Restoration.h"

namespace Restoration
{
    namespace
    {
        /** A notch (RBJ's cookbook), in doubles, run one sample at a time. */
        struct Notch
        {
            double b0 = 1.0, b1 = 0.0, b2 = 1.0, a1 = 0.0, a2 = 0.0;
            double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

            Notch (double frequency, double q, double sampleRate)
            {
                const auto w = juce::MathConstants<double>::twoPi * frequency / sampleRate;
                const auto alpha = std::sin (w) / (2.0 * q);
                const auto a0 = 1.0 + alpha;
                b0 = 1.0 / a0;
                b1 = -2.0 * std::cos (w) / a0;
                b2 = 1.0 / a0;
                a1 = b1;
                a2 = (1.0 - alpha) / a0;
            }

            double run (double x) noexcept
            {
                const auto y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
                x2 = x1; x1 = x;
                y2 = y1; y1 = y;
                return y;
            }
        };
    }

    bool isValid (const Hum& h, double sampleRate)
    {
        return sampleRate > 0.0 && h.frequency >= 20.0 && h.frequency <= 1000.0 && h.harmonics >= 1 && h.harmonics <= 40
               && h.q >= 5.0 && h.q <= 200.0 && h.frequency < 0.45 * sampleRate;
    }

    std::vector<double> humFrequencies (const Hum& h, double sampleRate)
    {
        std::vector<double> out;

        for (int k = 1; k <= h.harmonics && k * h.frequency < 0.45 * sampleRate; ++k)
            out.push_back (k * h.frequency);

        return out;
    }

    AudioEdits::Snapshot removeHum (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Hum& h)
    {
        start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
        end = juce::jlimit (start, s.getNumFrames(), end);

        if (start == end || ! isValid (h, sampleRate) || end - start > std::numeric_limits<int>::max() / 2)
            return nullptr;

        const auto frequencies = humFrequencies (h, sampleRate);
        const auto second = (int) std::ceil (sampleRate);
        const auto prime = (int) juce::jmin (start, (juce::int64) second);

        return AudioEdits::processRange (s, start - prime, end, [&] (juce::AudioBuffer<float>& b)
        {
            const auto n = b.getNumSamples();
            // Where there isn't a second before, the notches settle first on the buffer's opening
            // audio turned about its first frame (2 x[0] - x[i], in reverse): it runs on into the
            // audio with neither a jump nor a kink, as a sine's own past would.
            const auto mirrored = juce::jmin (second - prime, n - 1);

            for (int ch = 0; ch < b.getNumChannels(); ++ch)
            {
                auto* x = b.getWritePointer (ch);
                std::vector<Notch> notches;

                for (auto f : frequencies)
                    notches.emplace_back (f, h.q * f / h.frequency, sampleRate);   // the same width in Hz at each harmonic

                const auto filter = [&notches] (double v)
                {
                    for (auto& notch : notches)
                        v = notch.run (v);

                    return v;
                };

                for (int i = mirrored; i > 0; --i)
                    filter (2.0 * x[0] - x[i]);

                for (int i = 0; i < prime; ++i)
                    filter (x[i]);   // the audio before: settling only, left as it was

                for (int i = prime; i < n; ++i)
                    x[i] = (float) filter (x[i]);
            }
        });
    }
}
