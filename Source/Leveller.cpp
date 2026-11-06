#include "Leveller.h"
#include "AudioEdits.h"
#include "Loudness.h"

namespace Leveller
{
namespace
{
    constexpr double stepSeconds = 0.05;
    constexpr int windowSteps = 8;             // 400 ms blocks, as BS.1770's momentary loudness
    constexpr int reach = 15;                  // each step's level is of the blocks within 750 ms of it
    constexpr double relativeGate = 0.1;       // blocks more than 10 LU under one they overlap don't count
    constexpr int smoothSteps = 5;             // each of two averaging passes spans this many steps either side
    constexpr double edgeSeconds = 0.01;
    constexpr int outputBlockFrames = 1 << 20;

    int stepFor (double sampleRate)
    {
        return juce::jmax (1, (int) std::lround (stepSeconds * sampleRate));
    }

    /** Each value replaced by the mean of those within `span` of it (fewer at the ends). */
    std::vector<double> averaged (const std::vector<double>& v, int span)
    {
        const auto n = (int) v.size();
        std::vector<double> out ((size_t) n), sums ((size_t) n + 1, 0.0);

        for (int i = 0; i < n; ++i)
            sums[(size_t) i + 1] = sums[(size_t) i] + v[(size_t) i];

        for (int i = 0; i < n; ++i)
        {
            const auto lo = juce::jmax (0, i - span), hi = juce::jmin (n - 1, i + span);
            out[(size_t) i] = (sums[(size_t) hi + 1] - sums[(size_t) lo]) / (hi - lo + 1);
        }

        return out;
    }

