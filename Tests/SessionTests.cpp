// The mixer's sums are compared bit for bit where they must be exact.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/AudioFileIO.h"
#include "../Source/SessionMixer.h"
#include "TestUtils.h"
#include "SessionTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    /** A snapshot whose sample i of channel ch is (i + 1) / 100000 * (ch == 0 ? 1 : -1): easy to trace. */
    AudioDocument::Snapshot ramp (int channels, int frames)
    {
        juce::AudioBuffer<float> b (channels, frames);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, (float) (i + 1) / 100000.0f * (ch == 0 ? 1.0f : -1.0f));

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, frames } };
        return std::make_shared<const AudioSnapshot> (channels, std::move (segments));
    }

    SessionClip clipOf (SessionState& s, AudioDocument::Snapshot audio, juce::int64 position)
    {
        SessionClip c;
        c.id = s.nextId++;
        c.name = "clip";
        c.audio = audio;
        c.position = position;
        c.length = audio->getNumFrames();
        return c;
    }

    juce::AudioBuffer<float> mix (const SessionState& s, juce::int64 start, int frames)
    {
        juce::AudioBuffer<float> out (2, frames), scratch;
        SessionMixer::render (s, start, out, 0, frames, scratch);
        return out;
    }

    bool near (float a, float b)                  { return std::abs (a - b) <= 1.0e-7f; }
}

