#pragma once

#include "Session.h"

/** Mixes a session to stereo.

    Each clip plays its audio with its gain and fades and is panned by its track: mono clips
    with a constant-power law (-3 dB each side at the centre), stereo clips by balance, so the
    centre leaves them as they are. Clips with more channels play their first two. Then come the
    track's effects (Inserts), then its volume. When any track is soloed, only soloed tracks
    play; muted tracks never do. */
namespace SessionMixer
{
    /** Left and right gains for a pan position and a source with this many channels. */
    std::pair<float, float> panGains (float pan, int sourceChannels);

    /** Processing on each track between its pan and its volume: the track's effects. */
    struct Inserts
    {
        virtual ~Inserts() = default;

        /** True when the track has processing to run. */
        virtual bool isActive (juce::uint32 trackId) = 0;

        /** Frames by which the track's processing delays it. The mixer reads the track that far
            ahead, so every track lines up without delaying the mix. */
        virtual int getLatency (juce::uint32 trackId) = 0;

        /** Clears the processing's memory (after a jump in time). */
        virtual void reset (juce::uint32 trackId) = 0;

        /** Processes the first numFrames of a two-channel buffer in place. */
        virtual void process (juce::uint32 trackId, juce::AudioBuffer<float>&, int numFrames) = 0;
    };

    /** Working buffers, grown as needed; give them room up front on the audio thread. */
    struct Buffers
    {
        juce::AudioBuffer<float> clip, track;

        void reserve (int frames)
        {
            clip.setSize (2, frames);
            track.setSize (2, frames);
        }
    };

    /** Replaces frames [destStart, destStart + numFrames) of dest (two channels or more; only the
        first two are written, the rest cleared) with session frames [start, start + numFrames). */
    void render (const SessionState&, juce::int64 start, juce::AudioBuffer<float>& dest, int destStart, int numFrames,
                 Buffers&, Inserts* = nullptr);

    /** Before playing from start after a jump: resets each playing track's processing and runs
        it over the frames it reads ahead, so its first output is already the audio at start. */
    void prime (const SessionState&, juce::int64 start, Buffers&, Inserts&);
}