    /** The gain in dB of each step from start, and so how far it may go. */
    std::optional<std::vector<float>> stepGains (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double rate,
                                                 const Settings& s, const std::function<bool (float)>& progress)
    {
        const auto step = stepFor (rate);
        const auto count = (int) ((end - start + step - 1) / step);

        // Powers from the reach and half a block before the range to the same after it, so every
        // block looked at is whole; peaks of the range's own steps.
        const auto before = reach + windowSteps / 2 - 1, after = reach + windowSteps / 2;
        const auto powers = Loudness::stepPowers (snapshot, start - (juce::int64) before * step, start + (juce::int64) (count + after) * step,
                                                  rate, step, [&] (float p) { return progress == nullptr || progress (p * 0.8f); });

        if (! powers.has_value())
            return std::nullopt;

        const auto channels = snapshot.getNumChannels();
        std::vector<double> peak ((size_t) count, 0.0);
        juce::AudioBuffer<float> buffer (channels, step);

        for (int k = 0; k < count; ++k)
        {
            const auto from = start + (juce::int64) k * step;
            const auto n = (int) juce::jmin ((juce::int64) step, end - from);
            snapshot.read (buffer, 0, from, n);

            for (int ch = 0; ch < channels; ++ch)
                peak[(size_t) k] = juce::jmax (peak[(size_t) k], (double) buffer.getMagnitude (ch, 0, n));
        }

        // The power of the 400 ms block centred on each step, from `reach` steps before the range
        // to as many after it.
        std::vector<double> block ((size_t) (count + 2 * reach));
        const auto silence = std::pow (10.0, (s.silenceLufs + 0.691) / 10.0);

        for (size_t b = 0; b < block.size(); ++b)
        {
            double sum = 0.0;

            for (int j = 0; j < windowSteps; ++j)
                sum += (*powers)[b + (size_t) j];

            block[b] = sum / windowSteps;
        }

        // A block is of speech when it is above the silence threshold and within 10 LU of the
        // loudest block it overlaps: one that only just reaches into speech, at a pause's edge,
        // is far under the block beside it that is all speech, and doesn't count. (Only blocks
        // overlapping it, so speech beyond a pause, however loud, doesn't shut out quiet speech.)
        const auto blocks = (int) block.size();
        std::vector<bool> counts ((size_t) blocks, false);

        for (int b = 0; b < blocks; ++b)
        {
            auto loudest = 0.0;

            for (int j = juce::jmax (0, b - (windowSteps - 1)); j <= juce::jmin (blocks - 1, b + windowSteps - 1); ++j)
                loudest = juce::jmax (loudest, block[(size_t) j]);

            counts[(size_t) b] = block[(size_t) b] >= silence && block[(size_t) b] >= relativeGate * loudest;
        }

        // A step is of speech when the block centred on it is. Its level: the mean of the speech
        // blocks within its reach (at a passage's edge, reaching back into the passage), brought
        // to the target within the boost and cut. The other steps are pauses.
        std::vector<double> wanted ((size_t) count, 0.0);
        std::vector<bool> speech ((size_t) count, false);
        auto any = false;

        for (int k = 0; k < count; ++k)
        {
            if (! counts[(size_t) (k + reach)])
                continue;

            double kept = 0.0;
            int m = 0;

            for (int b = k; b <= k + 2 * reach; ++b)
                if (counts[(size_t) b])
                {
                    kept += block[(size_t) b];
                    ++m;
                }

            if (m == 0)
                continue;

            const auto lufs = -0.691 + 10.0 * std::log10 (kept / m);
            speech[(size_t) k] = any = true;
            wanted[(size_t) k] = juce::jlimit ((double) -s.maxCutDb, (double) s.maxBoostDb, (double) s.targetLufs - lufs);
        }

        if (! any)
            return std::vector<float> ((size_t) count, 0.0f);

        // Pauses: the gain of the speech either side, in a line between them, or of the nearest
        // speech at the ends.
        for (int k = 0; k < count;)
        {
            if (speech[(size_t) k])
            {
                ++k;
                continue;
            }

            auto e = k;

            while (e < count && ! speech[(size_t) e])
                ++e;

            for (auto j = k; j < e; ++j)
            {
                if (k == 0)
                    wanted[(size_t) j] = wanted[(size_t) e];
                else if (e == count)
                    wanted[(size_t) j] = wanted[(size_t) k - 1];
                else
                    wanted[(size_t) j] = wanted[(size_t) k - 1] + (wanted[(size_t) e] - wanted[(size_t) k - 1]) * (j - k + 1) / (e - k + 1);
            }

            k = e;
        }

        // The ceiling: a step's gain, and its neighbours' (the gain between two steps' centres is
        // between theirs), must not take its peak above it. That limit is then taken over twice
        // the averaging span, so the two averaging passes can't lift a gain past any limit.
        std::vector<double> limit ((size_t) count);

        for (int k = 0; k < count; ++k)
        {
            auto p = peak[(size_t) k];

            if (k > 0) p = juce::jmax (p, peak[(size_t) k - 1]);
            if (k + 1 < count) p = juce::jmax (p, peak[(size_t) k + 1]);

            // A hundredth of a dB under, so rounding can't take a peak over.
            limit[(size_t) k] = p > 0.0 ? (double) Settings::ceilingDb - 0.01 - 20.0 * std::log10 (p) : HUGE_VAL;
        }

        for (int k = 0; k < count; ++k)
        {
            auto least = HUGE_VAL;

            for (int j = juce::jmax (0, k - 2 * smoothSteps); j <= juce::jmin (count - 1, k + 2 * smoothSteps); ++j)
                least = juce::jmin (least, limit[(size_t) j]);

            wanted[(size_t) k] = juce::jmin (wanted[(size_t) k], least);
        }

        const auto smooth = averaged (averaged (wanted, smoothSteps), smoothSteps);
        std::vector<float> out ((size_t) count);

        for (int k = 0; k < count; ++k)
            out[(size_t) k] = (float) juce::jmin (smooth[(size_t) k], limit[(size_t) k]);

        return out;
    }
}

bool isValid (const Settings& s)
{
    return std::isfinite (s.targetLufs) && std::isfinite (s.maxBoostDb) && std::isfinite (s.maxCutDb) && std::isfinite (s.silenceLufs)
           && s.targetLufs >= -40.0f && s.targetLufs <= -5.0f && s.maxBoostDb >= 0.0f && s.maxBoostDb <= 30.0f
           && s.maxCutDb >= 0.0f && s.maxCutDb <= 30.0f && s.silenceLufs >= -80.0f && s.silenceLufs <= -20.0f;
}

std::optional<std::vector<float>> gains (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate,
                                         Settings settings, const std::function<bool (float)>& progress)
{
    start = juce::jlimit ((juce::int64) 0, snapshot.getNumFrames(), start);
    end = juce::jlimit (start, snapshot.getNumFrames(), end);

    if (end <= start || sampleRate <= 0.0 || ! isValid (settings))
        return std::vector<float>();

    return stepGains (snapshot, start, end, sampleRate, settings, progress);
}

AudioDocument::Snapshot level (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate,
                               Settings settings, const std::function<bool (float)>& progress)
{
    start = juce::jlimit ((juce::int64) 0, snapshot.getNumFrames(), start);
    end = juce::jlimit (start, snapshot.getNumFrames(), end);
    const auto channels = snapshot.getNumChannels();

    if (end <= start || channels == 0 || sampleRate <= 0.0 || ! isValid (settings))
        return nullptr;

    const auto dbs = stepGains (snapshot, start, end, sampleRate, settings, progress);

    if (! dbs.has_value())
        return nullptr;

    // Each step's gain belongs at its centre; between centres it goes in a straight line, and
    // before the first and after the last it holds. It eases from 1 over the range's edges.
    const auto step = stepFor (sampleRate);
    const auto count = (int) dbs->size();
    std::vector<float> g ((size_t) count);

    for (int k = 0; k < count; ++k)
        g[(size_t) k] = std::pow (10.0f, (*dbs)[(size_t) k] / 20.0f);

    const auto length = end - start;
    const auto edge = (double) juce::jmax ((juce::int64) 1, juce::jmin ((juce::int64) std::lround (edgeSeconds * sampleRate), length / 2));
    std::vector<AudioSnapshot::Segment> segments;

    for (juce::int64 pos = 0; pos < length; pos += outputBlockFrames)
    {
        const auto n = (int) juce::jmin ((juce::int64) outputBlockFrames, length - pos);
        juce::AudioBuffer<float> block (channels, n);
        snapshot.read (block, 0, start + pos, n);

        for (int i = 0; i < n; ++i)
        {
            const auto at = pos + i;
            const auto x = ((double) at - step / 2.0) / step;   // in steps, 0 at the first centre
            const auto k = (int) std::floor (x);
            float gain;

            if (k < 0)
                gain = g.front();
            else if (k >= count - 1)
                gain = g.back();
            else
                gain = g[(size_t) k] + (g[(size_t) k + 1] - g[(size_t) k]) * (float) (x - k);

            const auto ease = juce::jmin (1.0, ((double) at + 0.5) / edge, ((double) (length - at) - 0.5) / edge);
            gain = 1.0f + (gain - 1.0f) * (float) juce::jlimit (0.0, 1.0, ease);

            for (int ch = 0; ch < channels; ++ch)
                block.getWritePointer (ch)[i] *= gain;
        }

        segments.push_back ({ std::make_shared<const AudioBlock> (std::move (block)), 0, n });

        if (progress != nullptr && ! progress (0.8f + 0.2f * (float) ((double) (pos + n) / (double) length)))
            return nullptr;
    }

    const AudioClip clip { channels, sampleRate, std::move (segments) };
    return AudioEdits::replaceWithClip (snapshot, start, end, clip);
}
}
