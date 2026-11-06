#include "PhaseCorrection.h"
#include "AudioEdits.h"

namespace PhaseCorrection
{
namespace
{
    constexpr int halfTaps = 32;                // the sinc's taps either side
    constexpr double easeSeconds = 0.01;
    constexpr int outputBlockFrames = 1 << 20;

    /** The sinc, windowed (Blackman), for reading `frac` of a frame past a sample: tap j (from
        -halfTaps + 1 to halfTaps) weighs the sample j frames on. */
    std::vector<float> kernelFor (double frac)
    {
        std::vector<float> k ((size_t) (2 * halfTaps), 0.0f);

        // On a sample: that sample, exactly.
        if (juce::exactlyEqual (frac, 0.0))
        {
            k[(size_t) (halfTaps - 1)] = 1.0f;
            return k;
        }

        double sum = 0.0;

        for (int j = -halfTaps + 1; j <= halfTaps; ++j)
        {
            const auto t = (double) j - frac;
            const auto sinc = std::abs (t) < 1.0e-12 ? 1.0 : std::sin (juce::MathConstants<double>::pi * t) / (juce::MathConstants<double>::pi * t);
            const auto w = std::abs (t) >= halfTaps ? 0.0
                                                    : 0.42 + 0.5 * std::cos (juce::MathConstants<double>::pi * t / halfTaps)
                                                          + 0.08 * std::cos (juce::MathConstants<double>::twoPi * t / halfTaps);
            k[(size_t) (j + halfTaps - 1)] = (float) (sinc * w);
            sum += sinc * w;
        }

        // Unity at DC, so a steady level passes as it is.
        for (auto& v : k)
            v = (float) (v / sum);

        return k;
    }
}

std::optional<Finding> measure (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate, double mostMs,
                                const std::function<bool (float)>& progress)
{
    const auto length = snapshot.getNumFrames();
    start = juce::jlimit ((juce::int64) 0, length, start);
    end = juce::jlimit (start, length, end);

    if (snapshot.getNumChannels() != 2 || sampleRate <= 0.0 || ! std::isfinite (mostMs) || mostMs < shortestMs || mostMs > longestMs)
        return {};

    const auto maxLag = juce::jmax (1, (int) std::ceil (mostMs / 1000.0 * sampleRate));

    if (end - start < juce::jmax ((juce::int64) 1024, (juce::int64) 4 * maxLag))
        return {};

    // Blocks of the left channel, each against the right over the block and maxLag either side,
    // so every lag looked for is summed over every frame.
    int order = 14;

    while ((1 << order) < 4 * maxLag)
        ++order;

    const auto block = 1 << order, size = 2 * block, bins = size / 2 + 1;
    juce::dsp::FFT fft (order + 1);
    std::vector<float> x ((size_t) size * 2), y ((size_t) size * 2);
    std::vector<double> cross ((size_t) bins * 2, 0.0);   // the cross-spectrum, summed: conj (L) R
    juce::AudioBuffer<float> left (2, block), right (2, block + 2 * maxLag);
    double leftPower = 0.0, rightPower = 0.0;
    const auto blocks = (end - start + block - 1) / block;
    juce::int64 done = 0;

    for (auto pos = start; pos < end; pos += block, ++done)
    {
        const auto n = (int) juce::jmin ((juce::int64) block, end - pos);
        snapshot.read (left, 0, pos, n);
        snapshot.read (right, 0, pos - maxLag, n + 2 * maxLag);
        std::fill (x.begin(), x.end(), 0.0f);
        std::fill (y.begin(), y.end(), 0.0f);

        for (int i = 0; i < n; ++i)
        {
            const auto l = left.getSample (0, i), r = right.getSample (1, i + maxLag);
            x[(size_t) i] = l;
            leftPower += (double) l * l;
            rightPower += (double) r * r;
        }

        for (int i = 0; i < n + 2 * maxLag; ++i)
            y[(size_t) i] = right.getSample (1, i);

        fft.performRealOnlyForwardTransform (x.data(), true);
        fft.performRealOnlyForwardTransform (y.data(), true);

        for (int k = 0; k < bins; ++k)
        {
            const double xr = x[(size_t) (2 * k)], xi = x[(size_t) (2 * k + 1)], yr = y[(size_t) (2 * k)], yi = y[(size_t) (2 * k + 1)];
            cross[(size_t) (2 * k)] += xr * yr + xi * yi;
            cross[(size_t) (2 * k + 1)] += xr * yi - xi * yr;
        }

        if (progress != nullptr && ! progress ((float) ((double) (done + 1) / (double) blocks)))
            return {};
    }

    if (leftPower <= 0.0 || rightPower <= 0.0)
        return {};

    // The correlation at a lag of `at` frames (the right channel's segment starting maxLag early,
    // so lag 0 is at maxLag), from the cross-spectrum: exact between whole frames too.
    const auto correlationAt = [&] (double at)
    {
        auto sum = cross[0] + cross[(size_t) (2 * (bins - 1))] * std::cos (juce::MathConstants<double>::pi * at);

        for (int k = 1; k < bins - 1; ++k)
        {
            const auto w = juce::MathConstants<double>::twoPi * k * at / size;
            sum += 2.0 * (cross[(size_t) (2 * k)] * std::cos (w) - cross[(size_t) (2 * k + 1)] * std::sin (w));
        }

        return sum / size / std::sqrt (leftPower * rightPower);
    };

    // At whole frames, by the inverse transform; the peak either way.
    std::fill (x.begin(), x.end(), 0.0f);

    for (int k = 0; k < bins * 2; ++k)
        x[(size_t) k] = (float) cross[(size_t) k];

    fft.performRealOnlyInverseTransform (x.data());
    auto peak = 0;

    for (int k = 1; k <= 2 * maxLag; ++k)
        if (std::abs (x[(size_t) k]) > std::abs (x[(size_t) peak]))
            peak = k;

    if (peak == 0 || peak == 2 * maxLag)
        return {};

    // Between frames: the peak of the correlation, turned up the right way, within a frame of it.
    const auto sign = x[(size_t) peak] < 0.0f ? -1.0 : 1.0;
    auto lo = peak - 1.0, hi = peak + 1.0;
    const auto golden = (std::sqrt (5.0) - 1.0) / 2.0;
    auto a = hi - golden * (hi - lo), b = lo + golden * (hi - lo);
    auto fa = sign * correlationAt (a), fb = sign * correlationAt (b);

    for (int i = 0; i < 48; ++i)
    {
        if (fa > fb)
        {
            hi = b;
            b = a;
            fb = fa;
            a = hi - golden * (hi - lo);
            fa = sign * correlationAt (a);
        }
        else
        {
            lo = a;
            a = b;
            fa = fb;
            b = lo + golden * (hi - lo);
            fb = sign * correlationAt (b);
        }
    }

    const auto at = 0.5 * (lo + hi);
    return Finding { at - maxLag, correlationAt (at) };
}

AudioDocument::Snapshot correct (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate, double offset, bool turnOver)
{
    const auto length = snapshot.getNumFrames();
    start = juce::jlimit ((juce::int64) 0, length, start);
    end = juce::jlimit (start, length, end);

    if (end <= start || snapshot.getNumChannels() != 2 || sampleRate <= 0.0 || ! std::isfinite (offset))
        return nullptr;

    // The left channel is read half the offset back, the right half of it on.
    const double shifts[2] = { -0.5 * offset, 0.5 * offset };
    std::vector<float> kernels[2];
    juce::int64 whole[2];

    for (int ch = 0; ch < 2; ++ch)
    {
        whole[ch] = (juce::int64) std::floor (shifts[ch]);
        kernels[ch] = kernelFor (shifts[ch] - (double) whole[ch]);
    }

    const auto margin = (juce::int64) std::ceil (std::abs (0.5 * offset)) + halfTaps + 1;
    const auto ease = juce::jmax ((juce::int64) 1, juce::jmin ((juce::int64) std::lround (easeSeconds * sampleRate), (end - start) / 2));
    const auto easeIn = start > 0, easeOut = end < length;
    std::vector<AudioSnapshot::Segment> segments;
    juce::AudioBuffer<float> source (2, outputBlockFrames + (int) (2 * margin));

    for (auto from = start; from < end; from += outputBlockFrames)
    {
        const auto n = (int) juce::jmin ((juce::int64) outputBlockFrames, end - from);
        snapshot.read (source, 0, from - margin, n + (int) (2 * margin));
        juce::AudioBuffer<float> out (2, n);

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto* s = source.getReadPointer (ch);
            auto* d = out.getWritePointer (ch);
            const auto& k = kernels[ch];
            const auto gain = ch == 1 && turnOver ? -1.0f : 1.0f;

            for (int i = 0; i < n; ++i)
            {
                // Frame from + i of the output reads from + i + shift of the input.
                const auto base = (juce::int64) i + margin + whole[ch] - halfTaps + 1;
                double v = 0.0;

                for (int j = 0; j < 2 * halfTaps; ++j)
                    v += (double) k[(size_t) j] * s[base + j];

                auto value = gain * (float) v;
                const auto frame = from + i;
                const auto before = s[i + margin];

                if (easeIn && frame - start < ease)
                    value = before + (value - before) * (float) ((double) (frame - start) / (double) ease);
                else if (easeOut && end - 1 - frame < ease)
                    value = before + (value - before) * (float) ((double) (end - 1 - frame) / (double) ease);

                d[i] = value;
            }
        }

        segments.push_back ({ std::make_shared<const AudioBlock> (std::move (out)), 0, n });
    }

    const AudioClip clip { 2, sampleRate, std::move (segments) };
    return AudioEdits::replaceWithClip (snapshot, start, end, clip);
}
}
