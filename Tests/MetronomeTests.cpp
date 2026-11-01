// Clicks must land on the beat grid exactly, without touching the mix.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/Metronome.h"
#include "TestUtils.h"
#include "MetronomeTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;
    constexpr int blockSize = 256;

    void testGrid()
    {
        // An awkward tempo, so beats fall between frames.
        constexpr double tempo = 133.7;
        auto inside = true;

        for (juce::int64 frame = 0; inside && frame < 200000; frame += 7)
        {
            const auto beat = Metronome::beatAt (frame, tempo, 44100.0);
            inside = Metronome::beatFrame (beat, tempo, 44100.0) <= frame && frame < Metronome::beatFrame (beat + 1, tempo, 44100.0);
        }

        check (inside && Metronome::beatFrame (1000, tempo, 44100.0) == std::llround (1000.0 * 60.0 * 44100.0 / tempo),
               "metronome: every frame belongs to the last beat starting at or before it, and beats don't drift");
        check (Metronome::barFrames (120.0, 3, rate) == 72000 && Metronome::beatAt (-1, 120.0, rate) == -1,
               "metronome: a bar of three beats at 120 is 1.5 s; frames before the start belong to beat -1");

        const auto length = (int) std::llround (Metronome::clickSeconds * rate);
        auto accentPeak = 0.0f, beatPeak = 0.0f;

        for (int i = 0; i < length; ++i)
        {
            accentPeak = juce::jmax (accentPeak, std::abs (Metronome::sample (i, true, rate)));
            beatPeak = juce::jmax (beatPeak, std::abs (Metronome::sample (i, false, rate)));
        }

        check (accentPeak > beatPeak && accentPeak <= 0.5f && beatPeak > 0.1f && Metronome::sample (0, true, rate) == 0.0f
                   && Metronome::sample (length, true, rate) == 0.0f && Metronome::sample (-1, false, rate) == 0.0f,
               "metronome: a click starts from silence, ends after " + juce::String (length) + " frames, and the bar's first is louder ("
                   + juce::String (accentPeak, 3) + " against " + juce::String (beatPeak, 3) + ")");
    }

    /** The session player, block by block, with the click outputs; keeps all four channels. */
    struct Rig
    {
        Rig (Session& s, double deviceRate = rate) : session (s)
        {
            player.setMode (DocumentPlayerProcessor::Mode::session);
            player.prepareToPlay (deviceRate, blockSize);
        }

        ~Rig()  { player.releaseResources(); }

        void run (int blocks)
        {
            for (int b = 0; b < blocks; ++b)
            {
                juce::AudioBuffer<float> out (4, blockSize);
                juce::MidiBuffer midi;
                player.processBlock (out, midi);

                for (int ch = 0; ch < 4; ++ch)
                    sent[(size_t) ch].insert (sent[(size_t) ch].end(), out.getReadPointer (ch), out.getReadPointer (ch) + blockSize);
            }
        }

        Session& session;
        AudioDocument document;
        TrackEffects inserts;
        DocumentPlayerProcessor player { document, session, inserts };
        std::array<std::vector<float>, 4> sent;
    };

    /** The click expected at a session frame (0 where none should sound). */
    float clickAt (const SessionState& s, juce::int64 frame)
    {
        const auto beat = Metronome::beatAt (frame, s.tempo, s.sampleRate);
        return Metronome::sample (frame - Metronome::beatFrame (beat, s.tempo, s.sampleRate), beat % s.beatsPerBar == 0, s.sampleRate);
    }

    SessionState toneSession()
    {
        SessionState s;
        s.sampleRate = rate;
        s.tempo = 137.0;
        s.beatsPerBar = 3;
        s.addTrack();
        juce::AudioBuffer<float> b (1, 200000);

        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.25f * std::sin ((float) i * 0.01f));

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, 200000 } };
        SessionEdits::addClip (s, 0, juce::File ("/takes/tone.wav"), std::make_shared<const AudioSnapshot> (1, std::move (segments)), 0);
        return s;
    }

    void testClicks()
    {
        const auto initial = toneSession();
        Session session;
        session.reset (initial, {});
        const juce::int64 start = 30011;
        constexpr int blocks = 160;     // past frame 63066, where the second bar starts

        // Metronome on: clicks on the grid, on channels 2 and 3 only; the mix is as without it.
        Rig on (session);
        on.player.setMetronome (true);
        on.player.play (start, -1);
        on.run (blocks);
        on.player.pause();

        Rig off (session);
        off.player.play (start, -1);
        off.run (blocks);
        off.player.pause();

        auto onGrid = true, mixSame = true, silentOff = true;
        int accents = 0;

        for (size_t j = 0; j < on.sent[0].size(); ++j)
        {
            const auto frame = start + (juce::int64) j;
            onGrid = onGrid && on.sent[2][j] == clickAt (initial, frame) && on.sent[3][j] == on.sent[2][j];
            mixSame = mixSame && on.sent[0][j] == off.sent[0][j] && on.sent[1][j] == off.sent[1][j];
            silentOff = silentOff && off.sent[2][j] == 0.0f && off.sent[3][j] == 0.0f;

            if (const auto beat = Metronome::beatAt (frame, initial.tempo, rate); Metronome::beatFrame (beat, initial.tempo, rate) == frame && beat % 3 == 0)
                ++accents;
        }

        check (onGrid && accents > 0, "metronome: playing, a click on every beat of 137 a minute, the first of each three-beat bar accented, "
                                      "bit for bit (" + juce::String (accents) + " bars)");
        check (mixSame && silentOff, "metronome: the mix is untouched by the click, and off there is no click");

        // A count-in to 40000 with the metronome off: clicks up to there, none after.
        Rig counting (session);
        counting.player.setCountIn (40000);
        counting.player.play (start, -1);
        counting.run (blocks);
        counting.player.pause();
        auto countedIn = true;

        for (size_t j = 0; j < counting.sent[2].size(); ++j)
        {
            const auto frame = start + (juce::int64) j;
            countedIn = countedIn && counting.sent[2][j] == (frame < 40000 ? clickAt (initial, frame) : 0.0f);
        }

        check (countedIn, "metronome: a count-in clicks up to its end with the metronome off, then stops");

        // Through a loop the played frame counts on; the click follows the session frame.
        Rig looping (session);
        looping.player.setMetronome (true);
        looping.player.play (start, std::numeric_limits<juce::int64>::max() / 4);
        looping.player.setLoop (35000, 41003);
        looping.run (blocks);
        looping.player.pause();
        auto followed = true;

        for (size_t j = 0; j < looping.sent[2].size(); ++j)
        {
            auto frame = start + (juce::int64) j;

            if (frame >= 41003)
                frame = 35000 + (frame - 35000) % 6003;

            followed = followed && looping.sent[2][j] == clickAt (initial, frame);
        }

        check (followed, "metronome: through a loop the clicks follow the session's beats, pass after pass");

        // A device at another rate than the session: no click (the frames would not be the session's).
        Rig other (session, 44100.0);
        other.player.setMetronome (true);
        other.player.play (start, -1);
        other.run (20);
        other.player.pause();
        check (std::all_of (other.sent[2].begin(), other.sent[2].end(), [] (float v) { return v == 0.0f; }),
               "metronome: with the device at another rate, it stays silent");
    }

    void testSnapping()
    {
        // At 133.7 a minute and 44.1 kHz beats fall between frames; snapping lands on the frame
        // each beat is rounded to, as the metronome clicks there.
        SessionState s;
        s.sampleRate = 44100.0;
        s.tempo = 133.7;
        s.beatsPerBar = 3;
        s.addTrack();
        const auto beat = [&] (juce::int64 b) { return Metronome::beatFrame (b, s.tempo, s.sampleRate); };

        SessionSnap::Targets beats;
        beats.gridBeats = 1;
        check (SessionSnap::snap (s, beat (10) + 37, 100, beats) == beat (10) && SessionSnap::snap (s, beat (11) - 41, 100, beats) == beat (11)
                   && SessionSnap::snap (s, beat (10) + 500, 100, beats) == beat (10) + 500,
               "beat snapping: a frame near a beat lands on the beat's frame; one further than the tolerance stays put");

        SessionSnap::Targets bars;
        bars.gridBeats = 3;
        check (SessionSnap::snap (s, beat (11) - 5, 20000, bars) == beat (12) && SessionSnap::snap (s, beat (10) + 5, 20000, bars) == beat (9)
                   && SessionSnap::snap (s, beat (10) + 5, 100, bars) == beat (10) + 5,
               "beat snapping: on a grid of bars, only bar starts are targets");

        SessionSnap::Targets withCursor = beats;
        withCursor.extra = { beat (10) + 30 };
        check (SessionSnap::snap (s, beat (10) + 25, 100, withCursor) == beat (10) + 30
                   && SessionSnap::snapSpan (s, { beat (4) + 60, beat (7) + 9 }, 100, beats) == -9,
               "beat snapping: a nearer target still wins, and a moved span snaps by its nearer edge");
    }

    void testFile (juce::AudioFormatManager& formats)
    {
        SessionState s;
        s.sampleRate = rate;
        s.tempo = 93.25;
        s.beatsPerBar = 7;
        s.addTrack();

        const auto file = juce::File::createTempFile (".sbsession");
        const auto error = SessionFile::save (s, file);
        const auto loaded = SessionFile::load (file, formats);
        file.deleteFile();

        check (error.isEmpty() && loaded.error.isEmpty() && juce::exactlyEqual (loaded.state.tempo, 93.25) && loaded.state.beatsPerBar == 7,
               "metronome: the tempo and beats in a bar come back from a session file");
        check (juce::exactlyEqual (SessionState().tempo, 120.0) && SessionState().beatsPerBar == 4,
               "metronome: a new session is 120 a minute, four beats in a bar");
    }
}

void runMetronomeTests (juce::AudioFormatManager& formats)
{
    testGrid();
    testClicks();
    testSnapping();
    testFile (formats);
}
