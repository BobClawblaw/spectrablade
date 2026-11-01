#pragma once

#include "Session.h"

/** Mixes a session to stereo.

    Each clip plays its audio with its gain and fades and is panned by its track: mono clips
    with a constant-power law (-3 dB each side at the centre), stereo clips by balance, so the
    centre leaves them as they are. Clips with more channels play their first two. Then come the
    track's effects (Inserts), then its volume, and the track goes to the mix. Its sends take the
    signal before the volume (pre-fader) or after it, to buses, which play it through their
    effects, volume and pan (balance) into the mix.

    A volume or pan envelope takes over from the setting, sample by sample: a track's pan
    envelope where its clips are panned, its volume envelope where its volume is applied (after
    its effects, so pre-fader sends don't follow it), and a bus's at its output.
    Envelopes on effect parameters are followed by the effects themselves (Inserts), given the
    session frame of the audio they process.

    Effects delay what passes through them. A track is read ahead by its effects' latency plus
    the most latency of the buses it sends to, and each of its outputs is held back by what that
    path lacks, so everything lines up without delaying the mix.

    Muted tracks and buses play nothing. When anything is soloed, only soloed tracks and buses
    play, along with the buses a soloed track sends to and the tracks that feed a soloed bus. */
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

        /** Processes the first numFrames of a two-channel buffer in place: the track's audio from
            session frame `frame` on (what its effects' automation follows). */
        virtual void process (const SessionTrack&, juce::int64 frame, juce::AudioBuffer<float>&, int numFrames) = 0;
    };

    /** Working buffers and the delay lines that line paths up, kept between blocks. Grown as
        needed: give them room up front. A session change that adds a bus or a held-back path
        allocates once, when it first plays. */
    struct Buffers
    {
        juce::AudioBuffer<float> clip, track, held, discard;
        juce::AudioBuffer<float> automation;   // pan, volume: envelope values for the block
        std::map<juce::uint32, juce::AudioBuffer<float>> buses;

        struct Delay
        {
            juce::AudioBuffer<float> ring;
            int frames = 0, write = 0;
        };

        std::map<std::pair<juce::uint32, juce::uint32>, Delay> delays;   // (track, destination: 0 = the mix)

        void reserve (int frames)
        {
            for (auto* b : { &clip, &track, &held, &discard, &automation })
                b->setSize (2, frames);
        }
    };

    /** Audio coming in live on one track (input monitoring). It joins the track's clips, panned
        by the track as a clip with that many channels would be, and goes on through its effects,
        volume, sends and buses like them. */
    struct LiveInput
    {
        juce::uint32 trackId = 0;
        const juce::AudioBuffer<float>* audio = nullptr;   // the block's frames, from frame 0
        int numChannels = 0;                               // 1 or 2
        bool clips = true;                                 // false: the clips are silent (stopped)
    };

    /** Replaces frames [destStart, destStart + numFrames) of dest (two channels or more; only the
        first two are written, the rest cleared) with session frames [start, start + numFrames),
        plus the live input when there is one. */
    void render (const SessionState&, juce::int64 start, juce::AudioBuffer<float>& dest, int destStart, int numFrames,
                 Buffers&, Inserts* = nullptr, const LiveInput* = nullptr);

    /** Before playing from start after a jump: resets every effect and delay line, then mixes
        the frames before start that the longest path needs, so the first block is already in time. */
    void prime (const SessionState&, juce::int64 start, Buffers&, Inserts&);
}
