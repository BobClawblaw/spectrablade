#include "AudioDocument.h"

namespace
{
    juce::Range<float> scan (const float* samples, juce::int64 count)
    {
        if (count <= 0)
            return {};

        auto r = juce::FloatVectorOperations::findMinAndMax (samples, (int) count);
        return r;
    }

    juce::Range<float> merge (juce::Range<float> a, juce::Range<float> b, bool& haveA)
    {
        if (! haveA)
        {
            haveA = true;
            return b;
        }

        return { juce::jmin (a.getStart(), b.getStart()), juce::jmax (a.getEnd(), b.getEnd()) };
    }
}

//==============================================================================
AudioBlock::AudioBlock (juce::AudioBuffer<float>&& samples)
    : data (std::move (samples))
{
    const auto numChunks = (data.getNumSamples() + framesPerPeak - 1) / framesPerPeak;
    peaks.resize ((size_t) data.getNumChannels());

    for (int ch = 0; ch < data.getNumChannels(); ++ch)
    {
        auto& chunkPeaks = peaks[(size_t) ch];
        chunkPeaks.reserve ((size_t) numChunks);

        for (int i = 0; i < numChunks; ++i)
        {
            const auto start = i * framesPerPeak;
            chunkPeaks.push_back (scan (data.getReadPointer (ch, start), juce::jmin (framesPerPeak, data.getNumSamples() - start)));
        }
    }
}

juce::Range<float> AudioBlock::getPeak (int channel, juce::int64 start, juce::int64 end) const
{
    start = juce::jlimit ((juce::int64) 0, getNumFrames(), start);
    end   = juce::jlimit (start, getNumFrames(), end);

    const auto* samples = data.getReadPointer (channel);
    const auto& chunkPeaks = peaks[(size_t) channel];
    bool have = false;
    juce::Range<float> result;

    // Raw samples up to the first chunk boundary, whole chunks from the table, raw samples after.
    const auto firstWhole = (start + framesPerPeak - 1) / framesPerPeak;
    const auto lastWhole  = end / framesPerPeak;   // exclusive

    if (firstWhole >= lastWhole)
        return scan (samples + start, end - start);

    if (start < firstWhole * framesPerPeak)
        result = merge (result, scan (samples + start, firstWhole * framesPerPeak - start), have);

    for (auto i = firstWhole; i < lastWhole; ++i)
        result = merge (result, chunkPeaks[(size_t) i], have);

    if (end > lastWhole * framesPerPeak)
        result = merge (result, scan (samples + lastWhole * framesPerPeak, end - lastWhole * framesPerPeak), have);

    return result;
}

//==============================================================================
AudioSnapshot::AudioSnapshot (int channels, std::vector<Segment> segs)
    : numChannels (channels), segments (std::move (segs))
{
    starts.reserve (segments.size());

    for (auto& s : segments)
    {
        jassert (s.block != nullptr && s.block->getNumChannels() == numChannels);
        jassert (s.offset >= 0 && s.offset + s.length <= s.block->getNumFrames());
        starts.push_back (totalFrames);
        totalFrames += s.length;
    }
}

int AudioSnapshot::findSegment (juce::int64 frame) const
{
    const auto it = std::upper_bound (starts.begin(), starts.end(), frame);
    return (int) std::distance (starts.begin(), it) - 1;
}

void AudioSnapshot::read (juce::AudioBuffer<float>& dest, int destStart, juce::int64 start, int numFrames) const
{
    const auto channelsToCopy = juce::jmin (numChannels, dest.getNumChannels());

    for (int ch = channelsToCopy; ch < dest.getNumChannels(); ++ch)
        dest.clear (ch, destStart, numFrames);

    auto frame = start;
    auto out = destStart;
    auto remaining = numFrames;

    if (frame < 0)
    {
        const auto silent = (int) juce::jmin ((juce::int64) remaining, -frame);
        for (int ch = 0; ch < channelsToCopy; ++ch)
            dest.clear (ch, out, silent);

        frame += silent;
        out += silent;
        remaining -= silent;
    }

    for (auto index = findSegment (frame); remaining > 0 && index >= 0 && index < (int) segments.size(); ++index)
    {
        const auto& seg = segments[(size_t) index];
        const auto within = frame - starts[(size_t) index];
        const auto count = (int) juce::jmin ((juce::int64) remaining, seg.length - within);

        for (int ch = 0; ch < channelsToCopy; ++ch)
            dest.copyFrom (ch, out, seg.block->getChannel (ch) + seg.offset + within, count);

        frame += count;
        out += count;
        remaining -= count;
    }

    if (remaining > 0)
        for (int ch = 0; ch < channelsToCopy; ++ch)
            dest.clear (ch, out, remaining);
}

