#include "AudioEdits.h"

#include "SessionMixer.h"

using Segment = AudioSnapshot::Segment;

juce::int64 AudioClip::getNumFrames() const
{
    juce::int64 total = 0;

    for (auto& s : segments)
        total += s.length;

    return total;
}

namespace AudioEdits
{
namespace
{
    juce::Range<juce::int64> clampRange (const AudioSnapshot& s, juce::int64 start, juce::int64 end)
    {
        start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
        return { start, juce::jlimit (start, s.getNumFrames(), end) };
    }

    Snapshot assemble (int numChannels, std::vector<Segment> before, std::vector<Segment> middle, std::vector<Segment> after)
    {
        std::vector<Segment> all;
        all.reserve (before.size() + middle.size() + after.size());

        for (auto* part : { &before, &middle, &after })
            for (auto& s : *part)
                if (s.length > 0)
                    all.push_back (std::move (s));

        return std::make_shared<const AudioSnapshot> (numChannels, std::move (all));
    }

    juce::AudioBuffer<float> readRange (const AudioSnapshot& s, juce::int64 start, juce::int64 end)
    {
        jassert (end - start <= std::numeric_limits<int>::max());
        juce::AudioBuffer<float> buffer (s.getNumChannels(), (int) (end - start));
        s.read (buffer, 0, start, buffer.getNumSamples());
        return buffer;
    }
}

Snapshot fromBuffer (juce::AudioBuffer<float>&& buffer)
{
    const auto channels = buffer.getNumChannels();
    const auto frames = (juce::int64) buffer.getNumSamples();
    auto block = std::make_shared<const AudioBlock> (std::move (buffer));
    std::vector<Segment> segs;

    if (frames > 0)
        segs.push_back ({ block, 0, frames });

    return std::make_shared<const AudioSnapshot> (channels, std::move (segs));
}

AudioClip copy (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate)
{
    const auto r = clampRange (s, start, end);
    return { s.getNumChannels(), sampleRate, s.slice (r.getStart(), r.getEnd()) };
}

Snapshot remove (const AudioSnapshot& s, juce::int64 start, juce::int64 end)
{
    const auto r = clampRange (s, start, end);
    return assemble (s.getNumChannels(), s.slice (0, r.getStart()), {}, s.slice (r.getEnd(), s.getNumFrames()));
}

Snapshot crop (const AudioSnapshot& s, juce::int64 start, juce::int64 end)
{
    const auto r = clampRange (s, start, end);
    return assemble (s.getNumChannels(), {}, s.slice (r.getStart(), r.getEnd()), {});
}

juce::AudioBuffer<float> matchChannels (const juce::AudioBuffer<float>& source, int numChannels)
{
    const auto srcChannels = source.getNumChannels();

    if (srcChannels == numChannels)
        return source;

    juce::AudioBuffer<float> out (numChannels, source.getNumSamples());
    out.clear();

    if (numChannels == 1)
    {
        for (int ch = 0; ch < srcChannels; ++ch)
            out.addFrom (0, 0, source, ch, 0, source.getNumSamples(), 1.0f / (float) srcChannels);
    }
    else
    {
        // Mono goes to every channel; otherwise channels map one to one and any extra stay silent.
        for (int ch = 0; ch < numChannels; ++ch)
            if (srcChannels == 1 || ch < srcChannels)
                out.copyFrom (ch, 0, source, srcChannels == 1 ? 0 : ch, 0, source.getNumSamples());
    }

    return out;
}

Snapshot convertChannels (const AudioSnapshot& s, int numChannels)
{
    constexpr juce::int64 blockSize = 65536;
    std::vector<AudioSnapshot::Segment> segments;

    for (juce::int64 start = 0; start < s.getNumFrames(); start += blockSize)
    {
        const auto block = fromBuffer (matchChannels (readRange (s, start, juce::jmin (s.getNumFrames(), start + blockSize)), numChannels));
        segments.insert (segments.end(), block->getSegments().begin(), block->getSegments().end());
    }

    return std::make_shared<const AudioSnapshot> (numChannels, std::move (segments));
}

Snapshot replaceWithClip (const AudioSnapshot& s, juce::int64 start, juce::int64 end, const AudioClip& clip)
{
    const auto r = clampRange (s, start, end);
    auto middle = clip.segments;

    if (clip.numChannels != s.getNumChannels())
    {
        // Rare case: materialise the clip once with the document's channel layout.
        const AudioSnapshot clipSnapshot (clip.numChannels, clip.segments);
        auto fitted = matchChannels (readRange (clipSnapshot, 0, clipSnapshot.getNumFrames()), s.getNumChannels());
        middle = fromBuffer (std::move (fitted))->getSegments();
    }

    return assemble (s.getNumChannels(), s.slice (0, r.getStart()), std::move (middle), s.slice (r.getEnd(), s.getNumFrames()));
}

Snapshot mixClip (const AudioSnapshot& s, juce::int64 at, const AudioClip& clip)
{
    const auto frames = clip.getNumFrames();

    if (frames > std::numeric_limits<int>::max())
        return nullptr;

    const AudioSnapshot clipSnapshot (clip.numChannels, clip.segments);
    const auto added = matchChannels (readRange (clipSnapshot, 0, frames), s.getNumChannels());
    const auto total = s.getNumFrames();
    at = juce::jlimit ((juce::int64) 0, total, at);
    const auto overlap = (int) juce::jmin (frames, total - at);

    auto next = processRange (s, at, at + overlap, [&added] (juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            b.addFrom (ch, 0, added, ch, 0, b.getNumSamples());
    });

    if (overlap == (int) frames)
        return next;

    juce::AudioBuffer<float> tail (s.getNumChannels(), (int) frames - overlap);

    for (int ch = 0; ch < tail.getNumChannels(); ++ch)
        tail.copyFrom (ch, 0, added, ch, overlap, tail.getNumSamples());

    const AudioClip rest { s.getNumChannels(), 0.0, fromBuffer (std::move (tail))->getSegments() };
    return replaceWithClip (*next, total, total, rest);
}

Snapshot insertSilence (const AudioSnapshot& s, juce::int64 at, juce::int64 frames)
{
    jassert (juce::isPositiveAndBelow (frames, (juce::int64) std::numeric_limits<int>::max()));
    juce::AudioBuffer<float> zeros (s.getNumChannels(), (int) frames);
    zeros.clear();
    const AudioClip clip { s.getNumChannels(), 0.0, fromBuffer (std::move (zeros))->getSegments() };
    return replaceWithClip (s, at, at, clip);
}

Snapshot processRange (const AudioSnapshot& s, juce::int64 start, juce::int64 end,
                       const std::function<void (juce::AudioBuffer<float>&)>& process)
{
    const auto r = clampRange (s, start, end);
    auto buffer = readRange (s, r.getStart(), r.getEnd());
    process (buffer);
    jassert (buffer.getNumChannels() == s.getNumChannels());
    auto middle = fromBuffer (std::move (buffer))->getSegments();
    return assemble (s.getNumChannels(), s.slice (0, r.getStart()), std::move (middle), s.slice (r.getEnd(), s.getNumFrames()));
}

Snapshot gain (const AudioSnapshot& s, juce::int64 start, juce::int64 end, float decibels)
{
    const auto factor = juce::Decibels::decibelsToGain (decibels, -1000.0f);
    return processRange (s, start, end, [factor] (auto& b) { b.applyGain (factor); });
}

Snapshot silence (const AudioSnapshot& s, juce::int64 start, juce::int64 end)
{
    return processRange (s, start, end, [] (auto& b) { b.clear(); });
}

Snapshot invert (const AudioSnapshot& s, juce::int64 start, juce::int64 end)
{
    return processRange (s, start, end, [] (auto& b) { b.applyGain (-1.0f); });
}

Snapshot reverse (const AudioSnapshot& s, juce::int64 start, juce::int64 end)
{
    return processRange (s, start, end, [] (auto& b) { b.reverse (0, b.getNumSamples()); });
}

Snapshot removeDcOffset (const AudioSnapshot& s, juce::int64 start, juce::int64 end)
{
    return processRange (s, start, end, [] (juce::AudioBuffer<float>& b)
    {
        const auto n = b.getNumSamples();

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            auto* samples = b.getWritePointer (ch);
            double sum = 0.0;

            for (int i = 0; i < n; ++i)
                sum += samples[i];

            if (n > 0)
                juce::FloatVectorOperations::add (samples, (float) (-sum / n), n);
        }
    });
}

