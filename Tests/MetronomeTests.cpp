// Clicks must land on the beat grid exactly, without touching the mix.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/Metronome.h"
#include "../Source/TapTempo.h"
#include "../Source/TimeEntry.h"
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
                juce::AudioBuffer<float> out (DocumentPlayerProcessor::clickChannel + 2, blockSize);
                juce::MidiBuffer midi;
                player.processBlock (out, midi);

                for (int ch = 0; ch < 4; ++ch)   // the mix, then the click
                {
                    const auto* from = out.getReadPointer (ch < 2 ? ch : DocumentPlayerProcessor::clickChannel + ch - 2);
                    sent[(size_t) ch].insert (sent[(size_t) ch].end(), from, from + blockSize);
                }
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

    /** The click expected at a session frame on its tempo map. */
    float clickOnMap (const SessionState& s, juce::int64 frame)
    {
        const auto map = s.tempoMap();
        const auto beat = map.beatAt (frame);
        return Metronome::sample (frame - map.beatFrame (beat), map.barAndBeat (beat).second == 0, s.sampleRate);
    }

    void testTempoMap()
    {
        // 120 a minute in fours, 90 in threes from bar 2 (counted from 0), 150 in fours from bar 4.
        SessionState s;
        s.sampleRate = rate;
        const auto refused = ! SessionEdits::setTempoChange (s, { 0, 90.0, 3 }) && ! SessionEdits::setTempoChange (s, { 2, 19.0, 3 })
                             && ! SessionEdits::setTempoChange (s, { 2, 90.0, 17 });
        const auto added = SessionEdits::setTempoChange (s, { 4, 150.0, 4 }) && SessionEdits::setTempoChange (s, { 2, 100.0, 3 })
                           && SessionEdits::setTempoChange (s, { 2, 90.0, 3 }) && ! SessionEdits::setTempoChange (s, { 2, 90.0, 3 });
        check (refused && added && s.tempoChanges.size() == 2 && s.tempoChanges[0].bar == 2 && juce::exactlyEqual (s.tempoChanges[0].tempo, 90.0)
                   && s.tempoChanges[1].bar == 4,
               "tempo map: changes after the first bar, in range, one a bar (replaced), kept in order");

        const auto map = s.tempoMap();
        check (map.beatFrame (8) == 192000 && map.beatFrame (9) == 224000 && map.beatFrame (14) == 384000 && map.beatFrame (15) == 403200
                   && map.beatAt (223999) == 8 && map.beatAt (224000) == 9 && map.beatAt (-1) == -1
                   && map.barAndBeat (13) == std::pair<juce::int64, int> (3, 2) && map.barAndBeat (14) == std::pair<juce::int64, int> (4, 0)
                   && map.barFrame (3) == 288000 && map.barAt (383999) == 3 && map.barAt (384000) == 4
                   && juce::exactlyEqual (map.tempoAt (191999), 120.0) && juce::exactlyEqual (map.tempoAt (192000), 90.0) && map.beatsPerBarAt (300000) == 3
                   && juce::exactlyEqual (map.shortestBeat(), 19200.0) && juce::exactlyEqual (map.shortestBar(), 76800.0),
               "tempo map: beats, bars, tempo and beats in a bar follow each change from its bar on");

        std::vector<juce::int64> bars, beats;
        map.forEachLine (150000, 420000, Metronome::Grid { 2 }, [&] (juce::int64 frame, juce::int64) { bars.push_back (frame); });
        map.forEachLine (370000, 410000, Metronome::Grid {}, [&] (juce::int64 frame, juce::int64) { beats.push_back (frame); });
        check (bars == std::vector<juce::int64> { 192000, 384000 } && beats == std::vector<juce::int64> { 384000, 403200 }
                   && map.linesAround (300000, {}) == std::pair<juce::int64, juce::int64> (288000, 320000)
                   && map.linesAround (300000, Metronome::Grid { 2 }) == std::pair<juce::int64, juce::int64> (192000, 384000),
               "tempo map: grid lines every beat or every two bars across the changes");

        // Beats between frames: every frame lies in the beat beatAt gives, and with no changes the
        // beats are the plain grid's.
        SessionState odd;
        odd.sampleRate = 44100.0;
        odd.tempo = 133.7;
        odd.beatsPerBar = 3;
        const auto plain = odd.tempoMap();
        auto same = true;

        for (juce::int64 b = -5; b < 3000; ++b)
            same = same && plain.beatFrame (b) == Metronome::beatFrame (b, 133.7, 44100.0);

        SessionEdits::setTempoChange (odd, { 3, 97.3, 5 });
        SessionEdits::setTempoChange (odd, { 7, 211.1, 7 });
        const auto oddMap = odd.tempoMap();
        auto inside = true;

        for (juce::int64 frame = -100; frame < 800000; frame += 37)
        {
            const auto beat = oddMap.beatAt (frame);
            inside = inside && oddMap.beatFrame (beat) <= frame && frame < oddMap.beatFrame (beat + 1);
        }

        check (same && inside, "tempo map: with no changes the plain grid exactly; with changes, every frame inside the beat it is in");

        // The click follows the map: from bar 1 at 63066, 200 a minute in twos.
        auto clicking = toneSession();
        SessionEdits::setTempoChange (clicking, { 1, 200.0, 2 });
        Session session;
        session.reset (clicking, {});
        const juce::int64 start = 30011;
        Rig on (session);
        on.player.setMetronome (true);
        on.player.play (start, -1);
        on.run (220);
        on.player.pause();
        auto followed = true;

        for (size_t j = 0; j < on.sent[2].size(); ++j)
            followed = followed && on.sent[2][j] == clickOnMap (clicking, start + (juce::int64) j);

        const auto clickStarts = [&] (juce::int64 frame) { return on.sent[2][(size_t) (frame - start + 1)] != 0.0f && on.sent[2][(size_t) (frame - start - 1)] == 0.0f; };
        check (followed && clickStarts (63066) && clickStarts (63066 + 14400) && ! clickStarts (63066 + 21022),
               "tempo map: the metronome clicks on the changed tempo from its bar, bit for bit");

        // Snapping to the grid follows it too.
        SessionSnap::Targets grid;
        grid.beatGrid = Metronome::Grid {};
        SessionSnap::Targets everyBar;
        everyBar.beatGrid = Metronome::Grid { 1 };
        check (SessionSnap::snap (s, 224000 + 300, 1000, grid) == 224000 && SessionSnap::snap (s, 288000 - 700, 1000, everyBar) == 288000
                   && SessionSnap::snap (s, 256000, 1000, everyBar) == 256000,
               "tempo map: snapping lands on the beats and bars of the changed tempo");
    }

    void testLoopPlayback()
    {
        // The open file looping over a selection: 1000 to 3000 again and again, each frame's
        // sample telling where it came from.
        Session session;
        Rig rig (session);
        juce::AudioBuffer<float> b (1, 20000);

        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, (float) i * 1.0e-5f);

        rig.document.reset (AudioEdits::fromBuffer (std::move (b)), rate, {}, 24);
        rig.player.setMode (DocumentPlayerProcessor::Mode::document);
        rig.player.prepareToPlay (rate, blockSize);
        rig.player.play (1000, 3000);
        rig.player.setLoop (1000, 3000);
        rig.run (40);
        const auto position = rig.player.getPositionFrames();
        rig.player.clearLoop();
        rig.run (20);
        rig.player.pause();

        auto looped = true;

        for (size_t j = 0; j < 40 * blockSize; ++j)
            looped = looped && rig.sent[0][j] == (float) (1000 + (int) (j % 2000)) * 1.0e-5f && rig.sent[1][j] == rig.sent[0][j];

        // Cleared, it plays on to the selection's end and then silence.
        const auto after = 40 * blockSize;
        const auto resumed = 1000 + (int) (after % 2000);
        auto stopped = true;

        for (size_t j = after; j < rig.sent[0].size(); ++j)
        {
            const auto frame = resumed + (int) (j - after);
            stopped = stopped && rig.sent[0][j] == (frame < 3000 ? (float) frame * 1.0e-5f : 0.0f);
        }

        check (looped && stopped && position >= 1000 && position < 3000,
               "loop playback: the file plays its selection over and over, sample for sample; cleared, it plays to the end of the selection and stops");
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
        beats.beatGrid = Metronome::Grid {};
        check (SessionSnap::snap (s, beat (10) + 37, 100, beats) == beat (10) && SessionSnap::snap (s, beat (11) - 41, 100, beats) == beat (11)
                   && SessionSnap::snap (s, beat (10) + 500, 100, beats) == beat (10) + 500,
               "beat snapping: a frame near a beat lands on the beat's frame; one further than the tolerance stays put");

        SessionSnap::Targets bars;
        bars.beatGrid = Metronome::Grid { 1 };
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

        auto changed = s;
        SessionEdits::setTempoChange (changed, { 8, 141.5, 5 });
        SessionEdits::setTempoChange (changed, { 3, 60.0, 2 });
        const auto changedFile = juce::File::createTempFile (".sbsession");
        const auto changedError = SessionFile::save (changed, changedFile);
        const auto changedLoaded = SessionFile::load (changedFile, formats);
        changedFile.deleteFile();
        check (changedError.isEmpty() && changedLoaded.state.tempoChanges == changed.tempoChanges && changedLoaded.state.tempoChanges.size() == 2,
               "tempo map: the changes come back from a session file, in order");
        check (juce::exactlyEqual (SessionState().tempo, 120.0) && SessionState().beatsPerBar == 4,
               "metronome: a new session is 120 a minute, four beats in a bar");
    }
}

