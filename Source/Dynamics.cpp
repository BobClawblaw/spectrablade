#include "Dynamics.h"
#include "SampleRateConverter.h"
#include "Loudness.h"

#include <deque>

namespace Dynamics
{
namespace
{
    /** The coefficient of a one-pole smoother that moves 63 % of the way in `ms`. */
    double smoothing (double ms, double sampleRate)
    {
        return std::exp (-1.0 / (ms / 1000.0 * sampleRate));
    }

    bool rangeOk (const AudioSnapshot& s, juce::int64& start, juce::int64& end)
    {
        start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
        end = juce::jlimit (start, s.getNumFrames(), end);
        return start < end && end - start <= std::numeric_limits<int>::max();
    }

    double loudest (const juce::AudioBuffer<float>& b, int i)
    {
        double peak = 0.0;

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            peak = juce::jmax (peak, (double) std::abs (b.getSample (ch, i)));

        return peak;
    }
}

bool isValid (const Compressor& c)
{
    return c.thresholdDb >= -80.0 && c.thresholdDb <= 0.0 && c.ratio >= 1.0 && c.ratio <= 100.0 && c.kneeDb >= 0.0 && c.kneeDb <= 24.0
        && c.attackMs >= 0.01 && c.attackMs <= 1000.0 && c.releaseMs >= 1.0 && c.releaseMs <= 5000.0 && std::abs (c.makeupDb) <= 48.0;
}

double staticGainDb (const Compressor& c, double levelDb)
{
    const auto over = levelDb - c.thresholdDb;
    const auto slope = 1.0 / c.ratio - 1.0;

    if (2.0 * over <= -c.kneeDb)
        return 0.0;

    if (2.0 * over >= c.kneeDb)
        return slope * over;

    // Inside the knee, a parabola joining the two lines with matching slopes.
    const auto into = over + c.kneeDb / 2.0;
    return slope * into * into / (2.0 * c.kneeDb);
}

AudioEdits::Snapshot compress (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Compressor& c)
{
    if (! isValid (c) || sampleRate <= 0.0 || ! rangeOk (s, start, end))
        return nullptr;

    const auto attack = smoothing (c.attackMs, sampleRate);
    const auto release = smoothing (c.releaseMs, sampleRate);

    return AudioEdits::processRange (s, start, end, [&c, attack, release] (juce::AudioBuffer<float>& b)
    {
        double gainDb = 0.0;

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const auto level = loudest (b, i);
            const auto target = level > 0.0 ? staticGainDb (c, 20.0 * std::log10 (level)) : 0.0;
            const auto coefficient = target < gainDb ? attack : release;
            gainDb = target + coefficient * (gainDb - target);
            const auto factor = std::pow (10.0, (gainDb + c.makeupDb) / 20.0);

            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                b.setSample (ch, i, (float) (b.getSample (ch, i) * factor));
        }
    });
}

bool isValid (const Limiter& l)
{
    return std::abs (l.inputGainDb) <= 48.0 && l.ceilingDb >= -48.0 && l.ceilingDb <= 0.0 && l.releaseMs >= 1.0 && l.releaseMs <= 5000.0
        && l.lookaheadMs >= 0.1 && l.lookaheadMs <= 50.0;
}

namespace
{
    constexpr int oversampling = 4, chunk = 1 << 16, context = 256;

    /** Calls use (frame, peak) for every frame of b, where peak is the largest magnitude of any
        channel within half a frame of it after 4x oversampling (or of the frame itself), with silence
        outside b. Works through b in chunks, each with context either side for the filter. */
    void forEachTruePeak (const juce::AudioBuffer<float>& b, double sampleRate, const std::function<void (int, float)>& use)
    {
        const auto n = b.getNumSamples(), channels = b.getNumChannels();
        juce::AudioBuffer<float> span (channels, chunk + 2 * context), up;

        for (int pos = 0; pos < n; pos += chunk)
        {
            const auto count = juce::jmin (chunk, n - pos);
            const auto before = juce::jmin (context, pos), after = juce::jmin (context, n - pos - count);
            span.clear();

            for (int ch = 0; ch < channels; ++ch)
                span.copyFrom (ch, context - before, b, ch, pos - before, before + count + after);

            SampleRateConverter::convert (span, sampleRate, sampleRate * oversampling, up);

            for (int i = 0; i < count; ++i)
            {
                // The oversampled points from half a frame before this frame to half a frame after.
                const auto centre = (context + i) * oversampling;
                float peak = 0.0f;

                for (int ch = 0; ch < channels; ++ch)
                {
                    peak = juce::jmax (peak, std::abs (b.getSample (ch, pos + i)));

                    for (int k = centre - oversampling / 2; k <= centre + oversampling / 2; ++k)
                        if (juce::isPositiveAndBelow (k, up.getNumSamples()))
                            peak = juce::jmax (peak, std::abs (up.getSample (ch, k)));
                }

                use (pos + i, peak);
            }
        }
    }