Snapshot balance (const AudioSnapshot& s, juce::int64 start, juce::int64 end, float pan)
{
    return processRange (s, start, end, [pan] (juce::AudioBuffer<float>& b)
    {
        if (b.getNumChannels() != 2)
            return;

        const auto [left, right] = SessionMixer::panGains (pan, 2);
        b.applyGain (0, 0, b.getNumSamples(), left);
        b.applyGain (1, 0, b.getNumSamples(), right);
    });
}

Snapshot swapChannels (const AudioSnapshot& s, juce::int64 start, juce::int64 end)
{
    return processRange (s, start, end, [] (juce::AudioBuffer<float>& b)
    {
        if (b.getNumChannels() < 2)
            return;

        auto* left = b.getWritePointer (0);
        auto* right = b.getWritePointer (1);
        std::swap_ranges (left, left + b.getNumSamples(), right);
    });
}

Snapshot fadeIn (const AudioSnapshot& s, juce::int64 start, juce::int64 end, FadeShape shape)
{
    return processRange (s, start, end, [shape] (juce::AudioBuffer<float>& b)
    {
        const auto n = b.getNumSamples();

        for (int i = 0; i < n; ++i)
        {
            const auto g = FadeShapes::level (shape, i, n - 1);

            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                b.getWritePointer (ch)[i] *= g;
        }
    });
}

