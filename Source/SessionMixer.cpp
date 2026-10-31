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
}

void render (const SessionState& state, juce::int64 start, juce::AudioBuffer<float>& dest, int destStart, int numFrames,
             juce::AudioBuffer<float>& scratch)
{
    for (int ch = 0; ch < dest.getNumChannels(); ++ch)
        dest.clear (ch, destStart, numFrames);

    const auto end = start + numFrames;
    const auto solo = state.anySolo();

    for (auto& track : state.tracks)
    {
        if (track.mute || (solo && ! track.solo))
            continue;

        const auto trackGain = juce::Decibels::decibelsToGain (track.gainDb, -150.0f);

        for (auto& clip : track.clips)
        {
            const auto from = juce::jmax (start, clip.position);
            const auto to = juce::jmin (end, clip.getEnd());

            if (to <= from || clip.audio == nullptr)
                continue;

            const auto count = (int) (to - from);
            const auto channels = juce::jmin (2, clip.audio->getNumChannels());

            if (scratch.getNumChannels() < 2 || scratch.getNumSamples() < count)
                scratch.setSize (2, count, false, false, true);

            clip.audio->read (scratch, 0, clip.offset + (from - clip.position), count);
            const auto clipGain = juce::Decibels::decibelsToGain (clip.gainDb, -150.0f) * trackGain;

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* s = scratch.getWritePointer (ch);

                if (clip.fadeIn > 0 || clip.fadeOut > 0)
                {
                    for (int i = 0; i < count; ++i)
                    {
                        const auto inClip = from - clip.position + i;
                        s[i] *= fadeGain (inClip, clip.fadeIn) * fadeGain (clip.length - 1 - inClip, clip.fadeOut);
                    }
                }
            }

            const auto [left, right] = panGains (track.pan, channels);
            const auto offset = destStart + (int) (from - start);
            dest.addFrom (0, offset, scratch, 0, 0, count, clipGain * left);
            dest.addFrom (1, offset, scratch, channels - 1, 0, count, clipGain * right);
        }
    }
}
}