    void limitBuffer (juce::AudioBuffer<float>& b, double sampleRate, const Limiter& l, double ceilingDb)
    {
        const auto n = b.getNumSamples();
        const auto input = std::pow (10.0, l.inputGainDb / 20.0);
        const auto ceiling = (double) (float) std::pow (10.0, ceilingDb / 20.0);
        const auto release = smoothing (l.releaseMs, sampleRate);
        const auto ahead = juce::jmax (1, (int) std::lround (l.lookaheadMs / 1000.0 * sampleRate));
        const auto unity = juce::exactlyEqual (l.inputGainDb, 0.0);

        // The gain each frame needs on its own; one float a frame, reused for the look-ahead minimum.
        std::vector<float> need ((size_t) n);
        const auto needFor = [input, ceiling] (double peak) { return peak * input > ceiling ? (float) (ceiling / (peak * input)) : 1.0f; };

        if (l.truePeak)
            forEachTruePeak (b, sampleRate, [&need, &needFor] (int i, float peak) { need[(size_t) i] = needFor (peak); });
        else
            for (int i = 0; i < n; ++i)
                need[(size_t) i] = needFor (loudest (b, i));

        // The smallest need over each frame's look-ahead [i, i + ahead], with a monotonic queue that
        // keeps its own copies of the values, so the minimum can be written over the need in place.
        std::deque<std::pair<int, float>> queue;

        for (int i = n - 1; i >= 0; --i)
        {
            const auto value = need[(size_t) i];

            while (! queue.empty() && queue.back().second >= value)
                queue.pop_back();

            queue.emplace_back (i, value);

            while (queue.front().first > i + ahead)
                queue.pop_front();

            need[(size_t) i] = queue.front().second;
        }

        // Averaged over the frames [i - ahead, i]: each of their windows holds frame i, so the
        // average is never above what frame i needs (only rounding could put it above, and the
        // clamp below catches that). Frames before the range count as needing 1.
        double sum = 0.0, gain = 1.0;

        for (int i = 0; i < n; ++i)
        {
            sum += (double) need[(size_t) i] - (i >= ahead + 1 ? (double) need[(size_t) (i - ahead - 1)] : 1.0);
            const auto average = juce::jmin (1.0, (sum + (double) (ahead + 1)) / (double) (ahead + 1));

            // Down at once to the averaged gain, up with the release; never above the average.
            gain = average < gain ? average : average + release * (gain - average);

            if (unity && juce::exactlyEqual (gain, 1.0))
                continue;

            for (int ch = 0; ch < b.getNumChannels(); ++ch)
            {
                const auto y = (double) b.getSample (ch, i) * input * gain;
                b.setSample (ch, i, (float) juce::jlimit (-ceiling, ceiling, y));  // only float rounding can reach past it
            }
        }
    }

    double truePeakOf (const juce::AudioBuffer<float>& b, double sampleRate)
    {
        float peak = 0.0f;
        forEachTruePeak (b, sampleRate, [&peak] (int, float p) { peak = juce::jmax (peak, p); });
        return peak > 0.0f ? 20.0 * std::log10 ((double) peak) : -HUGE_VAL;
    }
}

AudioEdits::Snapshot limit (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Limiter& l)
{
    if (! isValid (l) || sampleRate <= 0.0 || ! rangeOk (s, start, end))
        return nullptr;

    return AudioEdits::processRange (s, start, end, [sampleRate, &l] (juce::AudioBuffer<float>& b)
    {
        if (! l.truePeak)
        {
            limitBuffer (b, sampleRate, l, l.ceilingDb);
            return;
        }

        const juce::AudioBuffer<float> original (b);
        auto ceiling = l.ceilingDb;

        for (int pass = 0; pass < 4; ++pass)
        {
            if (pass > 0)
                b.makeCopyOf (original, true);

            limitBuffer (b, sampleRate, l, ceiling);
            const auto over = truePeakOf (b, sampleRate) - l.ceilingDb;

            if (over <= 0.001)
                break;

            ceiling -= over;
        }
    });
}

double truePeakDb (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate)
{
    if (sampleRate <= 0.0 || ! rangeOk (s, start, end))
        return -HUGE_VAL;

    juce::AudioBuffer<float> b (s.getNumChannels(), (int) (end - start));
    s.read (b, 0, start, b.getNumSamples());
    return truePeakOf (b, sampleRate);
}

Matched matchLoudness (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate,
                       double integrated, double targetLufs, double ceilingDbtp)
{
    Matched m;

    if (! std::isfinite (integrated) || ! std::isfinite (targetLufs) || ! std::isfinite (ceilingDbtp) || ! rangeOk (s, start, end))
        return m;

    m.gainDb = juce::jlimit (-48.0, 48.0, targetLufs - integrated);

    for (int pass = 0; pass < 4; ++pass)
    {
        auto next = limit (s, start, end, sampleRate, Limiter { m.gainDb, juce::jlimit (-48.0, 0.0, ceilingDbtp), 50.0, 5.0, true });

        if (next == nullptr)
            return {};

        const auto r = Loudness::measure (*next, start, end, sampleRate);

        if (! r.has_value())
            return {};

        m.snapshot = std::move (next);
        m.integrated = r->integrated;
        m.truePeak = r->truePeak;
        const auto shortfall = targetLufs - r->integrated;

        if (std::abs (shortfall) <= 0.05 || ! std::isfinite (shortfall) || m.gainDb >= 48.0)
            break;

        m.gainDb = juce::jmin (48.0, m.gainDb + shortfall);
    }

    return m;
}
}
