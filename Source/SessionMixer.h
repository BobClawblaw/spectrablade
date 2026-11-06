#pragma once

#include "Session.h"

/** Mixes a session to stereo.

    Each clip plays its audio with its gain and fades and is panned by its track: mono clips
    with a constant-power law (-3 dB each side at the centre), stereo clips by balance, so the
    centre leaves them as they are. Clips with more channels play their first two. Then come the
    track's effects (Inserts), then its volume, and the track goes to the mix. Its sends take the
    signal before the volume (pre-fader) or after it, to buses, which play it through their
    effects, volume and pan (balance) into the mix. A track whose output is a bus goes there,
    after its volume, in place of the mix; so does a bus whose output is another bus. Each bus is
    mixed before the buses it plays into.

    A volume or pan envelope takes over from the setting, sample by sample: a track's pan
    envelope where its clips are panned, its volume envelope where its volume is applied (after
    its effects, so pre-fader sends don't follow it), and a bus's at its output.
    Envelopes on effect parameters are followed by the effects themselves (Inserts), given the
    session frame of the audio they process.

    Effects delay what passes through them. A track is read ahead by its effects' latency plus
    the most latency of the paths it feeds (a bus's own, and that of the buses its output goes
    through on the way to the mix), and each of its outputs is held back by what that path
    lacks, so everything lines up without delaying the mix.

    A track ducking under another comes down while that key track's output (after its volume) is
    above the threshold, following the attack and release; it acts with the volume, so the
    track's output and post-fader sends dip, not its pre-fader sends. Keys are mixed before the
    tracks they duck. The key is lined up with the ducked track when the buses the key feeds
    have at least as much latency as the ducked track's; otherwise it comes late by the
    difference. A key that doesn't play (muted, or left out by a solo) ducks nothing.

    A sidechain feeds a key track's output (after its volume, as ducking hears it) into an
    effect's sidechain input on a track or bus. Keys are mixed first, and a key is read ahead far
    enough that its output, held back, lines up with the audio going into that effect, whatever
    latency comes before it; a key that doesn't play feeds silence.

    Muted tracks and buses play nothing. When anything is soloed, only soloed tracks and buses
    play, along with the buses a soloed track or bus feeds (by a send or an output, through any
    buses between) and the tracks and buses that feed a soloed bus. A track or bus whose output
    bus doesn't play is not heard. */
namespace SessionMixer
{
    /** Left and right gains for a pan position and a source with this many channels. */
    std::pair<float, float> panGains (float pan, int sourceChannels);

    /** A key's output for a block, lined up with the audio going into the effect slot whose
        sidechain it feeds: two channels, frames from 0. */
    struct KeyAudio
    {
        juce::uint32 slotId = 0;
        const juce::AudioBuffer<float>* audio = nullptr;
    };

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

        /** The latency of the processing before this slot on the track, where the slot is there,
            not bypassed, and takes a sidechain; otherwise -1. */
        virtual int getLatencyBefore (juce::uint32 trackId, juce::uint32 slotId) = 0;

        /** Processes the first numFrames of a two-channel buffer in place: the track's audio from
            session frame `frame` on (what its effects' automation follows), with the keys for
            the slots that take a sidechain (any slot not given one hears silence there). */
        virtual void process (const SessionTrack&, juce::int64 frame, juce::AudioBuffer<float>&, int numFrames,
                              const std::vector<KeyAudio>& keys) = 0;
    };

    /** Peak levels of the tracks and buses as they go into the mix (after their volume and pan),
        by their place in the session, for meters. The audio thread raises them; the message
        thread takes them, which starts them again from 0. Tracks past maxTracks aren't metered.
        Each reader (0 the mixer, 1 the track headers) has its own, so neither takes the other's. */
    class Levels
    {
    public:
        static constexpr int maxTracks = 256, readers = 2;

        void raise (int track, float left, float right) noexcept;

        /** The left and right peaks since this reader's last take. */
        std::pair<float, float> take (int track, int reader = 0) noexcept;

    private:
        std::array<std::atomic<float>, 2 * maxTracks * readers> peaks {};
    };

    /** Working buffers and the delay lines that line paths up, kept between blocks. Grown as
        needed: give them room up front. A session change that adds a bus or a held-back path
        allocates once, when it first plays. */
    struct Buffers
    {
        juce::AudioBuffer<float> clip, track, held, discard;
        juce::AudioBuffer<float> automation;   // pan, volume: envelope values for the block
        juce::AudioBuffer<float> clipGains;    // a clip's gain envelope for the block, in dB then as gains
        std::map<juce::uint32, juce::AudioBuffer<float>> buses;

        struct Delay
        {
            juce::AudioBuffer<float> ring;
            int frames = 0, write = 0;
        };

        std::map<std::pair<juce::uint32, juce::uint32>, Delay> delays;   // (track, destination: 0 = the mix or its output bus;
                                                                          //  a key's, the track it ducks)
        Levels* levels = nullptr;          // metered when set

        // Ducking: each key track's output for the block, and each ducked track's detector.
        struct Duck
        {
            float held = 0.0f;             // the key's level, held over 20 ms
            float gainDb = 0.0f;           // the ducking now
        };

        std::map<juce::uint32, juce::AudioBuffer<float>> keys;
        std::map<juce::uint32, juce::AudioBuffer<float>> sidechains;   // by slot: each key lined up for its effect
        std::vector<KeyAudio> keyAudio;
        std::map<juce::uint32, Duck> ducks;
        juce::AudioBuffer<float> keyHeld, duckGains;
        std::vector<int> order;            // the tracks in the order they are mixed: keys first
        std::vector<char> marks;
        std::vector<std::pair<int, int>> busOrder;   // (minus depth, place): each bus before those it plays into

        void reserve (int frames)
        {
            for (auto* b : { &clip, &track, &held, &discard, &automation, &clipGains, &keyHeld, &duckGains })
                b->setSize (2, frames);

            order.reserve (64);
            keyAudio.reserve (16);
            marks.reserve (64);
            busOrder.reserve (64);
        }
    };

    /** Audio coming in live on a track (input monitoring). It joins the track's clips, panned
        by the track as a clip with that many channels would be, and goes on through its effects,
        volume, sends and buses like them. */
    struct LiveInput
    {
        juce::uint32 trackId = 0;
        int firstChannel = 0, numChannels = 0;   // its channels in Live::audio: 1 or 2
    };

    /** The live inputs of a block, one per track at most. */
    struct Live
    {
        static constexpr int maxTracks = 16;
        std::array<LiveInput, maxTracks> inputs;
        int count = 0;
        const juce::AudioBuffer<float>* audio = nullptr;   // the block's frames, from frame 0
        bool clips = true;                                 // false: the clips are silent (stopped)
    };

    /** Replaces frames [destStart, destStart + numFrames) of dest (two channels or more) with
        session frames [start, start + numFrames), plus the live inputs: the mix in channels 0 and 1,
        and what plays out of an interface pair from channel c in channels 2 + c and 3 + c, when
        dest has them (a stereo dest gets the mix alone). */
    void render (const SessionState&, juce::int64 start, juce::AudioBuffer<float>& dest, int destStart, int numFrames,
                 Buffers&, Inserts* = nullptr, const Live* = nullptr);

    /** Before playing from start after a jump: resets every effect and delay line, then mixes
        the frames before start that the longest path needs, so the first block is already in time. */
    void prime (const SessionState&, juce::int64 start, Buffers&, Inserts&);
}
