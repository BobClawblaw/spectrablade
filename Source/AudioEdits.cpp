#include "AudioEdits.h"

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

    /** Smooth S-curve gain for t in [0, 1]. */
    float fadeCurve (float t)
    {
        return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * juce::jlimit (0.0f, 1.0f, t));
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

Snapshot fadeIn (const AudioSnapshot& s, juce::int64 start, juce::int64 end)
{
    return processRange (s, start, end, [] (juce::AudioBuffer<float>& b)
    {
        const auto n = b.getNumSamples();

        for (int i = 0; i < n; ++i)
        {
            const auto g = fadeCurve (n > 1 ? (float) i / (float) (n - 1) : 1.0f);

            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                b.getWritePointer (ch)[i] *= g;
        }
    });
}

Snapshot fadeOut (const AudioSnapshot& s, juce::int64 start, juce::int64 end)
{
    return processRange (s, start, end, [] (juce::AudioBuffer<float>& b)
    {
        const auto n = b.getNumSamples();

        for (int i = 0; i < n; ++i)
        {
            const auto g = fadeCurve (n > 1 ? 1.0f - (float) i / (float) (n - 1) : 0.0f);

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
}