juce::Range<float> AudioSnapshot::getPeak (int channel, juce::int64 start, juce::int64 end) const
{
    start = juce::jmax ((juce::int64) 0, start);
    end = juce::jmin (end, totalFrames);
    bool have = false;
    juce::Range<float> result;

    for (auto index = findSegment (start); index >= 0 && index < (int) segments.size() && start < end; ++index)
    {
        const auto& seg = segments[(size_t) index];
        const auto within = start - starts[(size_t) index];
        const auto count = juce::jmin (end - start, seg.length - within);

        result = merge (result, seg.block->getPeak (channel, seg.offset + within, seg.offset + within + count), have);
        start += count;
    }

    return result;
}

std::vector<AudioSnapshot::Segment> AudioSnapshot::slice (juce::int64 start, juce::int64 end) const
{
    start = juce::jlimit ((juce::int64) 0, totalFrames, start);
    end   = juce::jlimit (start, totalFrames, end);
    std::vector<Segment> result;

    for (auto index = findSegment (start); index >= 0 && index < (int) segments.size() && start < end; ++index)
    {
        const auto& seg = segments[(size_t) index];
        const auto within = start - starts[(size_t) index];
        const auto count = juce::jmin (end - start, seg.length - within);

        result.push_back ({ seg.block, seg.offset + within, count });
        start += count;
    }

    return result;
}

//==============================================================================
AudioDocument::AudioDocument()
{
    current = std::make_shared<const AudioSnapshot> (2, std::vector<AudioSnapshot::Segment>());
    history.push_back ({ current, {} });
}

AudioDocument::Snapshot AudioDocument::getSnapshot() const
{
    const juce::SpinLock::ScopedLockType sl (lock);
    return current;
}

void AudioDocument::setCurrent (Snapshot next)
{
    Snapshot previous;

    {
        const juce::SpinLock::ScopedLockType sl (lock);
        previous = std::exchange (current, std::move (next));
    }

    ++version;

    retire (std::move (previous));
    sendChangeMessage();
}

void AudioDocument::retire (Snapshot s)
{
    // Never let the last reference to a snapshot drop while the audio thread may hold it.
    // Each snapshot is held here at most once, so use_count() == 1 means nobody else has it.
    if (s != nullptr && std::find (pendingRelease.begin(), pendingRelease.end(), s) == pendingRelease.end())
        pendingRelease.push_back (std::move (s));
}

void AudioDocument::reset (Snapshot initial, double rate, const juce::File& f, int bitDepth)
{
    for (auto& e : history)
        retire (std::move (e.snapshot));

    history.clear();
    history.push_back ({ initial, {} });
    position = savedPosition = 0;
    sampleRate = rate;
    file = f;
    sourceBitDepth = bitDepth;
    ++resetCount;
    setCurrent (std::move (initial));
}

void AudioDocument::apply (Snapshot next, const juce::String& description)
{
    // A new edit after undo discards the redo branch.
    for (auto i = (size_t) position + 1; i < history.size(); ++i)
        retire (std::move (history[i].snapshot));

    history.resize ((size_t) position + 1);

    if (savedPosition > position)
        savedPosition = -1;   // the saved state is no longer reachable

    history.push_back ({ next, description });
    ++position;
    setCurrent (std::move (next));
}

juce::String AudioDocument::getUndoDescription() const
{
    return canUndo() ? history[(size_t) position].description : juce::String();
}

juce::String AudioDocument::getRedoDescription() const
{
    return canRedo() ? history[(size_t) position + 1].description : juce::String();
}

void AudioDocument::undo()
{
    if (canUndo())
        setCurrent (history[(size_t) --position].snapshot);
}

void AudioDocument::redo()
{
    if (canRedo())
        setCurrent (history[(size_t) ++position].snapshot);
}

void AudioDocument::markSaved (const juce::File& savedAs, int bitDepth)
{
    savedPosition = position;
    file = savedAs;
    sourceBitDepth = bitDepth;
    sendChangeMessage();
}

void AudioDocument::releaseUnusedSnapshots()
{
    pendingRelease.erase (std::remove_if (pendingRelease.begin(), pendingRelease.end(),
                                          [] (const Snapshot& s) { return s == nullptr || s.use_count() == 1; }),
                          pendingRelease.end());
}
