#include "Loudness.h"
#include "SampleRateConverter.h"

namespace Loudness
{
namespace
{
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;

        double process (double x)
        {
            const auto y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    /** The two K-weighting stages for any rate, from the analogue prototypes behind the
        48 kHz coefficients in BS.1770. */
    std::pair<Biquad, Biquad> kWeighting (double rate)
    {
        Biquad shelf, highPass;

        {
            const auto f0 = 1681.974450955533, gainDb = 3.999843853973347, q = 0.7071752369554196;
            const auto k = std::tan (juce::MathConstants<double>::pi * f0 / rate);
            const auto vh = std::pow (10.0, gainDb / 20.0);
            const auto vb = std::pow (vh, 0.4996667741545416);
            const auto a0 = 1.0 + k / q + k * k;
            shelf.b0 = (vh + vb * k / q + k * k) / a0;
            shelf.b1 = 2.0 * (k * k - vh) / a0;
            shelf.b2 = (vh - vb * k / q + k * k) / a0;
            shelf.a1 = 2.0 * (k * k - 1.0) / a0;
            shelf.a2 = (1.0 - k / q + k * k) / a0;
        }

        {
            const auto f0 = 38.13547087602444, q = 0.5003270373238773;
            const auto k = std::tan (juce::MathConstants<double>::pi * f0 / rate);
            const auto a0 = 1.0 + k / q + k * k;
            highPass.b0 = 1.0;
            highPass.b1 = -2.0;
            highPass.b2 = 1.0;
            highPass.a1 = 2.0 * (k * k - 1.0) / a0;
            highPass.a2 = (1.0 - k / q + k * k) / a0;
        }

        return { shelf, highPass };
    }

    double channelWeight (int channel, int numChannels)
    {
        if (numChannels == 6)   // L R C LFE Ls Rs
            return channel == 3 ? 0.0 : channel >= 4 ? 1.41 : 1.0;

        // L R C LFE Lrs Rrs Lss Rss: the sides (at 90 degrees) count 1.41 times, the rears (at 135) once,
        // as BS.1770-4 weights channels by where they are.
        if (numChannels == 8)
            return channel == 3 ? 0.0 : channel >= 6 ? 1.41 : 1.0;

        return 1.0;
    }

    double toLufs (double meanSquare)             { return meanSquare > 0.0 ? -0.691 + 10.0 * std::log10 (meanSquare) : -HUGE_VAL; }

    /** Accumulates K-weighted power in 100 ms steps; windows are sums of consecutive steps. */
    struct Meter
    {
        Meter (int channels, double rate) : numChannels (channels), stepLength ((int) std::llround (rate / 10.0))
        {
            for (int ch = 0; ch < channels; ++ch)
                filters.push_back (kWeighting (rate));
        }

        void process (const juce::AudioBuffer<float>& buffer, int count)
        {
            for (int i = 0; i < count; ++i)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                {
                    const auto w = channelWeight (ch, numChannels);

                    if (w <= 0.0)
                        continue;

                    auto& [shelf, highPass] = filters[(size_t) ch];
                    const auto y = highPass.process (shelf.process ((double) buffer.getSample (ch, i)));
                    stepSum += w * y * y;
                }

                if (++stepFill == stepLength)
                {
                    steps.push_back (stepSum / stepLength);
                    stepSum = 0.0;
                    stepFill = 0;
                }
            }
        }

        /** Mean power of every window of n steps, one per step. */
        std::vector<double> windows (int n) const
        {
            std::vector<double> result;
            double sum = 0.0;

            for (size_t i = 0; i < steps.size(); ++i)
            {
                sum += steps[i];

                if (i >= (size_t) n)
                    sum -= steps[i - (size_t) n];

                if (i + 1 >= (size_t) n)
                    result.push_back (juce::jmax (0.0, sum) / n);
            }

            return result;
        }

        int numChannels;
        int stepLength;
        std::vector<std::pair<Biquad, Biquad>> filters;
        std::vector<double> steps;
        double stepSum = 0.0;
        int stepFill = 0;
    };

    void finish (const Meter& meter, Result& r)
    {
        // Integrated: 400 ms blocks every 100 ms, absolute gate, then relative gate at -10 LU.
        const auto blocks = meter.windows (4);
        auto gatedMean = [] (const std::vector<double>& powers, double gate)
        {
            double sum = 0.0;
            int n = 0;

            for (auto p : powers)
                if (toLufs (p) > gate)
                    sum += p, ++n;

            return n > 0 ? sum / n : 0.0;
        };

        for (auto p : blocks)
            r.maxMomentary = juce::jmax (r.maxMomentary, toLufs (p));

        const auto absolute = gatedMean (blocks, -70.0);

        if (absolute > 0.0)
            r.integrated = toLufs (gatedMean (blocks, toLufs (absolute) - 10.0));

        // Loudness range: 3 s windows every 100 ms, absolute gate, relative gate at -20 LU,
        // then the spread between the 10th and 95th percentiles.
        const auto shortTerm = meter.windows (30);
        std::vector<double> levels;

        for (auto p : shortTerm)
        {
            r.maxShortTerm = juce::jmax (r.maxShortTerm, toLufs (p));

            if (toLufs (p) > -70.0)
                levels.push_back (p);
        }

        if (! levels.empty())
        {
            const auto gate = toLufs (std::accumulate (levels.begin(), levels.end(), 0.0) / (double) levels.size()) - 20.0;
            std::vector<double> kept;

            for (auto p : levels)
                if (toLufs (p) > gate)
                    kept.push_back (toLufs (p));

            std::sort (kept.begin(), kept.end());

            if (kept.size() > 1)
            {
                auto percentile = [&] (double q) { return kept[(size_t) std::llround (q * (double) (kept.size() - 1))]; };
                r.range = percentile (0.95) - percentile (0.10);
            }
        }
    }