Snapshot fadeOut (const AudioSnapshot& s, juce::int64 start, juce::int64 end, FadeShape shape)
{
    return processRange (s, start, end, [shape] (juce::AudioBuffer<float>& b)
    {
        const auto n = b.getNumSamples();

        for (int i = 0; i < n; ++i)
        {
            const auto g = n > 1 ? FadeShapes::level (shape, n - 1 - i, n - 1) : 0.0f;

            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                b.getWritePointer (ch)[i] *= g;
        }
    });
}

float peakLevel (const AudioSnapshot& s, juce::int64 start, juce::int64 end)
{
    const auto r = clampRange (s, start, end);
    float peak = 0.0f;

    for (int ch = 0; ch < s.getNumChannels(); ++ch)
    {
        const auto range = s.getPeak (ch, r.getStart(), r.getEnd());
        peak = juce::jmax (peak, std::abs (range.getStart()), std::abs (range.getEnd()));
    }

    return peak;
}

std::optional<juce::Range<juce::int64>> audibleRange (const AudioSnapshot& s, float threshold)
{
    constexpr juce::int64 blockSize = 65536;
    const auto total = s.getNumFrames();

    // The first frame in [start, end) above the threshold (the last, unless fromStart), or -1.
    const auto findIn = [&s, threshold] (juce::int64 start, juce::int64 end, bool fromStart) -> juce::int64
    {
        const auto block = readRange (s, start, end);
        const auto n = block.getNumSamples();
        juce::int64 found = -1;

        for (int ch = 0; ch < block.getNumChannels(); ++ch)
        {
            const auto* samples = block.getReadPointer (ch);

            if (fromStart)
            {
                for (int i = 0; i < n && (found < 0 || start + i < found); ++i)
                    if (std::abs (samples[i]) > threshold) { found = start + i; break; }
            }
            else
            {
                for (int i = n - 1; i >= 0 && start + i > found; --i)
                    if (std::abs (samples[i]) > threshold) { found = start + i; break; }
            }
        }

        return found;
    };

    juce::int64 first = -1;

    for (juce::int64 start = 0; start < total && first < 0; start += blockSize)
        first = findIn (start, juce::jmin (total, start + blockSize), true);

    if (first < 0)
        return std::nullopt;

    juce::int64 last = -1;

    for (auto end = total; end > first && last < 0; end -= blockSize)
        last = findIn (juce::jmax (first, end - blockSize), end, false);

    return juce::Range<juce::int64> (first, last + 1);
}

