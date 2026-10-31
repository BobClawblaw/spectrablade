#pragma once

#include "Session.h"

/** Mixes a session to stereo.

    Each clip plays its audio with its gain and fades, then its track's gain and pan. Mono
    clips are panned with a constant-power law (-3 dB each side at the centre); stereo clips
    are balanced, so the centre leaves them as they are. Clips with more channels play their
    first two. When any track is soloed, only soloed tracks play; muted tracks never do. */
namespace SessionMixer
{
    /** Left and right gains for a pan position and a source with this many channels. */
    std::pair<float, float> panGains (float pan, int sourceChannels);

    /** Replaces frames [destStart, destStart + numFrames) of dest (two channels or more; only the
        first two are written, the rest cleared) with session frames [start, start + numFrames).
        scratch is grown as needed, so on the audio thread give it room up front. */
    void render (const SessionState&, juce::int64 start, juce::AudioBuffer<float>& dest, int destStart, int numFrames,
                 juce::AudioBuffer<float>& scratch);
}
