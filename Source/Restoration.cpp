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

    namespace
    {
        /** The cubic a + b t + c t^2 + d t^3 best fitting the points (least squares). */
        std::array<double, 4> fitCubic (const std::vector<std::pair<double, double>>& points)
        {
            double m[4][5] {};

            for (auto [t, y] : points)
            {
                const double p[4] { 1.0, t, t * t, t * t * t };

                for (int r = 0; r < 4; ++r)
                {
                    for (int k = 0; k < 4; ++k)
                        m[r][k] += p[r] * p[k];

                    m[r][4] += p[r] * y;
                }
            }

            // Gaussian elimination with partial pivoting.
            for (int col = 0; col < 4; ++col)
            {
                int pivot = col;

                for (int r = col + 1; r < 4; ++r)
                    if (std::abs (m[r][col]) > std::abs (m[pivot][col]))
                        pivot = r;

                for (int k = 0; k < 5; ++k)
                    std::swap (m[col][k], m[pivot][k]);

                if (std::abs (m[col][col]) < 1.0e-12)
                    return { 0.0, 0.0, 0.0, 0.0 };

                for (int r = 0; r < 4; ++r)
                {
                    if (r == col)
                        continue;

                    const auto f = m[r][col] / m[col][col];

                    for (int k = col; k < 5; ++k)
                        m[r][k] -= f * m[col][k];
                }
            }

            return { m[0][4] / m[0][0], m[1][4] / m[1][1], m[2][4] / m[2][2], m[3][4] / m[3][3] };
        }
    }

    DeclipResult declip (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Declip& settings)
    {
        DeclipResult result;
        start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
        end = juce::jlimit (start, s.getNumFrames(), end);

        if (start == end || sampleRate <= 0.0 || settings.threshold < 0.5 || settings.threshold > 1.0
            || end - start > std::numeric_limits<int>::max() / 2)
            return result;

        const auto longest = juce::jmax (2, (int) (maxRunSeconds * sampleRate));
        float clippedLevel = 0.0f, rebuiltPeak = 0.0f;

        auto audio = AudioEdits::processRange (s, start, end, [&] (juce::AudioBuffer<float>& b)
        {
            const auto n = b.getNumSamples();

            for (int ch = 0; ch < b.getNumChannels(); ++ch)
            {
                auto* x = b.getWritePointer (ch);
                const auto peak = b.getMagnitude (ch, 0, n);

                if (peak <= 0.0f)
                    continue;

                const auto level = (float) (settings.threshold * peak);
                const auto clipped = [&] (int i, float sign) { return x[i] * sign >= level; };

                for (int i = 0; i < n;)
                {
                    const auto sign = x[i] >= 0.0f ? 1.0f : -1.0f;

                    if (! clipped (i, sign))
                    {
                        ++i;
                        continue;
                    }

                    // A run holds the samples level with its first: clipping leaves them equal, while
                    // even a slow sine's rounded top changes from one sample to the next.
                    int j = i + 1;

                    while (j < n && clipped (j, sign) && std::abs (std::abs (x[j]) - std::abs (x[i])) <= flatness * peak)
                        ++j;

                    const auto length = j - i;

                    if (length >= 2)
                    {
                        if (length > longest)
                        {
                            ++result.tooLong;
                        }
                        else if (i >= fitPoints && j + fitPoints <= n)
                        {
                            std::vector<std::pair<double, double>> points;

                            for (int k = i - fitPoints; k < i; ++k)
                                points.emplace_back ((double) (k - i), x[k]);

                            for (int k = j; k < j + fitPoints; ++k)
                                points.emplace_back ((double) (k - i), x[k]);

                            const auto c = fitCubic (points);
                            const auto runLevel = juce::jmin (std::abs (x[i]), std::abs (x[j - 1]));

                            for (int k = i; k < j; ++k)
                            {
                                const auto t = (double) (k - i);
                                const auto y = (float) (c[0] + t * (c[1] + t * (c[2] + t * c[3])));
                                x[k] = sign * juce::jmax (y * sign, runLevel);   // never back inside the clipped level
                            }

                            clippedLevel = juce::jmax (clippedLevel, runLevel);
                            ++result.repaired;
                        }
                    }

                    i = j;
                }
            }

            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                rebuiltPeak = juce::jmax (rebuiltPeak, b.getMagnitude (ch, 0, n));

            if (settings.keepPeak && result.repaired > 0 && rebuiltPeak > clippedLevel && clippedLevel > 0.0f)
                b.applyGain (clippedLevel / rebuiltPeak);
        });

        if (result.repaired > 0)
            result.audio = std::move (audio);

        return result;
    }
}
