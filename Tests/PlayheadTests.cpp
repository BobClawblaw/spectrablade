#include <JuceHeader.h>
#include "PlayheadTests.h"
#include "TestUtils.h"
#include "../Source/AudioEngine.h"
#include "../Source/AudioEdits.h"

using TestUtils::check;

namespace
{
    constexpr double rate = 48000.0;
    constexpr int blockSize = 512;

    struct Rig
    {
        Rig (juce::int64 frames)
        {
            juce::AudioBuffer<float> audio (2, (int) frames);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < (int) frames; ++i)
                    audio.setSample (ch, i, (float) i / (float) frames);

            document.reset (AudioEdits::fromBuffer (std::move (audio)), rate, {}, 24);
            inserts.prepare (rate);
            player.resetSource();
            player.prepareToPlay (rate, blockSize);
        }

        ~Rig()  { player.releaseResources(); }

        /** One block; true when it was all silence. */
        bool block()
        {
            juce::AudioBuffer<float> out (2, blockSize);
            juce::MidiBuffer midi;
            player.processBlock (out, midi);
            return out.getMagnitude (0, blockSize) <= 0.0f;
        }

        void blocks (int n)  { for (int i = 0; i < n; ++i) block(); }

        /** The position is at frame, give or take the few frames the transport's resampler reads ahead. */
        bool at (juce::int64 frame) const  { return std::abs (player.getPositionFrames() - frame) <= 8; }

        Session session;
        AudioDocument document;
        TrackEffects inserts;
        DocumentPlayerProcessor player { document, session, inserts };
    };

    void testHeardFrame()
    {
        Rig rig (30000);
        auto& player = rig.player;

        // No latency: the heard frame is the position.
        player.play (5000, 20000);
        rig.blocks (2);
        const auto plain = player.getHeardFrame() == 6024 && rig.at (6024) && player.getOutputLatency() == 0;

        // 1,000 frames of latency: the start while the effects fill, then 1,000 behind the position.
        player.setOutputLatency (1000);
        player.play (5000, 20000);
        rig.blocks (1);
        const auto filling = player.getHeardFrame() == 5000 && rig.at (5512);
        rig.blocks (1);
        const auto behind = player.getHeardFrame() == 5024 && rig.at (6024);

        // Past the stop frame the source plays silence, and the heard frame reaches the stop 1,000 frames later.
        rig.blocks (28);
        const auto nearEnd = player.getHeardFrame() == 19360 && rig.at (20360) && player.isPlaying();
        rig.blocks (2);
        const auto pastEnd = player.getHeardFrame() == 20384 && player.isPlaying();
        player.pause();

        check (plain && filling && behind && nearEnd && pastEnd,
               "heard frame: the position with no latency; with 1,000 frames of it, the start for the first 1,000 frames played, then 1,000 "
               "behind the position, reaching the stop frame 1,000 frames after the position does");

        // A loop of 1,000 frames with 700 of latency: after 2,048 frames the position has gone round
        // twice (5,048), the sound is 700 behind it, on the pass before (5,348).
        player.setOutputLatency (700);
        player.setLoop (5000, 6000);
        player.play (5000, -1);
        rig.blocks (4);
        check (rig.at (5048) && player.getHeardFrame() == 5348,
               "heard frame: round a loop, the heard frame is the latency behind the position on the pass before");
        player.pause();
        player.clearLoop();
    }

    void testStopFollowsLoop()
    {
        // A selection of 5,000 to 6,000 plays looping: the play stops by itself at 6,000, the loop
        // wraps there. Lengthened to 5,000 to 8,000 while playing, the loop alone would play silence
        // from 6,000 on (the source stops there) and the view would stop the play; the stop moved
        // with it, the sound carries on to 8,000 and wraps.
        Rig rig (30000);
        auto& player = rig.player;
        player.setOutputLatency (0);
        player.setLoop (5000, 6000);
        player.play (5000, 6000);
        rig.blocks (3);   // 1,536 frames: round once, at 5,536
        const auto looping = rig.at (5536) && player.isPlaying();

        player.setStopFrame (8000);
        player.setLoop (5000, 8000);
        bool sounding = true;

        for (int i = 0; i < 4; ++i)   // 2,048 frames on: 7,584, all past the old stop with sound
            sounding = ! rig.block() && sounding;

        const auto on = rig.at (7584) && player.getStopFrame() == 8000;
        rig.block();                  // to 8,096: round the new end, at 5,096
        const auto wrapped = rig.at (5096) && player.isPlaying();
        check (looping && sounding && on && wrapped,
               "stop frame follows the loop: a loop lengthened while playing, with its stop moved, keeps sounding past the old stop and wraps at the new end");

        // The stop left where it was: silence from the old stop on, until the wrap.
        player.setStopFrame (6000);
        rig.blocks (2);               // to 6,120: the last of the sound
        const auto silent = rig.block() && rig.block() && player.isPlaying();   // 6,120 to 7,144: silence
        check (silent, "stop frame follows the loop: with the stop left at the old end the loop plays silence past it (what a changed selection did)");
        player.pause();
        player.clearLoop();
    }

    void testTail()
    {
        // 1,000 frames of latency on a 30,000-frame file: playing to the end, the transport runs on
        // 1,000 frames of silence past it (so the effects empty) before it stops by itself.
        Rig rig (30000);
        auto& player = rig.player;
        player.setOutputLatency (1000);
        player.play (25000, -1);
        rig.blocks (9);   // to 29,608
        const auto sound = ! rig.block() && player.isPlaying();   // 29,608 to 30,120: the last of the file
        const auto quiet = rig.block() && player.isPlaying();     // 30,120 to 30,632: silence, the tail, still playing
        const auto last = rig.block() && ! player.isPlaying();    // 30,632 to 31,144: past the tail, the transport stops
        check (sound && quiet && last,
               "tail: with 1,000 frames of latency the transport plays a block of silence past the end of the file and stops in the next, "
               "once the tail is through");

        // Without latency, the transport stops in the block the file ends in.
        player.setOutputLatency (0);
        player.play (25000, -1);
        rig.blocks (9);   // to 29,608
        const auto before = player.isPlaying();
        rig.block();      // to 30,120
        check (before && ! player.isPlaying(), "tail: with no latency the transport stops in the block the file ends in, as before");
    }
}

void runPlayheadTests()
{
    testHeardFrame();
    testStopFollowsLoop();
    testTail();
}