    double peakDb (const juce::AudioBuffer<float>& b)
    {
        float peak = 0.0f;

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            peak = juce::jmax (peak, b.getMagnitude (ch, 0, b.getNumSamples()));

        return peak > 0.0f ? 20.0 * std::log10 ((double) peak) : -HUGE_VAL;
    }
}

std::optional<Result> measure (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate,
                               const std::function<bool (float)>& progress)
{
    constexpr int chunk = 1 << 16;
    const auto channels = snapshot.getNumChannels();
    const auto total = juce::jmax ((juce::int64) 1, end - start);
    Meter meter (channels, sampleRate);
    Result r;
    juce::AudioBuffer<float> buffer (channels, chunk), oversampled;

    // True peak is found chunk by chunk with some overlap, so the converter's filter has context.
    constexpr int context = 256;
    juce::AudioBuffer<float> withContext (channels, chunk + 2 * context);

    for (auto pos = start; pos < end; pos += chunk)
    {
        const auto count = (int) juce::jmin ((juce::int64) chunk, end - pos);
        snapshot.read (buffer, 0, pos, count);
        meter.process (buffer, count);

        const juce::AudioBuffer<float> read (buffer.getArrayOfWritePointers(), channels, count);
        r.samplePeak = juce::jmax (r.samplePeak, peakDb (read));

        // Context before and after comes from the file itself; outside the measured range it is
        // silence, as if the range were a file of its own.
        withContext.clear();
        const auto before = (int) juce::jmin ((juce::int64) context, pos - start);
        const auto after = (int) juce::jmin ((juce::int64) context, end - (pos + count));
        snapshot.read (withContext, context - before, pos - before, before + count + after);
        juce::AudioBuffer<float> span (withContext.getArrayOfWritePointers(), channels, 0, count + 2 * context);
        SampleRateConverter::convert (span, sampleRate, sampleRate * 4.0, oversampled);

        juce::AudioBuffer<float> middle (oversampled.getArrayOfWritePointers(), channels, context * 4, count * 4);
        r.truePeak = juce::jmax (r.truePeak, peakDb (middle));

        if (progress != nullptr && ! progress ((float) ((double) (pos + count - start) / (double) total)))
            return std::nullopt;
    }

    r.truePeak = juce::jmax (r.truePeak, r.samplePeak);
    finish (meter, r);
    return r;
}

std::optional<std::vector<double>> stepPowers (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate,
                                               int stepFrames, const std::function<bool (float)>& progress)
{
    constexpr int chunk = 1 << 16;
    const auto channels = snapshot.getNumChannels();
    std::vector<std::pair<Biquad, Biquad>> filters;
    std::vector<double> powers;

    if (end <= start || stepFrames <= 0)
        return powers;

    for (int ch = 0; ch < channels; ++ch)
        filters.push_back (kWeighting (sampleRate));

    juce::AudioBuffer<float> buffer (channels, chunk);
    double sum = 0.0;
    int fill = 0;

    for (auto pos = start; pos < end; pos += chunk)
    {
        const auto count = (int) juce::jmin ((juce::int64) chunk, end - pos);
        snapshot.read (buffer, 0, pos, count);

        for (int i = 0; i < count; ++i)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                const auto w = channelWeight (ch, channels);

                if (w <= 0.0)
                    continue;

                auto& [shelf, highPass] = filters[(size_t) ch];
                const auto y = highPass.process (shelf.process ((double) buffer.getSample (ch, i)));
                sum += w * y * y;
            }

            if (++fill == stepFrames)
            {
                powers.push_back (sum / stepFrames);
                sum = 0.0;
                fill = 0;
            }
        }

        if (progress != nullptr && ! progress ((float) ((double) (pos + count - start) / (double) (end - start))))
            return std::nullopt;
    }

    if (fill > 0)
        powers.push_back (sum / fill);

    return powers;
}

Result measure (const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    std::vector<AudioSnapshot::Segment> segments;
    const auto frames = (juce::int64) buffer.getNumSamples();

    if (frames > 0)
        segments.push_back ({ std::make_shared<const AudioBlock> (juce::AudioBuffer<float> (buffer)), 0, frames });

    const AudioSnapshot snapshot (buffer.getNumChannels(), std::move (segments));
    return *measure (snapshot, 0, frames, sampleRate);
}

juce::String format (double value, const juce::String& unit)
{
    return std::isfinite (value) ? juce::String (value, 1) + " " + unit : "-inf " + unit;
}
}