void runSessionTests (juce::AudioFormatManager& formats)
{
    // A mono clip at the centre: -3 dB each side, starting at its position.
    {
        SessionState s;
        s.sampleRate = rate;
        s.addTrack().clips.push_back (clipOf (s, ramp (1, 1000), 100));
        const auto out = mix (s, 0, 2000);
        const auto g = std::sqrt (0.5f);
        bool ok = out.getSample (0, 99) == 0.0f && out.getSample (1, 99) == 0.0f
                  && near (out.getSample (0, 100), 0.00001f * g) && near (out.getSample (1, 100), 0.00001f * g)
                  && near (out.getSample (0, 1099), 0.01f * g) && out.getSample (0, 1100) == 0.0f;
        check (ok, "mixer: a mono clip plays at its position, -3 dB each side at the centre");
        check (std::abs (s.getLength() - 1100) == 0, "mixer: the session ends where its last clip does");
    }

    // Pan laws.
    {
        const auto [l, r] = SessionMixer::panGains (-1.0f, 1);
        const auto [sl, sr] = SessionMixer::panGains (0.0f, 2);
        const auto [hl, hr] = SessionMixer::panGains (0.5f, 2);
        check (near (l, 1.0f) && near (r, 0.0f), "mixer: a mono source hard left plays only on the left at full level");
        check (sl == 1.0f && sr == 1.0f, "mixer: a stereo source at the centre is untouched");
        check (near (hl, std::sqrt (0.5f)) && hr == 1.0f, "mixer: a stereo source half right lowers the left by 3 dB");
    }

    // A stereo clip passes through bit for bit; gain, mute and solo.
    {
        SessionState s;
        s.sampleRate = rate;
        s.addTrack().clips.push_back (clipOf (s, ramp (2, 500), 0));
        auto out = mix (s, 0, 500);
        bool exact = true;

        for (int i = 0; i < 500; ++i)
            exact = exact && out.getSample (0, i) == (float) (i + 1) / 100000.0f && out.getSample (1, i) == -(float) (i + 1) / 100000.0f;

        check (exact, "mixer: a stereo clip at 0 dB and centre comes out bit for bit");

        s.tracks[0].gainDb = -20.0f;
        out = mix (s, 0, 500);
        check (near (out.getSample (0, 499), 0.005f * 0.1f), "mixer: track gain -20 dB");

        s.tracks[0].gainDb = 0.0f;
        s.tracks[0].clips[0].gainDb = 6.0f;
        out = mix (s, 0, 500);
        check (near (out.getSample (0, 499), 0.005f * juce::Decibels::decibelsToGain (6.0f)), "mixer: clip gain +6 dB");
        s.tracks[0].clips[0].gainDb = 0.0f;

        auto& second = s.addTrack();
        second.clips.push_back (clipOf (s, ramp (2, 500), 0));
        out = mix (s, 0, 500);
        check (out.getSample (0, 499) == 0.01f, "mixer: two tracks add");

        s.tracks[1].mute = true;
        check (mix (s, 0, 500).getSample (0, 499) == 0.005f, "mixer: a muted track is silent");

        s.tracks[1].mute = false;
        s.tracks[1].solo = true;
        s.tracks[1].gainDb = -6.0f;
        check (near (mix (s, 0, 500).getSample (0, 499), 0.005f * juce::Decibels::decibelsToGain (-6.0f)),
               "mixer: with a solo, only the soloed track plays");
    }

    // Trimming and fades.
    {
        SessionState s;
        s.sampleRate = rate;
        auto c = clipOf (s, ramp (2, 1000), 50);
        c.offset = 200;
        c.length = 300;
        s.addTrack().clips.push_back (c);
        auto out = mix (s, 0, 1000);
        check (out.getSample (0, 49) == 0.0f && out.getSample (0, 50) == 201.0f / 100000.0f
                   && out.getSample (0, 349) == 500.0f / 100000.0f && out.getSample (0, 350) == 0.0f,
               "mixer: a trimmed clip plays only its part of the audio");

        s.tracks[0].clips[0].fadeIn = 100;
        s.tracks[0].clips[0].fadeOut = 100;
        out = mix (s, 0, 1000);
        check (out.getSample (0, 50) == 0.0f && near (out.getSample (0, 100), 0.5f * 251.0f / 100000.0f)
                   && out.getSample (0, 150) == 301.0f / 100000.0f && out.getSample (0, 349) == 0.0f,
               "mixer: raised-cosine fades start and end at silence, half level halfway");
    }

    // Rendering in small blocks gives exactly what one big block does.
    {
        SessionState s;
        s.sampleRate = rate;
        auto& t = s.addTrack();
        t.pan = -0.3f;
        auto c = clipOf (s, ramp (1, 3000), 123);
        c.fadeIn = 333;
        c.fadeOut = 77;
        t.clips.push_back (c);
        s.addTrack().clips.push_back (clipOf (s, ramp (2, 2000), 1500));

        const auto whole = mix (s, 0, 5000);
        juce::AudioBuffer<float> pieces (2, 5000), scratch;

        for (int p = 0; p < 5000; p += 7)
            SessionMixer::render (s, p, pieces, p, juce::jmin (7, 5000 - p), scratch);

        bool same = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 5000; ++i)
                same = same && whole.getSample (ch, i) == pieces.getSample (ch, i);

        check (same, "mixer: 7-frame blocks render bit for bit the same as one block");
    }

    // The player in session mode plays the mix, and stops at the stop frame.
    {
        AudioDocument document;
        Session session;
        SessionState s;
        s.sampleRate = rate;
        s.addTrack().clips.push_back (clipOf (s, ramp (2, 3000), 256));
        session.reset (s, {});

        DocumentPlayerProcessor player (document, session);
        player.setMode (DocumentPlayerProcessor::Mode::session);
        player.prepareToPlay (rate, 512);
        player.play (0, 2048);

        juce::AudioBuffer<float> out (2, 4096), block (2, 512);
        juce::MidiBuffer midi;

        for (int p = 0; p < 4096; p += 512)
        {
            player.processBlock (block, midi);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, p, block, ch, 0, 512);
        }

        const auto expected = mix (s, 0, 4096);
        float worst = 0.0f;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 2048; ++i)
                worst = juce::jmax (worst, std::abs (out.getSample (ch, i) - expected.getSample (ch, i)));

        check (player.getSourceRate() == rate && worst < 1.0e-6f,
               "player: session mode plays the mix (worst difference " + juce::String (worst, 9) + ")");
        check (out.getMagnitude (2048 + 64, 4096 - 2048 - 64) == 0.0f, "player: session playback stops at the stop frame");
        player.releaseResources();
    }

    // Clip edits.
    {
        SessionState s;
        s.sampleRate = rate;
        const auto id = SessionEdits::addClip (s, -1, juce::File ("/a.wav"), ramp (2, 4000), 1000);
        check (s.tracks.size() == 1 && s.tracks[0].clips[0].name == "a" && s.tracks[0].clips[0].length == 4000,
               "edits: adding a clip to no track makes a track");

        s.tracks[0].clips[0].fadeIn = 50;
        s.tracks[0].clips[0].fadeOut = 70;
        const auto before = mix (s, 0, 6000);
        auto split = s;
        check (SessionEdits::splitClip (split, id, 2500) && split.tracks[0].clips.size() == 2, "edits: split makes two clips");
        check (! SessionEdits::splitClip (split, id, 1000) && ! SessionEdits::splitClip (split, id, 2500),
               "edits: a split at a clip's edge does nothing");
        const auto after = mix (split, 0, 6000);
        bool same = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 6000; ++i)
                same = same && before.getSample (ch, i) == after.getSample (ch, i);

        check (same, "edits: a split clip plays bit for bit like the whole one, fades included");

        auto trimmed = s;
        trimmed.tracks[0].clips[0].fadeIn = trimmed.tracks[0].clips[0].fadeOut = 0;   // fades follow the edges
        SessionEdits::trimClip (trimmed, id, 1500, 3000);
        const auto t = mix (trimmed, 0, 6000);
        bool aligned = t.getSample (0, 1499) == 0.0f && t.getSample (0, 3000) == 0.0f;

        for (int i = 1500; i < 3000; ++i)
            aligned = aligned && t.getSample (0, i) == (float) (i - 1000 + 1) / 100000.0f;

        check (aligned, "edits: a trimmed clip keeps its audio in place in time");

        SessionEdits::trimClip (trimmed, id, 0, 99999);
        auto& tc = trimmed.tracks[0].clips[0];
        check (tc.position == 1000 && tc.offset == 0 && tc.length == 4000, "edits: trimming stops at the ends of the audio");

        auto moved = s;
        check (SessionEdits::moveClip (moved, id, 1, -50) && moved.tracks.size() == 2 && moved.tracks[0].clips.empty()
                   && moved.tracks[1].clips[0].position == 0,
               "edits: moving past the last track makes a track; positions stop at 0");

        check (SessionEdits::removeClip (moved, id) && moved.tracks[1].clips.empty() && SessionEdits::removeTrack (moved, 0)
                   && moved.tracks.size() == 1,
               "edits: remove a clip and a track");
    }

    // Undo, redo, merged steps and the dirty flag.
    {
        Session session;
        auto s = *session.getState();
        s.addTrack();
        session.apply (s, "Add Track");
        s.tracks[0].gainDb = -1.0f;
        session.apply (s, "Track Gain", true);
        s.tracks[0].gainDb = -2.0f;
        session.apply (s, "Track Gain", true);
        check (session.getUndoDescription() == "Track Gain" && session.getState()->tracks[0].gainDb == -2.0f,
               "session: a dragged control makes one undo step");

        session.undo();
        check (session.getState()->tracks.size() == 1 && session.getState()->tracks[0].gainDb == 0.0f,
               "session: undo takes the gain back in one step");
        session.undo();
        check (session.getState()->tracks.empty() && ! session.canUndo() && ! session.isDirty(), "session: undo to the start is clean");
        session.redo();
        check (session.getState()->tracks.size() == 1 && session.isDirty(), "session: redo");
    }

    // Session files: audio by relative path, read once, converted to the session rate.
    {
        const auto dir = juce::File::createTempFile ("sbsession");
        dir.createDirectory();
        const auto wav = dir.getChildFile ("audio/take.wav");
        wav.getParentDirectory().createDirectory();
        const auto audio = ramp (2, 4800);
        AudioFileIO::save (*audio, rate, wav, 32, formats);

        const auto slow = dir.getChildFile ("audio/slow.wav");
        AudioFileIO::save (*ramp (1, 4410), 44100.0, slow, 32, formats);

        SessionState s;
        s.sampleRate = rate;
        auto& t = s.addTrack();
        t.name = "Vocals";
        t.gainDb = -3.5f;
        t.pan = 0.25f;
        t.mute = true;
        auto c = clipOf (s, audio, 1000);
        c.source = wav;
        c.offset = 100;
        c.length = 2000;
        c.gainDb = 1.5f;
        c.fadeIn = 10;
        c.fadeOut = 20;
        t.clips.push_back (c);
        auto again = clipOf (s, audio, 5000);
        again.source = wav;
        t.clips.push_back (again);

        auto& u = s.addTrack();
        auto other = clipOf (s, ramp (1, 4410), 0);
        other.source = slow;
        u.clips.push_back (other);

        const auto sessionFile = dir.getChildFile ("song.sbsession");
        check (SessionFile::save (s, sessionFile).isEmpty(), "session file: saves");

        // Move the whole folder: the relative paths still find the audio.
        const auto moved = dir.getSiblingFile (dir.getFileName() + "-moved");
        dir.moveFileTo (moved);
        const auto loaded = SessionFile::load (moved.getChildFile ("song.sbsession"), formats);
        check (loaded.error.isEmpty() && loaded.missing.isEmpty() && loaded.state.tracks.size() == 2,
               "session file: loads after the folder moved");

        if (loaded.state.tracks.size() == 2 && loaded.state.tracks[0].clips.size() == 2)
        {
            auto& lt = loaded.state.tracks[0];
            auto& lc = lt.clips[0];
            check (lt.name == "Vocals" && lt.gainDb == -3.5f && lt.pan == 0.25f && lt.mute && ! lt.solo,
                   "session file: track settings round-trip");
            check (lc.position == 1000 && lc.offset == 100 && lc.length == 2000 && lc.gainDb == 1.5f && lc.fadeIn == 10 && lc.fadeOut == 20,
                   "session file: clip settings round-trip");
            check (lc.audio == lt.clips[1].audio, "session file: a file used twice is read once");
            check (lc.source == moved.getChildFile ("audio/take.wav"), "session file: clips point at the moved audio");

            // The first track only: the second track's audio was converted to the session rate.
            auto unmuted = loaded.state;
            unmuted.tracks[0].mute = false;
            unmuted.tracks.pop_back();
            auto original = s;
            original.tracks[0].mute = false;
            original.tracks.pop_back();
            auto a = mix (unmuted, 1000, 1500), b = mix (original, 1000, 1500);
            bool same = true;

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 1500; ++i)
                    same = same && a.getSample (ch, i) == b.getSample (ch, i);

            check (same, "session file: the loaded session mixes bit for bit like the saved one (float WAV)");

            const auto& converted = loaded.state.tracks[1].clips[0];
            check (converted.audio->getNumFrames() == 4800, "session file: a 44.1 kHz file joins a 48 kHz session converted ("
                                                               + juce::String (converted.audio->getNumFrames()) + " frames)");
        }

        moved.getChildFile ("audio/slow.wav").deleteFile();
        const auto partial = SessionFile::load (moved.getChildFile ("song.sbsession"), formats);
        check (partial.missing.size() == 1 && partial.state.tracks.size() == 2 && partial.state.tracks[1].clips.empty(),
               "session file: a missing audio file is reported and its clips left out");

        moved.deleteRecursively();
    }
}