Snapshot normalize (const AudioSnapshot& s, juce::int64 start, juce::int64 end, float targetDecibels)
{
    const auto peak = peakLevel (s, start, end);

    if (peak <= 0.0f)
        return nullptr;

    const auto factor = juce::Decibels::decibelsToGain (targetDecibels) / peak;
    return processRange (s, start, end, [factor] (auto& b) { b.applyGain (factor); });
}

Snapshot normalizeChannels (const AudioSnapshot& s, juce::int64 start, juce::int64 end, float targetDecibels)
{
    const auto r = clampRange (s, start, end);
    std::vector<float> factors;
    auto anySound = false;

    for (int ch = 0; ch < s.getNumChannels(); ++ch)
    {
        const auto range = s.getPeak (ch, r.getStart(), r.getEnd());
        const auto peak = juce::jmax (std::abs (range.getStart()), std::abs (range.getEnd()));
        anySound = anySound || peak > 0.0f;
        factors.push_back (peak > 0.0f ? juce::Decibels::decibelsToGain (targetDecibels) / peak : 1.0f);
    }

    if (! anySound)
        return nullptr;

    return processRange (s, r.getStart(), r.getEnd(), [&factors] (juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            b.applyGain (ch, 0, b.getNumSamples(), factors[(size_t) ch]);
    });
}

std::vector<ChannelLevels> channelLevels (const AudioSnapshot& s, juce::int64 start, juce::int64 end)
{
    constexpr juce::int64 blockSize = 65536;
    const auto r = clampRange (s, start, end);
    std::vector<ChannelLevels> levels ((size_t) s.getNumChannels());
    std::vector<double> sums (levels.size()), squares (levels.size());

    for (auto from = r.getStart(); from < r.getEnd(); from += blockSize)
    {
        const auto block = readRange (s, from, juce::jmin (r.getEnd(), from + blockSize));

        for (int ch = 0; ch < block.getNumChannels(); ++ch)
        {
            const auto* samples = block.getReadPointer (ch);
            auto& level = levels[(size_t) ch];

            for (int i = 0; i < block.getNumSamples(); ++i)
            {
                level.peak = juce::jmax (level.peak, std::abs (samples[i]));
                sums[(size_t) ch] += samples[i];
                squares[(size_t) ch] += (double) samples[i] * samples[i];
            }
        }
    }

    if (const auto n = (double) r.getLength(); n > 0.0)
        for (size_t ch = 0; ch < levels.size(); ++ch)
        {
            levels[ch].mean = sums[ch] / n;
            levels[ch].rms = std::sqrt (squares[ch] / n);
        }

    return levels;
}

std::optional<juce::Range<juce::int64>> nextClipping (const AudioSnapshot& s, juce::int64 from, float level, int minRun)
{
    constexpr juce::int64 blockSize = 65536;
    const auto total = s.getNumFrames();
    juce::int64 runStart = -1;

    for (auto start = juce::jmax ((juce::int64) 0, from); start < total; start += blockSize)
    {
        const auto block = readRange (s, start, juce::jmin (total, start + blockSize));

        for (int i = 0; i < block.getNumSamples(); ++i)
        {
            auto loud = false;

            for (int ch = 0; ch < block.getNumChannels() && ! loud; ++ch)
                loud = std::abs (block.getSample (ch, i)) >= level;

            const auto frame = start + i;

            if (loud && runStart < 0)
                runStart = frame;
            else if (! loud && runStart >= 0)
            {
                if (frame - runStart >= minRun)
                    return juce::Range<juce::int64> (runStart, frame);

                runStart = -1;
            }
        }
    }

    if (runStart >= 0 && total - runStart >= minRun)
        return juce::Range<juce::int64> (runStart, total);

    return std::nullopt;
}
}