namespace
{
    void testTapTempo()
    {
        // Taps half a second apart: 120 beats a minute from the second tap.
        TapTempo taps;
        const auto first = taps.tap (10.0);
        const auto second = taps.tap (10.5);
        const auto third = taps.tap (11.0);
        check (! first && second == 120.0 && third == 120.0 && taps.getNumIntervals() == 2,
               "tap tempo: nothing from one tap, then the tempo of their spacing");

        // Uneven taps average: 0.5, 0.6, 0.4 and 0.5 s give 0.5 s, 120. A 0.7 s spacing gives 85.71.
        TapTempo uneven;
        for (auto t : { 0.0, 0.5, 1.1, 1.5 })
            uneven.tap (t);

        TapTempo odd;
        odd.tap (0.0);
        const auto oddTempo = odd.tap (0.7);   // 85.714... beats a minute
        check (uneven.tap (2.0) == 120.0 && oddTempo == 85.71,
               "tap tempo: the spacings are averaged, and the tempo rounded to hundredths");

        // Only the last 8 spacings count: 8 at 0.25 s after 4 at 1 s give 240.
        TapTempo window;
        auto at = 0.0;
        window.tap (at);

        for (int i = 0; i < 4; ++i)
            window.tap (at += 1.0);

        std::optional<double> last;

        for (int i = 0; i < 8; ++i)
            last = window.tap (at += 0.25);

        check (last == 240.0 && window.getNumIntervals() == TapTempo::maxIntervals, "tap tempo: only the last 8 spacings are averaged");

        // A pause of 3 s starts again; so does going back in time. Very fast taps stop at 400.
        TapTempo paused;
        paused.tap (0.0);
        paused.tap (0.5);
        const auto afterPause = paused.tap (3.5);
        const auto resumed = paused.tap (4.5);
        TapTempo backwards;
        backwards.tap (5.0);
        const auto back = backwards.tap (4.0);
        TapTempo fast;
        fast.tap (0.0);
        check (! afterPause && resumed == 60.0 && ! back && fast.tap (0.05) == 400.0,
               "tap tempo: a pause of 3 s or a step back starts again; the tempo stays at most 400");

        // On a session: one undo step for a run, a new one for the next run; the same tempo again
        // changes nothing.
        Session session;
        SessionState s;
        s.tempo = 100.0;
        session.reset (s, {});
        TapTempo tapper;
        const auto none = ! tapSessionTempo (session, tapper, 0.0) && juce::exactlyEqual (session.getState()->tempo, 100.0);
        const auto set = tapSessionTempo (session, tapper, 0.5) && juce::exactlyEqual (session.getState()->tempo, 120.0)
                         && session.getUndoDescription() == "Tap Tempo";
        tapSessionTempo (session, tapper, 1.1);   // 0.5 and 0.6 s: 109.09
        tapSessionTempo (session, tapper, 1.5);   // with 0.4 s: 120 again
        const auto same = ! tapSessionTempo (session, tapper, 2.0) && juce::exactlyEqual (session.getState()->tempo, 120.0);
        session.undo();
        const auto oneStep = juce::exactlyEqual (session.getState()->tempo, 100.0) && ! session.canUndo();
        session.redo();
        tapSessionTempo (session, tapper, 10.0);
        tapSessionTempo (session, tapper, 11.0);
        session.undo();
        const auto secondRun = juce::exactlyEqual (session.getState()->tempo, 120.0);
        check (none && set && same && oneStep && secondRun,
               "tap tempo: a run of taps sets the session's tempo as one undo step, and the next run is another");
    }
}

