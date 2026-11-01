#include "Dynamics.h"

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

AudioEdits::Snapshot limit (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Limiter& l)
{
    if (! isValid (l) || sampleRate <= 0.0 || ! rangeOk (s, start, end))
        return nullptr;

    const auto input = std::pow (10.0, l.inputGainDb / 20.0);
    const auto ceiling = (double) (float) std::pow (10.0, l.ceilingDb / 20.0);
    const auto release = smoothing (l.releaseMs, sampleRate);
    const auto ahead = juce::jmax (1, (int) std::lround (l.lookaheadMs / 1000.0 * sampleRate));

    return AudioEdits::processRange (s, start, end, [input, ceiling, release, ahead, unity = juce::exactlyEqual (l.inputGainDb, 0.0)] (juce::AudioBuffer<float>& b)
    {
        const auto n = b.getNumSamples();

        // The gain each frame needs on its own.
        std::vector<double> need ((size_t) n);

        for (int i = 0; i < n; ++i)
        {
            const auto peak = loudest (b, i) * input;
            need[(size_t) i] = peak > ceiling ? ceiling / peak : 1.0;
        }

        // The smallest need over each frame's look-ahead [i, i + ahead], with a monotonic queue.
        std::vector<double> least ((size_t) n);
        std::deque<int> queue;

        for (int i = n - 1; i >= 0; --i)
        {
            while (! queue.empty() && need[(size_t) queue.back()] >= need[(size_t) i])
                queue.pop_back();

            queue.push_back (i);

            while (queue.front() > i + ahead)
                queue.pop_front();

            least[(size_t) i] = need[(size_t) queue.front()];
        }

        // Averaged over the frames [i - ahead, i]: each of their windows holds frame i, so the
        // average is never above what frame i needs. Frames before the range count as needing 1.
        double sum = 0.0, gain = 1.0;

        for (int i = 0; i < n; ++i)
        {
            sum += least[(size_t) i] - (i >= ahead + 1 ? least[(size_t) (i - ahead - 1)] : 1.0);
            const auto average = juce::jmin (1.0, (sum + (double) (ahead + 1)) / (double) (ahead + 1));
            const auto exact = juce::jmin (average, need[(size_t) i]);  // equal or above only by rounding

            // Down at once to the averaged gain, up with the release; never above the average.
            gain = exact < gain ? exact : exact + release * (gain - exact);

            if (unity && juce::exactlyEqual (gain, 1.0))
                continue;

            for (int ch = 0; ch < b.getNumChannels(); ++ch)
            {
                const auto y = (double) b.getSample (ch, i) * input * gain;
                b.setSample (ch, i, (float) juce::jlimit (-ceiling, ceiling, y));  // only float rounding can reach past it
            }
        }
    });
}
}
