#include "WowFlutter.h"
#include "AudioEdits.h"

namespace WowFlutter
{
namespace
{
    constexpr int fftOrder = 12, size = 1 << fftOrder, hop = size / 4, bins = size / 2 + 1;
    constexpr double matchWithin = 0.03, peakFloorDb = -60.0;

    struct Peak { double frequency, power; };

    /** The spectral peaks of a frame (bins over both neighbours and over the floor), each placed
        between bins by the parabola through it and its neighbours (in log power). */
    std::vector<Peak> peaksOf (const std::vector<float>& power, double binHz)
    {
        std::vector<Peak> peaks;
        const auto floor = (float) std::pow (10.0, peakFloorDb / 10.0);

        for (int k = 2; k < bins - 2; ++k)
        {
            const auto p = power[(size_t) k];

            if (p < floor || p <= power[(size_t) k - 1] || p < power[(size_t) k + 1])
                continue;

            const auto a = std::log (juce::jmax (1.0e-20f, power[(size_t) k - 1])), b = std::log (p), c = std::log (juce::jmax (1.0e-20f, power[(size_t) k + 1]));
            const auto denominator = a - 2.0 * b + c;
            const auto offset = std::abs (denominator) > 1.0e-12 ? 0.5 * (a - c) / denominator : 0.0;
            peaks.push_back ({ (k + juce::jlimit (-0.5, 0.5, offset)) * binHz, (double) p });
        }

        return peaks;
    }

    /** The weighted median of values. */
    double weightedMedian (std::vector<std::pair<double, double>> valuesAndWeights)
    {
        if (valuesAndWeights.empty())
            return 1.0;

        std::sort (valuesAndWeights.begin(), valuesAndWeights.end());
        auto total = 0.0;

        for (auto& [v, w] : valuesAndWeights)
            total += w;

        auto sum = 0.0;

        for (auto& [v, w] : valuesAndWeights)
        {
            sum += w;

            if (sum >= total / 2.0)
                return v;
        }

        return valuesAndWeights.back().first;
    }
}

bool isValid (const Settings& s)
{
    return s.fastestHz >= 0.5 && s.fastestHz <= 20.0 && s.amountPercent >= 0.0 && s.amountPercent <= 100.0;
}

Drift measure (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings,
               const std::function<bool (float)>& progress)
{
    Drift d;
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (! isValid (settings) || sampleRate <= 0.0 || end - start < (juce::int64) (0.5 * sampleRate) || end - start > std::numeric_limits<int>::max() / 4)
        return d;

    const auto channels = s.getNumChannels();
    const auto frames = (int) (end - start);
    const auto steps = (frames - size) / hop + 1;
    d.hop = hop;
    juce::dsp::FFT fft (fftOrder);
    std::vector<float> data ((size_t) (2 * size)), power ((size_t) bins), hann ((size_t) size);
    juce::AudioBuffer<float> block (channels, size);

    for (int i = 0; i < size; ++i)
        hann[(size_t) i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / size));

    // Each frame's speed change against the frame before, from its peaks; the changes summed (in
    // logs) into the speed curve.
    std::vector<Peak> previous;
    std::vector<double> logSpeed ((size_t) steps, 0.0);
    const auto binHz = sampleRate / size;

    for (int step = 0; step < steps; ++step)
    {
        if (progress && (step % 16) == 0 && ! progress (0.8f * (float) step / (float) steps))
            return {};

        s.read (block, 0, start + (juce::int64) step * hop, size);
        std::fill (data.begin(), data.end(), 0.0f);

        for (int i = 0; i < size; ++i)
        {
            auto mix = 0.0f;

            for (int ch = 0; ch < channels; ++ch)
                mix += block.getSample (ch, i);

            data[(size_t) i] = mix / (float) channels * hann[(size_t) i];
        }

        fft.performRealOnlyForwardTransform (data.data(), true);

        for (int k = 0; k < bins; ++k)
            power[(size_t) k] = data[(size_t) (2 * k)] * data[(size_t) (2 * k)] + data[(size_t) (2 * k + 1)] * data[(size_t) (2 * k + 1)];

        auto peaks = peaksOf (power, binHz);
        std::vector<std::pair<double, double>> ratios;

        for (const auto& p : peaks)
        {
            const Peak* nearest = nullptr;

            for (const auto& q : previous)
                if (std::abs (q.frequency - p.frequency) <= matchWithin * p.frequency && (nearest == nullptr || std::abs (q.frequency - p.frequency) < std::abs (nearest->frequency - p.frequency)))
                    nearest = &q;

            if (nearest != nullptr && nearest->frequency > 0.0)
                ratios.emplace_back (std::log (p.frequency / nearest->frequency), juce::jmin (p.power, nearest->power));
        }

        const auto change = ratios.size() >= 3 ? weightedMedian (ratios) : 0.0;
        logSpeed[(size_t) step] = (step > 0 ? logSpeed[(size_t) step - 1] : 0.0) + change;
        previous = std::move (peaks);
    }