namespace
{
    void testTimeEntry()
    {
        using Frame = std::optional<juce::int64>;
        const auto at = [] (const char* text) { return TimeEntry::parse (text, 48000.0); };
        check (at ("83.5") == Frame (4008000) && at ("83.5 s") == Frame (4008000) && at ("1:23.5") == Frame (4008000)
                   && at (" 2:00 ") == Frame (5760000) && at ("1:01:23.5") == Frame (176808000) && at ("0") == Frame (0)
                   && at ("0.00001") == Frame (0) && at (".5") == Frame (24000),
               "go to time: seconds, minutes and seconds, and hours, minutes and seconds");

        auto bad = 0;

        for (auto text : { "", "abc", "-1", "1:75", "1.5:20", "1:2:3:4", "1..2", "5|3", "1:", ":5", "1e3" })
            bad += at (text).has_value() ? 1 : 0;

        check (bad == 0, "go to time: negative, malformed and out-of-range times, and bars without a tempo map, are refused (" + juce::String (bad) + " read)");

        // 120 beats a minute in 4 from the start (24000 frames a beat); 60 in 3 from the third bar.
        const std::vector<Metronome::TempoChange> changes { { 2, 60.0, 3 } };
        const Metronome::TempoMap map (120.0, 4, changes, 48000.0);
        const auto bars = [&map] (const char* text) { return TimeEntry::parse (text, 48000.0, &map); };
        check (bars ("1|1") == Frame (0) && bars ("5|3") == Frame (576000) && bars ("2|4.5") == Frame (180000)
                   && bars ("3|2") == Frame (240000) && bars (" 3 | 3 ") == Frame (288000) && bars ("83.5") == Frame (4008000),
               "go to time: bar and beat, a fraction of a beat, across a change of tempo and time signature");

        auto badBars = 0;

        for (auto text : { "0|1", "1|0", "1|5", "3|4", "1.5|1", "2|", "|2", "1|2|3" })
            badBars += bars (text).has_value() ? 1 : 0;

        check (badBars == 0, "go to time: bars and beats that don't exist are refused (" + juce::String (badBars) + " read)");

        // Set Selection: a start and length in a 10 s file, the end cut to the file's.
        using Span = std::optional<juce::Range<juce::int64>>;
        const auto span = [] (const char* start, const char* length) { return TimeEntry::parseRange (start, length, 48000.0, 480000); };
        check (span ("1.5", "2") == Span ({ 72000, 168000 }) && span ("0:09", "0:05") == Span ({ 432000, 480000 })
                   && span ("0", "10") == Span ({ 0, 480000 }),
               "set selection: start and length typed as times, the end cut to the file's");
        check (! span ("10", "1") && ! span ("1", "0") && ! span ("abc", "1") && ! span ("1", "") && ! span ("-1", "1"),
               "set selection: a start at or past the end, a zero length and unreadable times are refused");

        check (TimeEntry::describe ({ 72000, 168000 }, 48000.0) == "0:01.500\t0:03.500\t0:02.000"
                   && TimeEntry::format (48000 * 3725 + 24, 48000.0) == "1:02:05.001" && TimeEntry::format (48000 * 600, 48000.0) == "10:00.000"
                   && TimeEntry::parse (TimeEntry::format (4008000, 48000.0), 48000.0) == Frame (4008000),
               "copy selection time: start, end and length as text that Go to Time reads back; hours from an hour on");
        check (TimeEntry::parseOffset ("-0.5", 48000.0) == Frame (-24000) && TimeEntry::parseOffset ("+1:00", 48000.0) == Frame (2880000)
                   && TimeEntry::parseOffset (" 2 ", 48000.0) == Frame (96000) && ! TimeEntry::parseOffset ("--1", 48000.0) && ! TimeEntry::parseOffset ("-", 48000.0),
               "move by: a time with an optional sign, minus for earlier; a doubled sign or no time refused");
    }
}

void runMetronomeTests (juce::AudioFormatManager& formats)
{
    testGrid();
    testTempoMap();
    testLoopPlayback();
    testClicks();
    testSnapping();
    testFile (formats);
    testTapTempo();
    testTimeEntry();
}
