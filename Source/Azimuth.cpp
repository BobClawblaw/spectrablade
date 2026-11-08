#include "Azimuth.h"
#include "AudioEdits.h"
#include "PhaseCorrection.h"

namespace Azimuth
{
namespace
{
    constexpr double windowSeconds = 2.0, hopSeconds = 1.0;
    constexpr int taps = 16;

    /** A sample of x at a fractional position, by a Hann-windowed sinc over `taps` samples; outside
        the buffer the nearest sample. */
    double sampleAt (const float* x, int n, double position)
    {
        const auto centre = (int) std::floor (position);
        const auto fraction = position - centre;
        auto sum = 0.0, weights = 0.0;

        for (int k = -taps / 2 + 1; k <= taps / 2; ++k)
        {
            const auto d = (double) k - fraction;   // the tap's distance from the position
            const auto sinc = std::abs (d) < 1.0e-9 ? 1.0 : std::sin (juce::MathConstants<double>::pi * d) / (juce::MathConstants<double>::pi * d);
            const auto hann = 0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * d / (taps / 2));
            const auto w = sinc * hann;
            sum += w * x[juce::jlimit (0, n - 1, centre + k)];
            weights += w;
        }

        return weights > 1.0e-9 ? sum / weights : 0.0;
    }
}

bool isValid (const Settings& s)
{
    return s.mostMs >= 0.5 && s.mostMs <= 50.0 && s.amountPercent >= 0.0 && s.amountPercent <= 100.0;
}

Drift measure (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings,
               const std::function<bool (float)>& progress)
{
    Drift d;
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (! isValid (settings) || s.getNumChannels() != 2 || sampleRate <= 0.0 || end - start < (juce::int64) (windowSeconds * sampleRate))
        return d;

    const auto window = (juce::int64) (windowSeconds * sampleRate);
    d.hopFrames = (int) (hopSeconds * sampleRate);
    d.firstCentre = start + window / 2;
    std::vector<bool> clear;

    for (auto from = start; from + window <= end; from += d.hopFrames)
    {
        if (progress && ! progress ((float) (from - start) / (float) (end - start)))
            return {};

        const auto found = PhaseCorrection::measure (s, from, from + window, sampleRate, settings.mostMs);
        const auto ok = found.has_value() && std::abs (found->correlation) >= PhaseCorrection::weakest;
        d.offsetFrames.push_back (ok ? found->offset : 0.0);
        clear.push_back (ok);
        d.anyClear = d.anyClear || ok;
    }

    if (! d.anyClear)
        return d;

    // Unclear readings filled from the nearest clear ones (between them, on the line), then
    // smoothed over three.
    const auto n = (int) d.offsetFrames.size();

    for (int i = 0; i < n; ++i)
    {
        if (clear[(size_t) i])
            continue;

        int before = i - 1, after = i + 1;

        while (before >= 0 && ! clear[(size_t) before]) --before;
        while (after < n && ! clear[(size_t) after]) ++after;

        if (before >= 0 && after < n)
            d.offsetFrames[(size_t) i] = d.offsetFrames[(size_t) before] + (d.offsetFrames[(size_t) after] - d.offsetFrames[(size_t) before]) * (i - before) / (double) (after - before);
        else
            d.offsetFrames[(size_t) i] = before >= 0 ? d.offsetFrames[(size_t) before] : d.offsetFrames[(size_t) after];
    }

    // Smoothed over three between the ends (an end reading averaged with its one neighbour would
    // lean inward, and a steady drift be undershot there).
    auto smoothed = d.offsetFrames;

    for (int i = 1; i < n - 1; ++i)
        smoothed[(size_t) i] = (d.offsetFrames[(size_t) i - 1] + d.offsetFrames[(size_t) i] + d.offsetFrames[(size_t) i + 1]) / 3.0;

    d.offsetFrames = smoothed;
    return d;
}

AudioDocument::Snapshot correct (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings,
                                 const std::function<bool (float)>& progress)
{
    const auto drift = measure (s, start, end, sampleRate, settings, progress);

    if (drift.offsetFrames.empty() || ! drift.anyClear)
        return nullptr;

    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (end - start > std::numeric_limits<int>::max() / 4)
        return nullptr;

    const auto amount = settings.amountPercent / 100.0;
    // The offset at a frame: between the readings on a straight line, and past the first or the
    // last reading's centre along the line through the two readings nearest that end.
    const auto offsetAt = [&drift, start] (int frame)
    {
        const auto& o = drift.offsetFrames;
        const auto n = (int) o.size();
        const auto position = ((double) (start + frame) - (double) drift.firstCentre) / drift.hopFrames;

        if (n == 1)
            return o[0];

        const auto i = juce::jlimit (0, n - 2, (int) std::floor (position));
        const auto t = position - i;   // past the ends this runs under 0 or over 1 along the same line
        return o[(size_t) i] + t * (o[(size_t) i + 1] - o[(size_t) i]);
    };

    return AudioEdits::processRange (s, start, end, [&offsetAt, amount] (juce::AudioBuffer<float>& b)
    {
        // Each channel read from a copy at the moving position: the right (late) channel earlier
        // by half the offset, the left later by half.
        const juce::AudioBuffer<float> source (b);
        const auto n = b.getNumSamples();

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto* x = source.getReadPointer (ch);
            auto* y = b.getWritePointer (ch);
            const auto sign = ch == 1 ? 1.0 : -1.0;

            for (int i = 0; i < n; ++i)
                y[i] = (float) sampleAt (x, n, i + sign * amount * offsetAt (i) / 2.0);
        }
    });
}
}
