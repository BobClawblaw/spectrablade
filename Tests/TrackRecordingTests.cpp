// A looped-back take must match what was played bit for bit.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "TestUtils.h"
#include "TrackRecordingTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;
    constexpr int blockSize = 256;

    /** Channel c, frame f: a value unique to both, so any misplaced frame shows. */
    AudioDocument::Snapshot ramp (int channels, int frames, float base)
    {
        juce::AudioBuffer<float> b (channels, frames);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, base + (float) ch * 0.25f + (float) i * 1.0e-6f);

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, frames } };
        return std::make_shared<const AudioSnapshot> (channels, std::move (segments));
    }

    float sampleOf (const SessionClip& c, int channel, juce::int64 sessionFrame)
    {
        juce::AudioBuffer<float> one (c.audio->getNumChannels(), 1);
        c.audio->read (one, 0, c.offset + (sessionFrame - c.position), 1);
        return one.getSample (channel, 0);
    }

    SessionClip makeClip (SessionState& s, juce::int64 position, juce::int64 length, float base)
    {
        SessionClip c;
        c.id = s.nextId++;
        c.audio = ramp (1, (int) length, base);
        c.position = position;
        c.length = length;
        c.fadeIn = 100;
        c.fadeOut = 200;
        return c;
    }

    const SessionClip* clipAt (const SessionTrack& t, juce::int64 frame)
    {
        for (auto& c : t.clips)
            if (frame >= c.position && frame < c.getEnd())
                return &c;

        return nullptr;
    }

    void testClearRange()
    {
        SessionState s;
        s.addTrack();
        auto& clips = s.tracks[0].clips;
        clips.push_back (makeClip (s, 0, 1000, 0.1f));       // cut back on its right
        clips.push_back (makeClip (s, 1500, 500, 0.2f));     // inside: removed
        clips.push_back (makeClip (s, 2500, 1000, 0.3f));    // cut back on its left
        clips.push_back (makeClip (s, 5000, 2000, 0.4f));    // split by a second range
        const auto before = s;
        const auto rightId = clips[2].id, splitId = clips[3].id;

        check (SessionEdits::clearRange (s, 0, 800, 3000), "clear range: clears clips under the range");
        const auto& t = s.tracks[0];
        check (t.clips.size() == 3 && clipAt (t, 1700) == nullptr, "clear range: the clip inside it is gone");

        const auto* left = clipAt (t, 0);
        const auto* right = clipAt (t, 3000);
        check (left != nullptr && left->getEnd() == 800 && left->fadeIn == 100 && left->fadeOut == 0,
               "clear range: a clip across the start ends there and loses its fade out");
        check (right != nullptr && right->id == rightId && right->position == 3000 && right->getEnd() == 3500
                   && right->fadeIn == 0 && right->fadeOut == 200,
               "clear range: a clip across the end starts there and loses its fade in");
        check (right != nullptr && sampleOf (*right, 0, 3200) == sampleOf (before.tracks[0].clips[2], 0, 3200)
                   && left != nullptr && sampleOf (*left, 0, 799) == sampleOf (before.tracks[0].clips[0], 0, 799),
               "clear range: what is left plays the same audio at the same time");

        check (SessionEdits::clearRange (s, 0, 5600, 5700), "clear range: a range inside one clip splits it");
        const auto* a = clipAt (s.tracks[0], 5599);
        const auto* b = clipAt (s.tracks[0], 5700);
        check (a != nullptr && b != nullptr && a->id == splitId && b->id != splitId && clipAt (s.tracks[0], 5650) == nullptr
                   && a->fadeIn == 100 && a->fadeOut == 0 && b->fadeIn == 0 && b->fadeOut == 200 && b->getEnd() == 7000,
               "clear range: the split keeps the outer fades, and the second part gets a new id");
        check (a != nullptr && b != nullptr && sampleOf (*b, 0, 5700) == sampleOf (before.tracks[0].clips[3], 0, 5700),
               "clear range: the second part plays on from the end of the range");

        const auto unchanged = s.tracks[0].clips.size();
        check (! SessionEdits::clearRange (s, 0, 4000, 4500) && s.tracks[0].clips.size() == unchanged,
               "clear range: an empty stretch changes nothing");
        check (! SessionEdits::clearRange (s, 3, 0, 100) && ! SessionEdits::clearRange (s, 0, 600, 600),
               "clear range: refuses a missing track or an empty range");
    }

    void testRecordClip()
    {
        SessionState s;
        s.addTrack();
        s.tracks[0].clips.push_back (makeClip (s, 0, 4000, 0.1f));
        s.addBus();

        const auto take = ramp (2, 1000, 0.7f);
        const auto id = SessionEdits::recordClip (s, 0, juce::File ("/takes/Track 1.wav"), take, 1500);
        const auto& t = s.tracks[0];
        const auto* placed = clipAt (t, 1500);
        check (t.clips.size() == 3 && placed != nullptr && placed->id == id && placed->length == 1000 && placed->name == "Track 1",
               "record clip: the take replaces what the track had under it");
        check (clipAt (t, 1499) != nullptr && clipAt (t, 1499)->getEnd() == 1500 && clipAt (t, 2500) != nullptr
                   && clipAt (t, 2500)->position == 2500 && clipAt (t, 2500)->id != id,
               "record clip: the old clip is kept either side of the take");

        const auto tracksBefore = s.tracks.size();
        SessionEdits::recordClip (s, 1, juce::File ("/takes/Take.wav"), take, 0);
        check (s.tracks.size() == tracksBefore + 1 && s.tracks.back().clips.size() == 1 && s.tracks[1].clips.empty(),
               "record clip: onto a bus (or no armed track), the take goes on a new track");
    }

    /** The session player and the recorder run in the same cycles, as the device would run them;
        the input carries the player's output from roundTrip frames earlier. */
    struct Loopback
    {
        Loopback (Session& s, int roundTripFrames) : session (s), roundTrip (roundTripFrames)
        {
            player.setMode (DocumentPlayerProcessor::Mode::session);
            player.prepareToPlay (rate, blockSize);
            recorder.setInput (0, 2);
            juce::BigInteger inputs;
            inputs.setRange (0, 2, true);
            recorder.configure (rate, inputs);
        }

        ~Loopback()  { player.releaseResources(); }

        void cycle()
        {
            juce::AudioBuffer<float> out (2, blockSize);
            juce::MidiBuffer midi;
            player.processBlock (out, midi);

            for (int ch = 0; ch < 2; ++ch)
                sent[(size_t) ch].insert (sent[(size_t) ch].end(), out.getReadPointer (ch), out.getReadPointer (ch) + blockSize);

            // Input frame i of this cycle is what left the output roundTrip frames before it.
            juce::AudioBuffer<float> in (2, blockSize);
            const auto first = (juce::int64) sent[0].size() - blockSize - roundTrip;

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < blockSize; ++i)
                    in.setSample (ch, i, first + i >= 0 ? sent[(size_t) ch][(size_t) (first + i)] : 0.0f);

            float silent[2][blockSize] {};
            float* outs[2] = { silent[0], silent[1] };
            recorder.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 2, outs, 2, blockSize, {});
        }

        Session& session;
        AudioDocument document;
        TrackEffects inserts;
        DocumentPlayerProcessor player { document, session, inserts };
        Recorder recorder { player.getPlayedFrame() };
        int roundTrip;
        std::array<std::vector<float>, 2> sent;
    };

    void testPunchInThroughPlayer()
    {
        constexpr int roundTrip = 731;
        const juce::int64 punchIn = 30011, punchOut = 52003, preroll = 24000;

        SessionState initial;
        initial.sampleRate = rate;
        initial.addTrack();
        SessionClip c;
        c.id = initial.nextId++;
        c.audio = ramp (2, 80000, 0.05f);
        c.length = 80000;
        c.fadeIn = 1000;
        initial.tracks[0].clips.push_back (c);

        Session session;
        session.reset (initial, {});
        const auto before = session.getState();

        Loopback loop (session, roundTrip);
        loop.recorder.start();

        for (int i = 0; i < 6; ++i)     // the recorder runs a few cycles before the player starts
            loop.cycle();

        loop.player.play (punchIn - preroll, std::numeric_limits<juce::int64>::max() / 4);

        while (loop.player.getPlayedFrame().load() < punchOut + roundTrip + 4 * blockSize)
            loop.cycle();

        const auto take = loop.recorder.stop();
        loop.player.pause();
        const auto p = Recorder::place (take, punchIn, punchOut, roundTrip);
        check (p.destStart == punchIn && p.takeEnd - p.takeStart == punchOut - punchIn,
               "track recording: the take covers the punch range (" + juce::String (p.destStart) + ", "
                   + juce::String (p.takeEnd - p.takeStart) + " frames)");

        // What the session played over the punch range, mixed on its own.
        juce::AudioBuffer<float> mix (2, (int) (punchOut - punchIn));
        SessionMixer::Buffers buffers;
        SessionMixer::render (*before, punchIn, mix, 0, mix.getNumSamples(), buffers);

        const AudioSnapshot takeAudio (take.clip.numChannels, take.clip.segments);
        juce::AudioBuffer<float> got (2, mix.getNumSamples());
        takeAudio.read (got, 0, p.takeStart, mix.getNumSamples());
        auto same = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; same && i < mix.getNumSamples(); ++i)
                same = got.getSample (ch, i) == mix.getSample (ch, i);

        check (same, "track recording: the looped-back take matches the session's own mix over the range, bit for bit");

        // Put it on the track: a 1 dB louder take lands in place and the clip is cut around it.
        auto next = *before;
        juce::AudioBuffer<float> louder (got);
        louder.applyGain (juce::Decibels::decibelsToGain (1.0f));
        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (louder)), 0, (juce::int64) got.getNumSamples() } };
        SessionEdits::recordClip (next, 0, juce::File ("/takes/Track 1.wav"), std::make_shared<const AudioSnapshot> (2, std::move (segments)), p.destStart);
        const auto& t = next.tracks[0];
        check (t.clips.size() == 3 && clipAt (t, punchIn - 1)->getEnd() == punchIn && clipAt (t, punchOut)->position == punchOut
                   && clipAt (t, punchIn)->length == punchOut - punchIn,
               "track recording: the old clip ends at the punch-in and picks up again at the punch-out");
    }

    void testRecordPastTheEnd()
    {
        // An empty session: the player plays silence past its end, so the take still lines up.
        Session session;
        SessionState empty;
        empty.sampleRate = rate;
        empty.addTrack();
        session.reset (empty, {});

        constexpr int roundTrip = 300;
        const juce::int64 punchIn = 48000;
        Loopback loop (session, roundTrip);
        loop.recorder.start();
        loop.player.play (punchIn - 24000, std::numeric_limits<juce::int64>::max() / 4);

        for (int i = 0; i < 400; ++i)
            loop.cycle();

        check (loop.player.isPlaying(), "track recording: an empty session keeps playing while recording");
        const auto take = loop.recorder.stop();
        loop.player.pause();
        const auto p = Recorder::place (take, punchIn, -1, roundTrip);
        check (take.anchorPlayed >= 0 && p.destStart == punchIn && p.takeEnd == take.clip.getNumFrames()
                   && p.takeEnd - p.takeStart == 400 * blockSize - 24000 - roundTrip,
               "track recording: past the end the take starts at the cursor (" + juce::String (p.takeEnd - p.takeStart) + " frames)");

        session.reset (empty, {});
        Loopback normal (session, 0);
        normal.player.play (0, -1);
        normal.cycle();
        check (! normal.player.isPlaying(), "track recording: plain playback of an empty session still stops at its end");
    }
}

void runTrackRecordingTests()
{
    testClearRange();
    testRecordClip();
    testPunchInThroughPlayer();
    testRecordPastTheEnd();
}