    // Smoothed to the drift asked for (a moving average over half a period of the fastest, both
    // ways), then centred on its mean, so the length stays.
    const auto frameRate = sampleRate / hop;
    const auto reach = juce::jmax (0, (int) std::lround (frameRate / settings.fastestHz / 4.0));
    std::vector<double> smoothed ((size_t) steps);

    for (int i = 0; i < steps; ++i)
    {
        auto sum = 0.0;
        auto n = 0;

        for (int j = juce::jmax (0, i - reach); j <= juce::jmin (steps - 1, i + reach); ++j)
        {
            sum += logSpeed[(size_t) j];
            ++n;
        }

        smoothed[(size_t) i] = sum / n;
    }

    auto mean = 0.0;

    for (auto v : smoothed)
        mean += v;

    mean /= steps;
    d.speed.resize ((size_t) steps);

    for (int i = 0; i < steps; ++i)
        d.speed[(size_t) i] = std::exp ((smoothed[(size_t) i] - mean) * settings.amountPercent / 100.0);

    return d;
}

AudioDocument::Snapshot correct (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings,
                                 const std::function<bool (float)>& progress)
{
    const auto drift = measure (s, start, end, sampleRate, settings, progress);

    if (drift.speed.empty())
        return nullptr;

    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    return AudioEdits::processRange (s, start, end, [&drift, &progress] (juce::AudioBuffer<float>& b)
    {
        // The speed at any frame: between the curve's points (each at its frame's centre), flat past
        // the ends. The audio is read at the inverse speed; the read position is scaled at the end
        // so the whole range is covered exactly.
        const auto frames = b.getNumSamples();
        const auto speedAt = [&drift] (double frame)
        {
            const auto position = (frame - drift.hop * 2.0) / drift.hop;   // the first frame's centre is at 2 hops (half a window)
            const auto i = juce::jlimit (0, (int) drift.speed.size() - 1, (int) std::floor (position));
            const auto j = juce::jmin ((int) drift.speed.size() - 1, i + 1);
            const auto t = juce::jlimit (0.0, 1.0, position - i);
            return drift.speed[(size_t) i] + t * (drift.speed[(size_t) j] - drift.speed[(size_t) i]);
        };

        std::vector<double> readAt ((size_t) frames + 1);
        readAt[0] = 0.0;

        for (int n = 0; n < frames; ++n)
            readAt[(size_t) n + 1] = readAt[(size_t) n] + 1.0 / speedAt (readAt[(size_t) n]);

        const auto scale = (double) frames / readAt[(size_t) frames];

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            const juce::AudioBuffer<float> source (b.getNumChannels(), frames);
            const_cast<juce::AudioBuffer<float>&> (source).copyFrom (ch, 0, b, ch, 0, frames);
            const auto* x = source.getReadPointer (ch);
            auto* y = b.getWritePointer (ch);
            const auto at = [x, frames] (int i) { return (double) x[juce::jlimit (0, frames - 1, i)]; };

            for (int n = 0; n < frames; ++n)
            {
                const auto p = readAt[(size_t) n] * scale;
                const auto i = (int) std::floor (p);
                const auto t = p - i;
                // Four-point (Catmull-Rom) interpolation between the samples round p.
                const auto a = at (i - 1), b0 = at (i), c = at (i + 1), d0 = at (i + 2);
                const auto v = b0 + 0.5 * t * (c - a + t * (2.0 * a - 5.0 * b0 + 4.0 * c - d0 + t * (3.0 * (b0 - c) + d0 - a)));
                y[n] = (float) v;
            }

            if (progress && ! progress (0.8f + 0.2f * (float) (ch + 1) / (float) b.getNumChannels()))
                return;
        }
    });
}
}
