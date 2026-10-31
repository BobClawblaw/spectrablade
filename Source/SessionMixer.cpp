#include "SessionMixer.h"

namespace SessionMixer
{
std::pair<float, float> panGains (float pan, int sourceChannels)
{
    pan = juce::jlimit (-1.0f, 1.0f, pan);
    constexpr auto halfPi = juce::MathConstants<float>::halfPi;

    if (sourceChannels == 1)
    {
        const auto angle = (pan + 1.0f) * 0.5f * halfPi;
        return { std::cos (angle), std::sin (angle) };
    }

    return { pan > 0.0f ? std::cos (pan * halfPi) : 1.0f, pan < 0.0f ? std::cos (-pan * halfPi) : 1.0f };
}

namespace
{
    float fadeGain (juce::int64 frame, juce::int64 fade)
    {
        return frame >= fade ? 1.0f : 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) frame / (float) fade);
    }

    bool plays (const SessionState& state, const SessionTrack& track)
    {
        return ! track.mute && (! track.solo ? ! state.anySolo() : true);
    }

    void grow (juce::AudioBuffer<float>& b, int frames)
    {
        if (b.getNumChannels() < 2 || b.getNumSamples() < frames)
            b.setSize (2, frames, false, false, true);
    }

    /** The track's clips, panned, before its effects and volume, into frames [0, numFrames) of out. */
    void renderTrack (const SessionTrack& track, juce::int64 start, juce::AudioBuffer<float>& out, int numFrames,
                      juce::AudioBuffer<float>& scratch)
    {
        out.clear (0, 0, numFrames);
        out.clear (1, 0, numFrames);
        const auto end = start + numFrames;

        for (auto& clip : track.clips)
        {
            const auto from = juce::jmax (start, clip.position);
            const auto to = juce::jmin (end, clip.getEnd());

            if (to <= from || clip.audio == nullptr)
                continue;

            const auto count = (int) (to - from);
            const auto channels = juce::jmin (2, clip.audio->getNumChannels());
            grow (scratch, count);
            clip.audio->read (scratch, 0, clip.offset + (from - clip.position), count);

            if (clip.fadeIn > 0 || clip.fadeOut > 0)
            {
                for (int ch = 0; ch < channels; ++ch)
                {
                    auto* s = scratch.getWritePointer (ch);

                    for (int i = 0; i < count; ++i)
                    {
                        const auto inClip = from - clip.position + i;
                        s[i] *= fadeGain (inClip, clip.fadeIn) * fadeGain (clip.length - 1 - inClip, clip.fadeOut);
                    }
                }
            }

            const auto clipGain = juce::Decibels::decibelsToGain (clip.gainDb, -150.0f);
            const auto [left, right] = panGains (track.pan, channels);
            const auto offset = (int) (from - start);
            out.addFrom (0, offset, scratch, 0, 0, count, clipGain * left);
            out.addFrom (1, offset, scratch, channels - 1, 0, count, clipGain * right);
        }
    }
}

void render (const SessionState& state, juce::int64 start, juce::AudioBuffer<float>& dest, int destStart, int numFrames,
             Buffers& buffers, Inserts* inserts)
{
    for (int ch = 0; ch < dest.getNumChannels(); ++ch)
        dest.clear (ch, destStart, numFrames);

    grow (buffers.track, numFrames);

    for (auto& track : state.tracks)
    {
        if (! plays (state, track))
            continue;

        const auto active = inserts != nullptr && inserts->isActive (track.id);
        const auto ahead = active ? inserts->getLatency (track.id) : 0;
        renderTrack (track, start + ahead, buffers.track, numFrames, buffers.clip);

        if (active)
            inserts->process (track.id, buffers.track, numFrames);

        const auto volume = juce::Decibels::decibelsToGain (track.gainDb, -150.0f);

        for (int ch = 0; ch < 2; ++ch)
            dest.addFrom (ch, destStart, buffers.track, ch, 0, numFrames, volume);
    }
}

void prime (const SessionState& state, juce::int64 start, Buffers& buffers, Inserts& inserts)
{
    for (auto& track : state.tracks)
    {
        if (! plays (state, track) || ! inserts.isActive (track.id))
            continue;

        inserts.reset (track.id);
        const auto ahead = inserts.getLatency (track.id);
        const auto chunk = juce::jmax (1, buffers.track.getNumSamples());

        for (int done = 0; done < ahead;)
        {
            const auto n = juce::jmin (chunk, ahead - done);
            grow (buffers.track, n);
            renderTrack (track, start + done, buffers.track, n, buffers.clip);
            inserts.process (track.id, buffers.track, n);
            done += n;
        }
    }
}
}
