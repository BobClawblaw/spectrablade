// Levels are checked against what the mixer itself computes.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioFileIO.h"
#include "../Source/Bounce.h"
#include "../Source/ClipList.h"
#include "../Source/ClipProperties.h"
#include "../Source/KeyShortcuts.h"
#include "../Source/LoadReadout.h"
#include "../Source/MarkerTable.h"
#include "../Source/MixerView.h"
#include "../Source/RecentFiles.h"
#include "../Source/MultitrackView.h"
#include "../Source/SessionCollect.h"
#include "../Source/SessionNotes.h"
#include "../Source/SessionRecovery.h"
#include "../Source/TrackColours.h"
#include "../Source/UndoHistory.h"
#include "TestUtils.h"
#include "MixerTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    AudioDocument::Snapshot constant (int channels, int frames, float value)
    {
        juce::AudioBuffer<float> b (channels, frames);

        for (int ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::fill (b.getWritePointer (ch), value * (float) (ch + 1), frames);

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, frames } };
        return std::make_shared<const AudioSnapshot> (channels, std::move (segments));
    }

    /** A mono track at -6 dB panned left a little, a stereo track sending to a bus, the bus, and
        a muted track. */
    SessionState mixSession()
    {
        SessionState s;
        s.sampleRate = rate;
        auto& mono = s.addTrack();
        mono.gainDb = -6.0f;
        mono.pan = -0.3f;
        auto& stereo = s.addTrack();
        stereo.pan = 0.5f;
        auto& bus = s.addBus();
        bus.gainDb = -3.0f;
        const auto busId = bus.id;
        s.addTrack().mute = true;
        SessionEdits::setSend (s, s.tracks[1].id, { busId, -10.0f, false });
        SessionEdits::addClip (s, 0, juce::File ("/takes/mono.wav"), constant (1, 9000, 0.5f), 0);
        SessionEdits::addClip (s, 1, juce::File ("/takes/stereo.wav"), constant (2, 9000, 0.2f), 0);
        SessionEdits::addClip (s, 3, juce::File ("/takes/muted.wav"), constant (1, 9000, 0.9f), 0);
        return s;
    }

    void testLevels()
    {
        const auto s = mixSession();
        SessionMixer::Levels levels;
        SessionMixer::Buffers buffers;
        buffers.levels = &levels;
        juce::AudioBuffer<float> out (2, 4096);
        SessionMixer::render (s, 100, out, 0, 4096, buffers);

        // What the mixer does, step by step: pan into the track, then its volume.
        const auto [monoLeft, monoRight] = SessionMixer::panGains (-0.3f, 1);
        const auto monoVolume = juce::Decibels::decibelsToGain (-6.0f, -150.0f);
        const auto [stereoLeft, stereoRight] = SessionMixer::panGains (0.5f, 2);
        const auto sendGain = juce::Decibels::decibelsToGain (-10.0f, -150.0f);
        const auto [busLeft, busRight] = SessionMixer::panGains (0.0f, 2);
        const auto busVolume = juce::Decibels::decibelsToGain (-3.0f, -150.0f);

        const auto mono = levels.take (0), stereo = levels.take (1), bus = levels.take (2), muted = levels.take (3);
        check (mono.first == (0.5f * monoLeft) * monoVolume && mono.second == (0.5f * monoRight) * monoVolume,
               "meters: a mono track's peaks are its level after its pan and volume (" + juce::String (mono.first, 4) + ", "
                   + juce::String (mono.second, 4) + ")");
        check (stereo.first == 0.2f * stereoLeft && stereo.second == 0.4f * stereoRight,
               "meters: a stereo track's peaks follow its balance, per side");
        check (bus.first == ((0.2f * stereoLeft) * sendGain) * (busVolume * busLeft) && bus.second == ((0.4f * stereoRight) * sendGain) * (busVolume * busRight),
               "meters: a bus's peaks are what its sends bring, after its volume");
        check (muted.first == 0.0f && muted.second == 0.0f && levels.take (0).first == 0.0f,
               "meters: a muted track shows nothing, and taking the peaks starts them again from 0");

        // Automated: the peak is the largest level the envelope gives.
        auto automated = s;
        automated.tracks[0].volumeEnvelope.points = { { 0, -20.0f }, { 4196, 0.0f } };
        SessionMixer::Levels ramped;
        SessionMixer::Buffers rampBuffers;
        rampBuffers.levels = &ramped;
        SessionMixer::render (automated, 100, out, 0, 4096, rampBuffers);
        const auto peak = ramped.take (0).first;
        check (peak > (0.5f * monoLeft) * 0.95f && peak <= 0.5f * monoLeft,
               "meters: under a volume envelope, the peak follows the envelope (" + juce::String (peak, 4) + ")");

        SessionMixer::Levels outside;
        outside.raise (SessionMixer::Levels::maxTracks, 1.0f, 1.0f);
        check (outside.take (SessionMixer::Levels::maxTracks).first == 0.0f && outside.take (-1).first == 0.0f,
               "meters: a track past the last metered place is ignored");
    }

    void testMixerView (AudioEngine& engine)
    {
        engine.session.reset (mixSession(), {});
        MixerView mixer (engine);
        mixer.setSize (MixerView::stripWidth * 6, 400);
        const auto state = engine.session.getState();
        const auto mono = state->tracks[0].id, stereo = state->tracks[1].id, bus = state->tracks[2].id;

        check (mixer.getNumStrips() == 4 && mixer.findFader (mono) != nullptr && mixer.findFader (bus) != nullptr
                   && mixer.findFader (12345) == nullptr && juce::exactlyEqual ((float) mixer.findFader (mono)->getValue(), -6.0f)
                   && juce::exactlyEqual (mixer.findPan (mono)->getValue(), -30.0) && mixer.findMute (state->tracks[3].id)->getToggleState(),
               "mixer: a strip for each track and bus, showing its volume, pan and mute");

        mixer.findFader (stereo)->setValue (-4.5, juce::sendNotificationSync);
        check (juce::exactlyEqual (engine.session.getState()->tracks[1].gainDb, -4.5f) && engine.session.getUndoDescription() == "Track Volume",
               "mixer: moving a fader sets the track's volume, as an undo step");

        mixer.findPan (bus)->setValue (40.0, juce::sendNotificationSync);
        mixer.findSolo (stereo)->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        const auto after = engine.session.getState();
        check (juce::exactlyEqual (after->tracks[2].pan, 0.4f) && after->tracks[1].solo && engine.session.getUndoDescription() == "Solo",
               "mixer: pan and solo work on buses and tracks alike");

        engine.session.undo();
        engine.session.undo();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (! mixer.findSolo (stereo)->getToggleState() && juce::exactlyEqual (mixer.findPan (bus)->getValue(), 0.0),
               "mixer: undo shows on the strips");

        // Meters: the player's levels, falling back once the sound stops.
        auto& levels = engine.getPlayer().getLevels();

        for (int i = 0; i < SessionMixer::Levels::maxTracks; ++i)
            levels.take (i);

        levels.raise (1, 0.5f, 0.25f);
        mixer.updateMeters();
        const auto shown = mixer.getShownLevel (stereo, 0), shownRight = mixer.getShownLevel (stereo, 1);
        mixer.updateMeters();
        const auto fallen = mixer.getShownLevel (stereo, 0);
        check (shown == 0.5f && shownRight == 0.25f && fallen < shown && fallen > 0.25f && mixer.getShownLevel (mono, 0) == 0.0f,
               "mixer: a strip's meter shows its track's peak, then falls back (" + juce::String (fallen, 3) + ")");

        // An automated volume can't be moved here; a new track gets a strip.
        auto next = *engine.session.getState();
        next.tracks[0].volumeEnvelope.points = { { 0, -12.0f } };
        next.addTrack();
        engine.session.apply (std::move (next), "Test");
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (! mixer.findFader (mono)->isEnabled() && mixer.findFader (stereo)->isEnabled() && mixer.getNumStrips() == 5,
               "mixer: an automated volume follows its envelope; a new track gets a strip");

        engine.session.reset ({}, {});
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (mixer.getNumStrips() == 0, "mixer: an empty session has no strips");
    }
}

namespace
{
    void testGroups (AudioEngine& engine)
    {
        SessionState s;
        s.sampleRate = rate;

        for (auto [name, group, db] : { std::tuple { "Kick", 1, 0.0f }, { "Snare", 1, -6.0f }, { "Vocal", 0, -2.0f }, { "Toms", 1, -58.0f } })
        {
            auto& t = s.addTrack();
            t.name = name;
            t.group = group;
            t.gainDb = db;
        }

        auto automated = s;
        automated.tracks[1].volumeEnvelope.points = { { 0, -6.0f } };

        // Kick down 3 dB: Snare follows by 3 dB, Toms stops at -60, Vocal (no group) stays.
        auto edited = s;
        auto before = edited.tracks[0];
        edited.tracks[0].gainDb = -3.0f;
        SessionEdits::followGroup (edited, before);
        auto withEnvelope = automated;
        before = withEnvelope.tracks[0];
        withEnvelope.tracks[0].gainDb = -3.0f;
        SessionEdits::followGroup (withEnvelope, before);
        check (juce::exactlyEqual (edited.tracks[1].gainDb, -9.0f) && juce::exactlyEqual (edited.tracks[3].gainDb, -60.0f)
                   && juce::exactlyEqual (edited.tracks[2].gainDb, -2.0f) && juce::exactlyEqual (withEnvelope.tracks[1].gainDb, -6.0f),
               "groups: a volume change moves the group's other volumes by as many dB, within limits, not ones on envelopes");

        before = edited.tracks[1];
        edited.tracks[1].mute = true;
        edited.tracks[1].solo = true;
        SessionEdits::followGroup (edited, before);
        check (edited.tracks[0].mute && edited.tracks[3].mute && edited.tracks[0].solo && ! edited.tracks[2].mute && ! edited.tracks[2].solo,
               "groups: mute and solo go to the whole group");

        // Through the mixer window and the multitrack view.
        engine.session.reset (s, {});
        MixerView mixer (engine);
        mixer.setSize (MixerView::stripWidth * 6, 400);
        const auto ids = [&] (int i) { return engine.session.getState()->tracks[(size_t) i].id; };
        mixer.findFader (ids (1))->setValue (-4.0, juce::sendNotificationSync);
        mixer.findMute (ids (0))->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        auto st = engine.session.getState();
        check (std::abs (st->tracks[0].gainDb - 2.0f) < 1.0e-4f && std::abs (st->tracks[3].gainDb + 56.0f) < 1.0e-4f
                   && st->tracks[1].mute && st->tracks[3].mute && ! st->tracks[2].mute
                   && juce::exactlyEqual ((float) mixer.findFader (ids (0))->getValue(), st->tracks[0].gainDb),
               "groups: a mixer fader and mute button move the whole group, and its strips show it ("
                   + juce::String (st->tracks[0].gainDb, 3) + " dB)");

        MultitrackView view (engine);
        view.setTrackGroup (ids (2), 1);
        const auto grouped = engine.session.getState()->tracks[2].group == 1 && engine.session.getUndoDescription() == "Group 1";
        view.setTrackGroup (ids (2), 0);
        check (grouped && engine.session.getState()->tracks[2].group == 0 && engine.session.getUndoDescription() == "Ungroup Track",
               "groups: putting a track in a group and taking it out are undo steps");

        auto saved = s;
        saved.tracks[2].group = SessionTrack::maxGroups;
        const auto file = juce::File::createTempFile (".sbsession");
        const auto error = SessionFile::save (saved, file);
        const auto loaded = SessionFile::load (file, engine.formatManager);
        file.deleteFile();
        check (error.isEmpty() && loaded.state.tracks.size() == 4 && loaded.state.tracks[0].group == 1 && loaded.state.tracks[2].group == 8,
               "groups: session files keep each track's group");

        engine.session.reset ({}, {});
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
    }
}

namespace
{
    void testColours (AudioEngine& engine)
    {
        SessionState s;
        s.sampleRate = rate;

        for (int i = 0; i < 8; ++i)
            s.addTrack();

        s.tracks[7].isBus = true;
        const auto automatic = TrackColours::of (s.tracks[0], 0) == juce::Colour (TrackColours::palette[0].argb)
                                && TrackColours::of (s.tracks[6], 6) == TrackColours::of (s.tracks[0], 0)
                                && TrackColours::of (s.tracks[7], 7) == juce::Colour (0xff9aa3ad);
        s.tracks[7].colour = 7;
        check (automatic && TrackColours::of (s.tracks[7], 7) == juce::Colour (TrackColours::palette[7].argb),
               "colours: tracks take the palette by their place and buses are grey, until one is chosen");

        engine.session.reset (s, {});
        MultitrackView view (engine);
        const auto id = engine.session.getState()->tracks[1].id;
        view.setTrackColour (id, 5);
        const auto chosen = engine.session.getState()->tracks[1].colour == 5 && engine.session.getUndoDescription() == "Track Colour";
        view.setTrackColour (id, 99);
        check (chosen && engine.session.getState()->tracks[1].colour == -1,
               "colours: choosing a track's colour is an undo step; one outside the palette means automatic");

        const auto file = juce::File::createTempFile (".sbsession");
        const auto error = SessionFile::save (s, file);
        const auto loaded = SessionFile::load (file, engine.formatManager);
        file.deleteFile();
        check (error.isEmpty() && loaded.state.tracks.size() == 8 && loaded.state.tracks[7].colour == 7 && loaded.state.tracks[0].colour == -1,
               "colours: session files keep a chosen colour and leave the rest automatic");

        engine.session.reset ({}, {});
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
    }
}

namespace
{
    void testNotes (AudioEngine& engine)
    {
        SessionState s;
        s.sampleRate = rate;
        s.addTrack().name = "Vox";
        const auto id = SessionEdits::addClip (s, 0, juce::File ("/takes/vox.wav"), constant (1, 48000, 0.5f), 0);
        engine.session.reset (s, {});
        ClipProperties properties (engine, [id] { return id; });
        properties.setSize (340, 280);
        const auto settle = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (30); };
        const auto type = [&] (ClipProperties::Field f, const juce::String& value)
        {
            properties.getEditor (f).setText (value, false);
            properties.apply (f);
            settle();
        };
        const auto clip = [&] { return engine.session.getState()->tracks[0].clips[0]; };
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);

        type (ClipProperties::Field::note, "  breath at 0:01 ");
        const auto noted = clip().note == "breath at 0:01" && engine.session.getUndoDescription() == "Clip Note";
        auto locked = *engine.session.getState();
        SessionEdits::setClipLocked (locked, id, true);
        engine.session.apply (std::move (locked), "Lock Clip");
        settle();
        type (ClipProperties::Field::note, "");
        type (ClipProperties::Field::start, "0.5");
        check (noted && clip().note.isEmpty() && clip().position == 0 && properties.getEditor (ClipProperties::Field::start).getText() == "0.000",
               "notes: typed into Clip Properties and cleared as undo steps, on a locked clip too, whose start stays put");

        MultitrackView view (engine);
        const auto trackId = engine.session.getState()->tracks[0].id;
        view.setTrackNote (trackId, " double the chorus ");
        settle();
        auto tooltip = juce::String();

        for (auto* area : view.getChildren())
            for (auto* header : area->getChildren())
                for (auto* control : header->getChildren())
                    if (auto* label = dynamic_cast<juce::Label*> (control); label != nullptr && label->getText() == "Vox")
                        tooltip = label->getTooltip();

        const auto steps = engine.session.getUndoDescription();
        const auto trimmed = engine.session.getState()->tracks[0].note == "double the chorus";
        view.setTrackNote (trackId, "double the chorus");
        engine.session.undo();   // the Track Note step: the same note again added none
        settle();
        const auto undone = engine.session.getState()->tracks[0].note.isEmpty();
        engine.session.redo();
        settle();
        check (trimmed && steps == "Track Note" && tooltip == "double the chorus" && undone,
               "notes: a track's note as an undo step, shown as its name's tooltip; the same note again changes nothing");

        const auto folder = juce::File::createTempFile ("notes");
        folder.createDirectory();
        const auto wav = folder.getChildFile ("vox.wav");
        auto onDisk = *engine.session.getState();
        onDisk.tracks[0].clips[0].source = wav;
        onDisk.tracks[0].clips[0].note = "comp from take 2 & 3 <keep>";
        const auto file = folder.getChildFile ("notes.sbsession");
        const auto wrote = AudioFileIO::save (*constant (1, 48000, 0.5f), rate, wav, 24, engine.formatManager).isEmpty()
                           && SessionFile::save (onDisk, file).isEmpty();
        const auto loaded = SessionFile::load (file, engine.formatManager);
        folder.deleteRecursively();
        check (wrote && loaded.state.tracks.size() == 1 && loaded.state.tracks[0].note == "double the chorus"
                   && loaded.state.tracks[0].clips.size() == 1 && loaded.state.tracks[0].clips[0].note == "comp from take 2 & 3 <keep>",
               "notes: session files keep track and clip notes, markup characters included");

        engine.session.reset ({}, {});
        settle();
    }
}

namespace
{
    void testUndoHistory (AudioEngine& engine)
    {
        SessionState s;
        s.sampleRate = rate;
        engine.session.reset (s, {});
        engine.session.markSaved ({});
        UndoHistory history (engine.session);
        history.setSize (280, 400);
        const auto settle = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (30); };

        for (auto stepName : { "Add Track", "Rename Track", "Add Bus" })
        {
            auto next = *engine.session.getState();

            if (juce::String (stepName) == "Rename Track")
                next.tracks[0].name = "Drums";
            else if (juce::String (stepName) == "Add Track")
                next.addTrack();
            else
                next.addBus();

            engine.session.apply (std::move (next), stepName);
        }

        settle();
        const auto listed = history.getNumRows() == 4 && history.getText (0) == "Start" && history.getText (1) == "Add Track"
                            && history.getText (3) == "Add Bus" && history.getHighlightedRow() == 3;

        history.chooseRow (1);
        settle();
        const auto back = engine.session.getState()->tracks.size() == 1 && engine.session.getState()->tracks[0].name != "Drums"
                          && history.getHighlightedRow() == 1 && engine.session.getRedoDescription() == "Rename Track" && history.getNumRows() == 4;
        history.chooseRow (3);
        settle();
        const auto forward = engine.session.getState()->tracks.size() == 2 && engine.session.getState()->tracks[0].name == "Drums";
        history.chooseRow (0);
        settle();
        const auto clean = engine.session.getState()->tracks.empty() && ! engine.session.isDirty();
        history.chooseRow (9);

        auto branch = *engine.session.getState();
        branch.addTrack();
        engine.session.apply (std::move (branch), "Add Track");
        settle();
        check (listed && back && forward && clean && history.getNumRows() == 2 && history.getHighlightedRow() == 1,
               "undo history: every step listed from the start, clicking one goes back or forward to it (back to the saved one is clean), "
               "and a new step drops the ones that could be redone");

        engine.session.reset ({}, {});
        settle();
    }
}

namespace
{
    void testCollect (AudioEngine& engine)
    {
        const auto root = juce::File::createTempFile ("collect");
        root.createDirectory();
        const auto a = root.getChildFile ("one/take.wav"), b = root.getChildFile ("two/Take.wav"), c = root.getChildFile ("two/pass.wav");
        auto wrote = true;

        for (auto& [f, level] : { std::pair (a, 0.25f), std::pair (b, 0.5f), std::pair (c, 0.75f) })
        {
            f.getParentDirectory().createDirectory();
            wrote = wrote && AudioFileIO::save (*constant (1, 4800, level), rate, f, 24, engine.formatManager).isEmpty();
        }

        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        s.addTrack();
        SessionEdits::addClip (s, 0, a, constant (1, 4800, 0.25f), 0);
        SessionEdits::addClip (s, 0, a, constant (1, 4800, 0.25f), 9600);   // the same file twice
        SessionEdits::addClip (s, 1, b, constant (1, 4800, 0.5f), 0);
        SessionEdits::addClip (s, 1, root.getChildFile ("gone.wav"), constant (1, 4800, 0.1f), 9600);
        auto& withTakes = s.tracks[1].clips[0];
        withTakes.takes = { { b, withTakes.audio, 0 }, { c, constant (1, 4800, 0.75f), 0 } };

        const auto copyFile = root.getChildFile ("Copy/Copy.sbsession");
        copyFile.getParentDirectory().createDirectory();
        juce::StringArray missing;
        const auto error = SessionCollect::save (s, copyFile, {}, missing);
        const auto audio = copyFile.getParentDirectory().getChildFile ("Audio");
        const auto names = [&]
        {
            juce::StringArray found;

            for (auto& f : audio.findChildFiles (juce::File::findFiles, false))
                found.add (f.getFileName());

            found.sort (true);
            return found;
        }();

        // Moved away from the originals, the copy still opens with all its audio.
        const auto moved = root.getChildFile ("Moved");
        copyFile.getParentDirectory().moveFileTo (moved);
        const auto loaded = SessionFile::load (moved.getChildFile ("Copy.sbsession"), engine.formatManager);
        const auto& lt = loaded.state.tracks;
        const auto sameBytes = moved.getChildFile ("Audio/Take 2.wav").hasIdenticalContentTo (b) && moved.getChildFile ("Audio/take.wav").hasIdenticalContentTo (a);
        check (wrote && error.isEmpty() && names == juce::StringArray { "pass.wav", "Take 2.wav", "take.wav" } && missing == juce::StringArray { root.getChildFile ("gone.wav").getFullPathName() }
                   && sameBytes && lt.size() == 2 && lt[0].clips.size() == 2 && lt[0].clips[0].source == moved.getChildFile ("Audio/take.wav")
                   && lt[1].clips.size() >= 1 && lt[1].clips[0].takes.size() == 2 && lt[1].clips[0].takes[1].source == moved.getChildFile ("Audio/pass.wav")
                   && a.existsAsFile() && b.existsAsFile(),
               "save with audio: each file used copied once beside the copy (a second of the same name numbered), takes too, a missing one named, "
               "and the copy opening with all its audio after being moved");
        root.deleteRecursively();
    }
}

namespace
{
    void testClipList (AudioEngine& engine)
    {
        SessionState s;
        s.sampleRate = rate;
        s.addTrack().name = "Drums";
        s.addTrack().name = "Bass";
        const auto late = SessionEdits::addClip (s, 1, juce::File ("/takes/bass.wav"), constant (1, 48000, 0.5f), 96000);
        const auto first = SessionEdits::addClip (s, 0, juce::File ("/takes/kick.wav"), constant (1, 24000, 0.5f), 48000);
        const auto beside = SessionEdits::addClip (s, 1, juce::File ("/takes/riff.wav"), constant (1, 12000, 0.5f), 48000);
        SessionEdits::setClipLocked (s, beside, true);
        SessionEdits::setClipMuted (s, beside, true);
        SessionEdits::addClip (s, 1, juce::File ("/takes/far.wav"), constant (1, 4800, 0.5f), 48000 * 600);
        engine.session.reset (s, {});

        MultitrackView view (engine);
        view.setSize (MultitrackView::headerWidth + 800, MultitrackView::rulerHeight + 3 * MultitrackView::trackHeight);
        view.zoomToRange ({ 0, 48000 * 10 });
        juce::uint32 chosen = 0;
        ClipList list (engine, [&view] { return view.getSelectedClip(); }, [&] (juce::uint32 id) { chosen = id; view.showClip (id); });
        list.setSize (700, 300);
        const auto settle = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (30); };

        using C = ClipList::Column;
        check (list.getNumRows() == 4 && list.getClip (0) == first && list.getClip (1) == beside && list.getClip (2) == late
                   && list.getText (0, C::track) == "Drums" && list.getText (0, C::name) == "kick" && list.getText (0, C::start) == "1.000"
                   && list.getText (0, C::length) == "0.500" && list.getText (0, C::file) == "kick.wav" && list.getText (1, C::state) == "Locked, Muted"
                   && list.getText (2, C::state).isEmpty() && list.getHighlightedRow() < 0,
               "clip list: every clip in time order (then by track), with its track, name, start, length, file and state");

        list.chooseRow (2);
        const auto shown = chosen == late && view.getSelectedClip() == late && view.getSelectedClips().size() == 1
                           && view.getSelectedTrack() == 1 && view.getCursor() == 96000 && list.getHighlightedRow() == 2;
        list.chooseRow (3);
        const auto far = view.getSelectedClips().size() == 1 && view.getVisibleFrames().contains (48000.0 * 600)
                         && list.getHighlightedRow() == 3;
        check (shown && far, "clip list: clicking a row selects that clip alone, puts the cursor at its start and scrolls it into view");

        auto renamed = *engine.session.getState();
        renamed.tracks[0].clips[0].name = "kick 2";
        engine.session.apply (std::move (renamed), "Rename Clip");
        settle();
        const auto followed = list.getText (0, C::name) == "kick 2";
        engine.session.undo();
        settle();
        auto gone = *engine.session.getState();
        SessionEdits::removeClip (gone, late);
        engine.session.apply (std::move (gone), "Delete Clip");
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        check (followed && list.getNumRows() == 3 && list.getText (0, C::name) == "kick" && list.getHighlightedRow() == 2,
               "clip list: follows edits and undo, and a deleted clip's row goes, the highlight staying on the selected clip");

        // Searching: names, files and track names, ignoring case; edits re-filter.
        list.setSearch ("RIFF");
        const auto byName = list.getNumRows() == 1 && list.getClip (0) == beside;
        list.setSearch ("bass");
        const auto byTrack = list.getNumRows() == 2 && list.getText (0, C::track) == "Bass" && list.getText (1, C::file) == "far.wav";
        list.setSearch (".WAV ");
        const auto byFile = list.getNumRows() == 3;
        list.setSearch ("zzz");
        const auto none = list.getNumRows() == 0;
        list.setSearch ("kick");
        const auto kickBefore = list.getNumRows() == 1;
        auto boom = *engine.session.getState();
        boom.tracks[0].clips[0].name = "boom";
        boom.tracks[0].clips[0].source = juce::File ("/takes/boom.wav");
        engine.session.apply (std::move (boom), "Rename Clip");
        settle();
        const auto refiltered = list.getNumRows() == 0;
        list.setSearch ({});
        check (byName && byTrack && byFile && none && kickBefore && refiltered && list.getNumRows() == 3 && list.getSearch().isEmpty(),
               "clip list: the search shows clips whose name, file or track holds the text, ignoring case and spaces around it, and follows edits");

        // Sorting by a header: each column in order (by value, by session track order, by name),
        // backwards on request, and back to time order.
        const auto column = [&list] (int which)
        {
            juce::StringArray values;

            for (int row = 0; row < list.getNumRows(); ++row)
                values.add (list.getText (row, which));

            return values;
        };

        const auto ordered = [] (const juce::StringArray& values, bool numeric, bool forwards)
        {
            for (int i = 1; i < values.size(); ++i)
            {
                const auto order = numeric ? (values[i - 1].getDoubleValue() < values[i].getDoubleValue() ? -1 : values[i - 1].getDoubleValue() > values[i].getDoubleValue() ? 1 : 0)
                                           : values[i - 1].compareNatural (values[i]);

                if (forwards ? order > 0 : order < 0)
                    return false;
            }

            return true;
        };

        list.setSort (C::length, false);
        const auto longestFirst = ordered (column (C::length), true, false);
        const auto sorted = engine.session.getState();
        const auto where = sorted->findClip (list.getClip (0));
        const auto rowMapsClip = where && list.getText (0, C::name) == sorted->tracks[(size_t) where->first].clips[(size_t) where->second].name;
        list.setSort (C::name, true);
        const auto byNameSorted = ordered (column (C::name), false, true);
        list.setSort (C::track, true);
        const auto tracks = column (C::track);
        const auto trackOrder = tracks.indexOf ("Bass") < 0 || tracks.indexOf ("Drums") < 0 || tracks.indexOf ("Drums") < tracks.indexOf ("Bass");
        list.setSort (0, true);
        check (list.getNumRows() == 3 && longestFirst && rowMapsClip && byNameSorted && trackOrder && ordered (column (C::start), true, true),
               "clip list: sorted by length backwards, by name and by track order, each row still its clip, and back to time order");

        const auto allCounted = list.getSummary() == "3 clips, 1 locked, 1 muted; 0.850 s in all";
        list.setSearch ("riff");
        const auto oneCounted = list.getSummary() == "3 clips, 1 locked, 1 muted; 1 shown, 0.250 s in all";
        list.setSearch ({});
        check (allCounted && oneCounted, "clip list: the count line, with locked and muted clips and the shown clips' length (" + list.getSummary() + ")");

        engine.session.reset ({}, {});
        settle();
    }
}

namespace
{
    void testClipProperties (AudioEngine& engine)
    {
        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        const auto id = SessionEdits::addClip (s, 0, juce::File ("/takes/vocal.wav"), constant (1, 96000, 0.5f), 48000);
        engine.session.reset (s, {});
        juce::uint32 selected = 0;
        ClipProperties properties (engine, [&selected] { return selected; });
        properties.setSize (340, 250);
        const auto settle = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (30); };
        using Field = ClipProperties::Field;
        const auto text = [&] (Field f) { return properties.getEditor (f).getText(); };
        const auto type = [&] (Field f, const juce::String& value)
        {
            properties.getEditor (f).setText (value, false);
            properties.apply (f);
            settle();
        };
        const auto clip = [&] { return engine.session.getState()->tracks[0].clips[0]; };

        const auto empty = properties.getShownClip() == 0 && text (Field::name).isEmpty() && ! properties.getEditor (Field::start).isEnabled();
        selected = id;
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);   // its timer notices the selection
        check (empty && properties.getShownClip() == id && text (Field::name) == "vocal" && text (Field::start) == "1.000"
                   && text (Field::length) == "2.000" && text (Field::gain) == "0.0" && text (Field::fadeIn) == "0.000",
               "clip properties: nothing without a selected clip, then the selected clip's settings in seconds and dB");

        type (Field::start, "2.5");
        const auto moved = clip().position == 120000 && engine.session.getUndoDescription() == "Move Clip";
        type (Field::length, "1.25");
        const auto trimmed = clip().length == 60000 && clip().position == 120000 && engine.session.getUndoDescription() == "Trim Clip";
        type (Field::gain, "-4.5");
        type (Field::fadeIn, "0.1");
        type (Field::fadeOut, "0.2");
        type (Field::name, "Lead vocal");
        const auto c = clip();
        check (moved && trimmed && juce::exactlyEqual (c.gainDb, -4.5f) && c.fadeIn == 4800 && c.fadeOut == 9600 && c.name == "Lead vocal"
                   && engine.session.getUndoDescription() == "Rename Clip",
               "clip properties: typing a start, length, gain, fades and name edits the clip, each as an undo step");

        type (Field::length, "nonsense");
        const auto refused = text (Field::length) == "1.250";
        type (Field::start, "-3");
        engine.session.undo();
        settle();
        check (refused && clip().position == 120000 && text (Field::name) == "vocal",
               "clip properties: text that isn't a setting shows the clip's again, and undo shows here at once");

        engine.session.reset ({}, {});
        settle();
        check (properties.getShownClip() == 0, "clip properties: a clip that goes away is no longer shown");
    }
}

namespace
{
    void testMarkerList (AudioEngine& engine)
    {
        // Markers as CSV: a quoted name with a comma, one with quotes, a region's end and length.
        const Markers listed { { 0, 0, "Say \"hi\"" }, { 48000, 0, "Intro" }, { 96000, 72000, "Verse, 1" } };
        check (MarkerEdits::toCsv (listed, rate)
                   == "Name,Start (s),End (s),Length (s),Start (frames),Length (frames)\r\n"
                      "\"Say \"\"hi\"\"\",0.000000,,,0,0\r\n"
                      "Intro,1.000000,,,48000,0\r\n"
                      "\"Verse, 1\",2.000000,3.500000,1.500000,96000,72000\r\n",
               "markers as CSV: a header, seconds and frames, regions with their end and length, names quoted where needed");

        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        s.markers = listed;
        engine.session.reset (s, {});
        MultitrackView view (engine);
        view.setSize (1200, 400);
        view.zoomToRange ({ 0, 48000 });
        std::vector<int> chosen;
        MarkerTable table (engine.session, { [&] (int i) { chosen.push_back (i); view.showMarker (i); }, {}, {} });
        table.setSize (470, 320);
        const auto settle = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (30); };
        settle();

        const auto texts = table.getNumRows() == 3 && table.getText (0, MarkerTable::name) == "Say \"hi\"" && table.getText (1, MarkerTable::start) == "1.000"
                           && table.getText (1, MarkerTable::end).isEmpty() && table.getText (2, MarkerTable::end) == "3.500"
                           && table.getText (2, MarkerTable::length) == "1.500" && table.getText (5, MarkerTable::name).isEmpty();
        table.chooseRow (2);
        const auto region = view.getTimeSelection() == juce::Range<juce::int64> (96000, 168000) && view.getCursor() == 96000
                            && view.getVisibleFrames().contains (96.0e3);
        table.chooseRow (1);
        table.chooseRow (7);
        check (texts && region && chosen == std::vector<int> { 2, 1 } && view.getTimeSelection().isEmpty() && view.getCursor() == 48000,
               "marker list: name, start, end and length; choosing a region selects it and scrolls to it, a point moves the cursor");

        const auto deleted = view.deleteMarker (1);
        settle();
        check (deleted && engine.session.getUndoDescription() == "Delete Intro" && table.getNumRows() == 2 && table.getText (1, MarkerTable::name) == "Verse, 1"
                   && ! view.deleteMarker (5) && ! view.showMarker (-1),
               "marker list: deleting a marker is an undo step and the list follows; a marker that isn't there does nothing");

        // Searching names, ignoring case: rows map to the markers they show, and edits re-filter.
        chosen.clear();
        table.setSearch ("VERSE");
        const auto found = table.getNumRows() == 1 && table.getText (0, MarkerTable::name) == "Verse, 1" && table.getMarkerIndex (0) == 1
                           && table.getMarkerIndex (1) == -1;
        table.chooseRow (0);
        const auto mapped = chosen == std::vector<int> { 1 } && view.getTimeSelection() == juce::Range<juce::int64> (96000, 168000);
        auto more = *engine.session.getState();
        more.markers[0].name = "Pre-verse";
        engine.session.apply (std::move (more), "Rename Say \"hi\"");
        settle();
        const auto refiltered = table.getNumRows() == 2 && table.getMarkerIndex (0) == 0 && table.getText (0, MarkerTable::name) == "Pre-verse";
        table.setSearch ("chorus");
        const auto none = table.getNumRows() == 0 && table.getText (0, MarkerTable::name).isEmpty();
        table.setSearch ({});
        check (found && mapped && refiltered && none && table.getNumRows() == 2,
               "marker list: the search shows markers whose name holds the text, ignoring case; a row chooses the marker it shows; edits re-filter");

        // Sorting: by name ignoring case with numbers in order, by length backwards, then back to time.
        auto sortable = *engine.session.getState();
        sortable.markers = { { 0, 0, "take 10" }, { 100, 50, "Take 2" }, { 200, 10, "intro" }, { 300, 0, "Take 1" } };
        engine.session.apply (std::move (sortable), "Markers");
        settle();
        const auto names = [&table]
        {
            juce::StringArray shown;

            for (int row = 0; row < table.getNumRows(); ++row)
                shown.add (table.getText (row, MarkerTable::name));

            return shown.joinIntoString ("|");
        };

        table.setSort (MarkerTable::name, true);
        const auto byName = names();
        table.setSort (MarkerTable::length, false);
        const auto byLength = names();
        const auto lengthMapped = table.getMarkerIndex (0) == 1;
        table.setSort (0, true);
        const auto byTime = names();
        check (byName == "intro|Take 1|Take 2|take 10" && byLength == "Take 2|intro|take 10|Take 1" && lengthMapped
                   && byTime == "take 10|Take 2|intro|Take 1",
               "marker list: sorted by name ignoring case with numbers in order, by length backwards, and back to time order (" + byName + "; " + byLength + ")");

        // The count line and the buttons: two of four shown by a search; Delete on a highlighted row
        // asks for its marker; nothing highlighted, no button.
        std::vector<int> removed;
        MarkerTable withButtons (engine.session, { {}, {}, [&removed] (int i) { removed.push_back (i); } });
        withButtons.setSize (470, 320);
        const auto counted = withButtons.getSummary() == "2 markers, 2 regions";
        withButtons.setSearch ("take 1");
        const auto filtered = withButtons.getSummary() == "2 markers, 2 regions (2 shown)";
        withButtons.pressDelete();
        const auto noneHighlighted = removed.empty();
        withButtons.highlightRow (1);
        withButtons.pressDelete();
        withButtons.pressRename();
        check (counted && filtered && noneHighlighted && removed == std::vector<int> { withButtons.getMarkerIndex (1) } && removed[0] >= 0,
               "marker list: the count of markers and regions and how many are shown; Delete asks for the highlighted marker, nothing without one ("
                   + withButtons.getSummary() + ")");
        engine.session.reset ({}, {});
        settle();
    }
}

namespace
{
    void testRecovery (AudioEngine& engine)
    {
        const auto folder = juce::File::createTempFile ("recovery");   // not made yet: write makes it
        const auto none = ! SessionRecovery::find (folder).has_value();

        SessionState s;
        s.sampleRate = rate;
        s.addTrack().name = "Vocal";
        s.markers = { { 4800, 0, "Here" } };
        const auto original = folder.getSiblingFile ("Song.sbsession");
        const auto before = juce::Time::getCurrentTime().toMilliseconds();
        const auto error = SessionRecovery::write (s, original, [] (juce::uint32) { return std::unique_ptr<juce::XmlElement>(); }, folder);
        const auto found = SessionRecovery::find (folder);
        const auto loaded = found.has_value() ? SessionFile::load (found->session, engine.formatManager) : SessionFile::Loaded {};
        check (none && error.isEmpty() && found.has_value() && found->original == original && found->written.toMilliseconds() >= before
                   && found->written.toMilliseconds() <= juce::Time::getCurrentTime().toMilliseconds()
                   && loaded.error.isEmpty() && loaded.state.tracks.size() == 1 && loaded.state.tracks[0].name == "Vocal" && loaded.state.markers == s.markers,
               "recovery: a copy is written to its folder with the session's own file and the time, and opens as the session");

        const auto untitled = SessionRecovery::write (s, {}, {}, folder).isEmpty() && SessionRecovery::find (folder).has_value()
                              && SessionRecovery::find (folder)->original == juce::File();
        SessionRecovery::clear (folder);
        const auto cleared = ! SessionRecovery::find (folder).has_value();
        folder.deleteRecursively();

        Session session;
        session.reset (s, original);
        const auto cleanAtFirst = ! session.isDirty();
        session.markUnsaved();
        check (untitled && cleared && cleanAtFirst && session.isDirty() && session.getFile() == original,
               "recovery: an unsaved session's copy names no file; clearing removes it; a recovered session starts with changes to save");
    }
}

namespace
{
    void testFadeShapeFile (AudioEngine& engine)
    {
        const auto folder = juce::File::createTempFile ("shapes");
        folder.createDirectory();
        juce::AudioBuffer<float> b (1, 4800);
        b.clear();
        const auto audioFile = folder.getChildFile ("tone.wav");
        const auto written = AudioFileIO::save (*AudioEdits::fromBuffer (std::move (b)), rate, audioFile, 24, engine.formatManager).isEmpty();

        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        const auto loadedAudio = AudioFileIO::load (audioFile, engine.formatManager);
        const auto a = SessionEdits::addClip (s, 0, audioFile, loadedAudio.snapshot, 0);
        const auto b2 = SessionEdits::addClip (s, 0, audioFile, loadedAudio.snapshot, 9600);
        SessionEdits::setClipFades (s, a, 480, 960);
        SessionEdits::setClipFadeShape (s, a, false, FadeShape::equalPower);
        SessionEdits::setClipFadeShape (s, a, true, FadeShape::fast);
        const auto sessionFile = folder.getChildFile ("Shapes.sbsession");
        const auto error = SessionFile::save (s, sessionFile);
        const auto text = sessionFile.loadFileAsString();
        const auto loaded = SessionFile::load (sessionFile, engine.formatManager);
        folder.deleteRecursively();
        const auto clipOf = [&loaded] (juce::uint32 index) -> const SessionClip& { return loaded.state.tracks[0].clips[index]; };

        check (written && error.isEmpty() && loaded.error.isEmpty() && loaded.state.tracks.size() == 1 && loaded.state.tracks[0].clips.size() == 2
                   && clipOf (0).fadeInShape == FadeShape::equalPower && clipOf (0).fadeOutShape == FadeShape::fast
                   && clipOf (1).fadeInShape == FadeShape::sCurve && clipOf (1).fadeOutShape == FadeShape::sCurve
                   && text.contains ("fadeInShape=\"equalPower\"") && text.contains ("fadeOutShape=\"fast\"") && b2 != 0
                   && text.indexOf ("fadeInShape") == text.lastIndexOf ("fadeInShape"),
               "fade shapes: kept in a session file, and left out of it for the S-curve");
    }
}

namespace
{
    void testFrozenFile (AudioEngine& engine)
    {
        const auto folder = juce::File::createTempFile ("frozen");
        folder.createDirectory();
        const auto take = folder.getChildFile ("take.wav"), frozenFile = folder.getChildFile ("Elsewhere/Guitar Frozen.wav");
        frozenFile.getParentDirectory().createDirectory();
        const auto written = AudioFileIO::save (*constant (1, 4800, 0.25f), rate, take, 24, engine.formatManager).isEmpty()
                             && AudioFileIO::save (*constant (2, 9600, 0.5f), rate, frozenFile, 32, engine.formatManager).isEmpty();

        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        s.addTrack();
        SessionEdits::addClip (s, 0, take, constant (1, 4800, 0.25f), 0);
        SessionEdits::addClip (s, 1, take, constant (1, 4800, 0.25f), 0);
        SessionEdits::freezeTrack (s, s.tracks[0].id, frozenFile, constant (2, 9600, 0.5f));
        const auto sessionFile = folder.getChildFile ("Frozen.sbsession");
        const auto error = SessionFile::save (s, sessionFile);
        const auto loaded = SessionFile::load (sessionFile, engine.formatManager);
        const auto& lt = loaded.state.tracks;
        const auto roundTrip = lt.size() == 2 && lt[0].isFrozen() && lt[0].frozenFile == frozenFile && lt[0].frozenAudio->getNumFrames() == 9600
                               && lt[0].frozenAudio->getNumChannels() == 2 && lt[0].clips.size() == 1 && ! lt[1].isFrozen();

        // Collecting copies the frozen audio too; without it, the track opens unfrozen.
        const auto plan = SessionCollect::plan (s, folder.getChildFile ("Audio"));
        const auto collected = plan.copies.size() == 2 && plan.state.tracks[0].frozenFile == folder.getChildFile ("Audio/Guitar Frozen.wav");
        frozenFile.deleteFile();
        const auto withoutIt = SessionFile::load (sessionFile, engine.formatManager);
        folder.deleteRecursively();

        check (written && error.isEmpty() && loaded.error.isEmpty() && loaded.missing.isEmpty() && roundTrip && collected
                   && withoutIt.state.tracks.size() == 2 && ! withoutIt.state.tracks[0].isFrozen() && withoutIt.state.tracks[0].clips.size() == 1
                   && withoutIt.missing == juce::StringArray { frozenFile.getFullPathName() },
               "freeze: a frozen track's audio kept in a session file and collected with it; missing, the track opens unfrozen and the file is named");
    }

    void testClipGroups (AudioEngine& engine)
    {
        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        s.addTrack();
        const auto a = SessionEdits::addClip (s, 0, juce::File ("/takes/a.wav"), constant (1, 4800, 0.1f), 0);
        const auto b = SessionEdits::addClip (s, 0, juce::File ("/takes/b.wav"), constant (1, 4800, 0.1f), 9600);
        const auto c = SessionEdits::addClip (s, 1, juce::File ("/takes/c.wav"), constant (1, 4800, 0.1f), 0);
        const auto d = SessionEdits::addClip (s, 1, juce::File ("/takes/d.wav"), constant (1, 4800, 0.1f), 9600);
        SessionEdits::setClipLocked (s, d, true);
        const auto tooFew = SessionEdits::groupClips (s, { a, 999 }) == 0;
        const auto first = SessionEdits::groupClips (s, { a, c });
        const auto second = SessionEdits::groupClips (s, { b, d });
        const auto sorted = [] (std::vector<juce::uint32> v) { std::sort (v.begin(), v.end()); return v; };
        const auto groups = first != 0 && second != 0 && first != second && sorted (SessionEdits::clipGroup (s, c)) == sorted ({ a, c })
                            && SessionEdits::clipGroup (s, d).size() == 2 && SessionEdits::clipGroup (s, 999).empty();

        // Copies of a grouped clip don't join its group; split parts stay in it.
        auto copies = s;
        const auto pasted = SessionEdits::placeClip (copies, 0, copies.tracks[0].clips[0], 30000);
        SessionEdits::splitClip (copies, a, 2400);
        const auto copiesLeave = SessionEdits::clipGroup (copies, pasted) == std::vector<juce::uint32> { pasted }
                                 && SessionEdits::clipGroup (copies, a).size() == 3;

        // Regrouping a clip takes it from its old group; ungrouping breaks up the whole group.
        const auto third = SessionEdits::groupClips (s, { a, b });
        const auto regrouped = SessionEdits::clipGroup (s, c) == std::vector<juce::uint32> { c } && SessionEdits::clipGroup (s, d) == std::vector<juce::uint32> { d }
                               && sorted (SessionEdits::clipGroup (s, a)) == sorted ({ a, b }) && third != 0;
        auto ungrouped = s;
        const auto broke = SessionEdits::ungroupClips (ungrouped, { a }) && SessionEdits::clipGroup (ungrouped, b) == std::vector<juce::uint32> { b }
                           && ! SessionEdits::ungroupClips (ungrouped, { a, b });

        const auto folder = juce::File::createTempFile ("groups");
        folder.createDirectory();
        const auto take = folder.getChildFile ("take.wav");
        const auto written = AudioFileIO::save (*constant (1, 4800, 0.1f), rate, take, 24, engine.formatManager).isEmpty();

        for (auto& t : s.tracks)
            for (auto& clip : t.clips)
                clip.source = take;

        const auto sessionFile = folder.getChildFile ("Groups.sbsession");
        const auto error = SessionFile::save (s, sessionFile);
        const auto loaded = SessionFile::load (sessionFile, engine.formatManager);
        folder.deleteRecursively();
        const auto& lt = loaded.state.tracks;
        const auto kept = lt.size() == 2 && lt[0].clips.size() == 2 && lt[1].clips.size() == 2
                          && lt[0].clips[0].groupId != 0 && lt[0].clips[0].groupId == lt[0].clips[1].groupId
                          && lt[1].clips[0].groupId != 0 && lt[1].clips[1].groupId != 0 && lt[1].clips[0].groupId != lt[1].clips[1].groupId
                          && lt[1].clips[0].groupId != lt[0].clips[0].groupId && lt[1].clips[1].groupId != lt[0].clips[0].groupId
                          && SessionEdits::clipGroup (loaded.state, lt[1].clips[1].id).size() == 1
                          && lt[0].clips[0].groupId < loaded.state.nextId && lt[1].clips[1].groupId < loaded.state.nextId;

        check (tooFew && groups && copiesLeave && regrouped && broke,
               "clip groups: two or more clips, locked ones too; copies leave the group and split parts stay; regrouping and ungrouping");
        check (written && error.isEmpty() && loaded.error.isEmpty() && kept,
               "clip groups: kept in a session file, each group with a fresh id, a group left with one clip too");
    }

    void testClipGainFile (AudioEngine& engine)
    {
        const auto folder = juce::File::createTempFile ("clipgain");
        folder.createDirectory();
        const auto take = folder.getChildFile ("take.wav");
        const auto written = AudioFileIO::save (*constant (1, 4800, 0.1f), rate, take, 24, engine.formatManager).isEmpty();

        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        const auto a = SessionEdits::addClip (s, 0, take, constant (1, 4800, 0.1f), 0);
        SessionEdits::addClip (s, 0, take, constant (1, 4800, 0.1f), 9600);
        const Envelope line { { { 100, -3.5f }, { 2000, 0.0f }, { 4000, 12.0f } } };
        SessionEdits::setClipGainEnvelope (s, a, line);
        const auto sessionFile = folder.getChildFile ("Gain.sbsession");
        const auto error = SessionFile::save (s, sessionFile);
        const auto text = sessionFile.loadFileAsString();
        const auto loaded = SessionFile::load (sessionFile, engine.formatManager);
        folder.deleteRecursively();
        const auto& lc = loaded.state.tracks[0].clips;

        check (written && error.isEmpty() && loaded.error.isEmpty() && lc.size() == 2 && lc[0].gainEnvelope == line && lc[1].gainEnvelope.isEmpty()
                   && text.indexOf ("<GAIN") == text.lastIndexOf ("<GAIN"),
               "clip gain line: kept in a session file, and left out for a clip without one");
    }

    void testSessionNotes (AudioEngine& engine)
    {
        SessionState s;
        s.notes = "Verse 1: take 3";
        engine.session.reset (s, {});
        SessionNotes notes (engine.session);
        auto& editor = notes.getEditor();
        const auto shown = editor.getText() == "Verse 1: take 3";

        // Typing is one undo step, however much is typed; undo shows the notes as they were.
        editor.setText ("Verse 1: take 3\nChorus", true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        editor.setText ("Verse 1: take 3\nChorus: double it", true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        const auto typed = engine.session.getState()->notes == "Verse 1: take 3\nChorus: double it" && engine.session.getUndoDescription() == "Session Notes";
        engine.session.undo();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        const auto undone = editor.getText() == "Verse 1: take 3" && engine.session.getState()->notes == "Verse 1: take 3" && ! engine.session.canUndo();

        // Kept in a session file as written, lines and characters XML treats specially too.
        SessionState withNotes;
        withNotes.notes = juce::String::fromUTF8 ("Bridge <slow> & quiet\n\n\"Caf\xc3\xa9\" take\t2  ");
        const auto file = juce::File::createTempFile (".sbsession");
        const auto error = SessionFile::save (withNotes, file);
        const auto loaded = SessionFile::load (file, engine.formatManager);
        SessionFile::save (SessionState(), file);
        const auto none = ! file.loadFileAsString().contains ("NOTES");
        file.deleteFile();

        check (shown && typed && undone, "session notes: the window shows the session's notes, typing changes them as one undo step, and undo shows them as they were");
        check (error.isEmpty() && loaded.error.isEmpty() && loaded.state.notes == withNotes.notes && none,
               "session notes: kept in a session file exactly, and left out when there are none");
        engine.session.reset ({}, {});
    }

    void testFolderStrips (AudioEngine& engine)
    {
        // A, then a folder at -6 dB holding B at -3 dB: strip i is track i, a folder's showing its volume.
        SessionState s;
        s.sampleRate = rate;
        s.addTrack().gainDb = -1.0f;
        auto& folder = s.addFolder();
        folder.gainDb = -6.0f;
        const auto folderId = folder.id;
        auto& b = s.addTrack();
        b.gainDb = -3.0f;
        b.pan = 0.5f;
        const auto bId = b.id;
        SessionEdits::setTrackFolder (s, bId, folderId);
        SessionEdits::moveTrack (s, 2, 1);
        SessionEdits::moveTrack (s, 2, 1);   // A, folder, B
        engine.session.reset (s, {});
        MixerView mixer (engine);
        mixer.setSize (MixerView::stripWidth * 5, 400);

        const auto& t = engine.session.getState()->tracks;
        const auto order = t.size() == 3 && t[1].id == folderId && t[2].id == bId;
        const auto shows = mixer.getNumStrips() == 3 && juce::exactlyEqual ((float) mixer.findFader (folderId)->getValue(), -6.0f)
                           && juce::exactlyEqual ((float) mixer.findFader (bId)->getValue(), -3.0f) && juce::exactlyEqual (mixer.findPan (bId)->getValue(), 50.0)
                           && ! mixer.findPan (folderId)->isVisible() && mixer.findPan (bId)->isVisible();

        mixer.findFader (folderId)->setValue (-2.0, juce::sendNotificationSync);
        mixer.findMute (folderId)->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        const auto& after = engine.session.getState()->tracks;
        const auto edits = juce::exactlyEqual (after[1].gainDb, -2.0f) && after[1].mute && juce::exactlyEqual (after[2].gainDb, -3.0f) && ! after[2].mute
                           && engine.session.getState()->isMuted (after[2]);

        check (order && shows && edits,
               "mixer: a folder has a strip in its place, with its volume, mute and solo but no pan, and the strips after it show their own tracks");
        engine.session.reset ({}, {});
    }

    void testFolders (AudioEngine& engine)
    {
        // A in a folder, sending to a bus also in it; B outside.
        SessionState s;
        s.sampleRate = rate;
        s.addTrack().name = "A";
        s.addTrack().name = "B";
        const auto busId = s.addBus().id;
        const auto folderId = s.addFolder().id;
        const auto a = s.tracks[0].id, b = s.tracks[1].id;
        SessionEdits::addClip (s, 0, juce::File ("/takes/a.wav"), constant (1, 4800, 0.25f), 0);
        SessionEdits::addClip (s, 1, juce::File ("/takes/b.wav"), constant (1, 4800, 0.5f), 0);
        SessionEdits::setSend (s, a, { busId, -6.0f, false });
        const auto inFolder = SessionEdits::setTrackFolder (s, a, folderId) && SessionEdits::setTrackFolder (s, busId, folderId);
        const auto refused = ! SessionEdits::setTrackFolder (s, folderId, folderId) && ! SessionEdits::setTrackFolder (s, a, folderId)
                             && ! SessionEdits::setTrackFolder (s, a, b) && ! SessionEdits::setTrackFolder (s, a, 999)
                             && ! SessionEdits::setSend (s, folderId, { busId, 0.0f, false });
        const auto render = [] (const SessionState& state)
        {
            juce::AudioBuffer<float> out (2, 2048);
            SessionMixer::Buffers buffers;
            SessionMixer::render (state, 0, out, 0, 2048, buffers);
            return out;
        };
        const auto only = [&] (std::initializer_list<juce::uint32> playing)
        {
            auto copy = s;

            for (auto& t : copy.tracks)
                t.mute = ! t.isFolder && std::find (playing.begin(), playing.end(), t.id) == playing.end();

            return render (copy);
        };
        const auto same = [] (const juce::AudioBuffer<float>& x, const juce::AudioBuffer<float>& y)
        {
            for (int ch = 0; ch < 2; ++ch)
                if (std::memcmp (x.getReadPointer (ch), y.getReadPointer (ch), sizeof (float) * 2048) != 0)
                    return false;

            return true;
        };

        auto muted = s;
        muted.tracks[3].mute = true;
        auto soloed = s;
        soloed.tracks[3].solo = true;
        auto collapsedState = s;
        collapsedState.tracks[3].collapsed = true;
        const auto mutes = same (render (muted), only ({ b })) && muted.isMuted (muted.tracks[0]) && ! muted.isMuted (muted.tracks[1]);
        const auto solos = same (render (soloed), only ({ a, busId })) && soloed.isSoloed (soloed.tracks[2]);
        const auto collapsedPlays = same (render (collapsedState), render (s)) && ! collapsedState.isShown (collapsedState.tracks[0])
                                    && collapsedState.isShown (collapsedState.tracks[1]) && collapsedState.isShown (collapsedState.tracks[3]);

        // Its volume: as if added to each member's own, sends after the volume too; at 0 dB,
        // bit for bit as with no folder.
        auto louder = s;
        louder.tracks[3].gainDb = -6.0f;
        auto byOwn = s;
        byOwn.tracks[0].gainDb = -6.0f;
        byOwn.tracks[2].gainDb = -6.0f;
        const auto withFolderVolume = render (louder), withOwnVolumes = render (byOwn);
        auto volumeWorst = 0.0f;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 2048; ++i)
                volumeWorst = juce::jmax (volumeWorst, std::abs (withFolderVolume.getSample (ch, i) - withOwnVolumes.getSample (ch, i)));

        // Bounced, a track in a folder lands in it too, so the mix stays the same.
        auto bounced = louder;
        const auto bounceId = Bounce::land (bounced, a, juce::File ("/takes/A Bounce.wav"), AudioEdits::fromBuffer (render (Bounce::isolate (louder, a))), 0);
        const auto afterBounce = render (bounced);
        auto bounceWorst = 0.0f;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 2048; ++i)
                bounceWorst = juce::jmax (bounceWorst, std::abs (afterBounce.getSample (ch, i) - withFolderVolume.getSample (ch, i)));

        const auto bounceStays = bounced.findTrack (bounceId) != nullptr && bounced.findTrack (bounceId)->folderId == folderId && bounceWorst < 1.0e-6f;

        auto noFolder = s;
        SessionEdits::removeTrack (noFolder, 3);
        const auto volumes = volumeWorst < 1.0e-6f && withFolderVolume.getMagnitude (0, 2048) > 0.1f && same (render (s), render (noFolder))
                             && juce::exactlyEqual (s.folderGain (s.tracks[0]), 1.0f) && std::abs (louder.folderGain (louder.tracks[2]) - 0.501187f) < 1.0e-5f;

        // No clips on a folder, and removing it leaves its members out of any.
        auto edits = s;
        const auto clipOnFolder = SessionEdits::addClip (edits, 3, juce::File ("/takes/c.wav"), constant (1, 100, 0.1f), 0);
        const auto newTrack = edits.tracks.size() == 5 && edits.tracks[4].clips.size() == 1 && edits.tracks[4].clips[0].id == clipOnFolder
                              && edits.tracks[3].clips.empty() && edits.tracks[4].name == "Track 3";
        SessionEdits::removeTrack (edits, 3);
        const auto released = edits.tracks[0].folderId == 0 && edits.tracks[2].folderId == 0;

        // Saved and loaded: the folder, collapsed, and who is in it.
        collapsedState.addFolder();
        collapsedState.tracks[3].gainDb = -4.5f;
        const auto file = juce::File::createTempFile (".sbsession");
        const auto error = SessionFile::save (collapsedState, file);
        const auto loaded = SessionFile::load (file, engine.formatManager);
        file.deleteFile();
        const auto& lt = loaded.state.tracks;
        const auto kept = lt.size() == 5 && lt[3].isFolder && lt[3].collapsed && lt[3].name == "Folder 1" && lt[0].folderId == lt[3].id
                          && lt[2].folderId == lt[3].id && lt[1].folderId == 0 && lt[4].isFolder && ! lt[4].collapsed && lt[4].name == "Folder 2"
                          && lt[0].sends.size() == 1 && juce::exactlyEqual (lt[3].gainDb, -4.5f);

        check (inFolder && refused, "folders: tracks and buses go in a folder; not a folder, nor into a track, a missing folder or where it is");
        check (mutes && solos && collapsedPlays,
               "folders: a folder's mute and solo act on its tracks and buses, bit for bit as their own would, and collapsed they still play");
        check (volumes, "folders: a folder's volume acts as if added to each track's and bus's own, sends after the volume too (within "
                            + juce::String (volumeWorst, 9) + "), and at 0 dB the mix is bit for bit as with no folder");
        check (bounceStays, "folders: a bounce of a track in a folder lands in the folder, the mix staying within float rounding ("
                                + juce::String (bounceWorst, 9) + ")");
        check (newTrack && released, "folders: a clip for a folder goes on a new track, and removing a folder frees its tracks");
        check (error.isEmpty() && loaded.error.isEmpty() && kept, "folders: kept in a session file, collapsed or not, with their tracks");
    }

    /** Two commands with default keys, as the main window has many. */
    struct TwoCommands final : public juce::ApplicationCommandTarget
    {
        juce::ApplicationCommandTarget* getNextCommandTarget() override     { return nullptr; }
        void getAllCommands (juce::Array<juce::CommandID>& ids) override   { ids.addArray ({ 1, 2 }); }
        bool perform (const InvocationInfo&) override                       { return true; }

        void getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info) override
        {
            info.setInfo (id == 1 ? "Play" : "Loop", {}, "Transport", 0);
            info.addDefaultKeypress (id == 1 ? juce::KeyPress::spaceKey : 'l', id == 1 ? 0 : juce::ModifierKeys::commandModifier);
        }
    };

    void testKeyShortcuts()
    {
        const auto file = juce::File::createTempFile (".settings");
        juce::PropertiesFile::Options options;
        juce::PropertiesFile settings (file, options);
        TwoCommands target;

        juce::ApplicationCommandManager first;
        first.registerAllCommandsForTarget (&target);
        KeyShortcuts::store (*first.getKeyMappings(), settings);
        const auto nothingStored = ! settings.containsKey (KeyShortcuts::settingsKey);
        const juce::KeyPress newLoop ('k', juce::ModifierKeys::commandModifier, 0);
        first.getKeyMappings()->removeKeyPress (2, 0);
        first.getKeyMappings()->addKeyPress (2, newLoop);
        KeyShortcuts::store (*first.getKeyMappings(), settings);

        // Read back from the file by a fresh set, as the next launch does.
        juce::PropertiesFile reread (file, options);
        juce::ApplicationCommandManager second;
        second.registerAllCommandsForTarget (&target);
        const auto restored = KeyShortcuts::restore (*second.getKeyMappings(), reread);
        const auto loopKeys = second.getKeyMappings()->getKeyPressesAssignedToCommand (2);
        const auto playKeys = second.getKeyMappings()->getKeyPressesAssignedToCommand (1);

        second.getKeyMappings()->resetToDefaultMappings();
        KeyShortcuts::store (*second.getKeyMappings(), reread);
        const auto backToNone = ! reread.containsKey (KeyShortcuts::settingsKey);
        file.deleteFile();

        check (nothingStored && restored && loopKeys.size() == 1 && loopKeys[0] == newLoop && playKeys.size() == 1
                   && playKeys[0] == juce::KeyPress (juce::KeyPress::spaceKey) && backToNone,
               "keyboard shortcuts: only changes are stored, they come back over the defaults in a fresh start, and resetting stores nothing");
    }

    /** Three commands in two menus, one hidden from key editors. */
    struct ThreeCommands final : public juce::ApplicationCommandTarget
    {
        juce::ApplicationCommandTarget* getNextCommandTarget() override     { return nullptr; }
        void getAllCommands (juce::Array<juce::CommandID>& ids) override   { ids.addArray ({ 1, 2, 3, 4 }); }
        bool perform (const InvocationInfo&) override                       { return true; }

        void getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info) override
        {
            const auto cmd = juce::ModifierKeys::commandModifier;

            switch (id)
            {
                case 1: info.setInfo ("Save", "Save the file", "File", 0); info.addDefaultKeypress ('s', cmd); break;
                case 2: info.setInfo ("Play", "Start playing", "Transport", 0); info.addDefaultKeypress (juce::KeyPress::spaceKey, 0); break;
                case 3: info.setInfo ("Loop Playback", "Play over and over", "Transport", 0); info.addDefaultKeypress ('l', cmd); break;
                default: info.setInfo ("Secret", {}, "Transport", juce::ApplicationCommandInfo::hiddenFromKeyEditor); break;
            }
        }
    };

    void testShortcutEditing()
    {
        ThreeCommands target;
        juce::ApplicationCommandManager commands;
        commands.registerAllCommandsForTarget (&target);
        auto& keys = *commands.getKeyMappings();
        const auto cmd = juce::ModifierKeys::commandModifier;
        const juce::KeyPress cmdS ('s', cmd, 0), cmdL ('l', cmd, 0), cmdK ('k', cmd, 0), space (juce::KeyPress::spaceKey);

        // Listing and searching.
        const auto all = KeyShortcuts::rows (commands, {}, false);
        const auto byWords = KeyShortcuts::rows (commands, "transport OVER", false);
        const auto byKey = KeyShortcuts::rows (commands, "space", false);
        const auto listed = all.size() == 3 && all[0].name == "Save" && all[1].name == "Loop Playback" && all[2].name == "Play"
                            && byWords.size() == 1 && byWords[0].id == 3 && byKey.size() == 1 && byKey[0].id == 2
                            && KeyShortcuts::rows (commands, {}, true).empty();

        // Taking a key from another command, replacing in place, and the changed-only list.
        const auto takenFrom = KeyShortcuts::assign (keys, 3, cmdS, 0);
        const auto took = takenFrom == 1 && keys.getKeyPressesAssignedToCommand (3) == juce::Array<juce::KeyPress> { cmdS }
                          && keys.getKeyPressesAssignedToCommand (1).isEmpty();
        const auto changed = KeyShortcuts::rows (commands, {}, true);
        const auto changedListed = changed.size() == 2 && changed[0].id == 1 && changed[1].id == 3;
        const auto added = KeyShortcuts::assign (keys, 2, cmdK) == 0 && keys.getKeyPressesAssignedToCommand (2) == juce::Array<juce::KeyPress> { space, cmdK };
        const auto own = KeyShortcuts::assign (keys, 2, space, 1) == 0 && keys.getKeyPressesAssignedToCommand (2) == juce::Array<juce::KeyPress> { space };

        // Resetting Save while Loop Playback holds its key: the key stays with Loop Playback.
        const auto kept = KeyShortcuts::resetCommand (commands, 1);
        const auto resetKept = kept == juce::Array<juce::KeyPress> { cmdS } && keys.getKeyPressesAssignedToCommand (1).isEmpty();
        KeyShortcuts::resetCommand (commands, 3);
        const auto resetBoth = KeyShortcuts::resetCommand (commands, 1).isEmpty() && keys.getKeyPressesAssignedToCommand (1) == juce::Array<juce::KeyPress> { cmdS }
                               && keys.getKeyPressesAssignedToCommand (3) == juce::Array<juce::KeyPress> { cmdL };

        // A set exported and imported into a fresh start; a file that isn't one refused.
        KeyShortcuts::assign (keys, 3, cmdK);
        keys.removeKeyPress (2, 0);
        const auto file = juce::File::createTempFile (KeyShortcuts::fileSuffix), junk = juce::File::createTempFile (".txt");
        const auto exported = KeyShortcuts::exportTo (keys, file).wasOk();
        junk.replaceWithText ("<NOT_KEYS/>");

        juce::ApplicationCommandManager fresh;
        fresh.registerAllCommandsForTarget (&target);
        auto& freshKeys = *fresh.getKeyMappings();
        KeyShortcuts::assign (freshKeys, 1, cmdK);   // changed before the import, which starts from the defaults
        const auto imported = KeyShortcuts::importFrom (freshKeys, file).wasOk();
        const auto same = freshKeys.getKeyPressesAssignedToCommand (1) == juce::Array<juce::KeyPress> { cmdS }
                          && freshKeys.getKeyPressesAssignedToCommand (2).isEmpty()
                          && freshKeys.getKeyPressesAssignedToCommand (3) == juce::Array<juce::KeyPress> { cmdL, cmdK };
        const auto refused = KeyShortcuts::importFrom (freshKeys, junk).failed() && freshKeys.getKeyPressesAssignedToCommand (3).size() == 2;
        file.deleteFile();
        junk.deleteFile();

        // The window: searching narrows its list; choosing a command shows its keys.
        KeyShortcuts::Editor editor (fresh);
        editor.getSearchBox().setText ("loop", false);   // the box tells its listener later, so list again now
        editor.refresh();
        const auto narrowed = editor.getNumShown() == 1;
        editor.choose (3);

        check (listed, "shortcut editor: commands listed by menu then name, hidden ones left out, every search word matched in name, menu, "
                       "description or key");
        check (took && changedListed && added && own,
               "shortcut editor: a key given to one command is taken from the one that had it, replacing in place or added; both then listed as changed");
        check (resetKept && resetBoth, "shortcut editor: resetting a command gives back its defaults, except one another command holds now");
        check (exported && imported && same && refused && narrowed && editor.getChosen() == 3,
               "shortcut editor: a set exported and imported over a changed fresh start gives the same keys; a file that isn't a set changes nothing; "
               "the window's search narrows its list");
    }
}

namespace
{
    void testSoloSafe (AudioEngine& engine)
    {
        SessionState s;
        s.sampleRate = 48000.0;
        s.addTrack();
        s.addBus();
        s.addFolder();
        s.tracks[0].folderId = s.tracks[2].id;
        engine.session.reset (s, {});
        MultitrackView view (engine);
        const auto busId = engine.session.getState()->tracks[1].id, folderId = engine.session.getState()->tracks[2].id;

        view.setTrackSoloSafe (busId, true);
        const auto* after = engine.session.getState().get();
        const auto set = after->tracks[1].soloSafe && engine.session.getUndoDescription() == "Solo Safe" && ! after->isSoloSafe (after->tracks[0]);

        // A folder's solo safe covers what is in it.
        view.setTrackSoloSafe (folderId, true);
        after = engine.session.getState().get();
        const auto viaFolder = after->isSoloSafe (after->tracks[0]) && ! after->tracks[0].soloSafe;

        const auto file = juce::File::createTempFile (".sbsession");
        const auto error = SessionFile::save (*engine.session.getState(), file);
        const auto loaded = SessionFile::load (file, engine.formatManager);
        file.deleteFile();
        const auto filed = error.isEmpty() && loaded.state.tracks.size() == 3 && ! loaded.state.tracks[0].soloSafe
                           && loaded.state.tracks[1].soloSafe && loaded.state.tracks[2].soloSafe;

        view.setTrackSoloSafe (busId, false);
        const auto off = ! engine.session.getState()->tracks[1].soloSafe && engine.session.getUndoDescription() == "Not Solo Safe";
        engine.session.undo();
        const auto undone = engine.session.getState()->tracks[1].soloSafe;
        check (set && viaFolder && filed && off && undone,
               "solo safe: set and cleared as undo steps, through a folder for its members, and kept in session files");
    }

    /** The item with this text in a menu or its submenus, or null. */
    const juce::PopupMenu::Item* findItem (const juce::PopupMenu& menu, const juce::String& text)
    {
        for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next();)
            if (it.getItem().text == text)
                return &it.getItem();

        return nullptr;
    }

    void testStripMenu (AudioEngine& engine)
    {
        SessionState s;
        s.sampleRate = 48000.0;
        s.addTrack();
        s.addBus();
        engine.session.reset (s, {});
        MixerView mixer (engine);
        const auto busId = engine.session.getState()->tracks[1].id;
        const auto settle = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (30); };

        auto menu = mixer.getStripMenu (busId);
        const auto* safe = findItem (menu, "Solo Safe");
        const auto offered = safe != nullptr && ! safe->isTicked && findItem (menu, "Violet") != nullptr && mixer.getStripMenu (999).getNumItems() == 0;

        if (safe != nullptr)
            safe->action();

        settle();
        const auto made = engine.session.getState()->tracks[1].soloSafe && engine.session.getUndoDescription() == "Solo Safe";
        menu = mixer.getStripMenu (busId);
        safe = findItem (menu, "Solo Safe");
        const auto ticked = safe != nullptr && safe->isTicked;

        if (const auto* violet = findItem (menu, "Violet"))
            violet->action();

        settle();
        const auto coloured = engine.session.getState()->tracks[1].colour == 2 && engine.session.getUndoDescription() == "Track Colour"
                              && engine.session.getState()->tracks[0].colour == -1;
        menu = mixer.getStripMenu (busId);
        const auto* violet = findItem (menu, "Violet");
        const auto* automatic = findItem (menu, "Automatic");
        const auto colourTicked = violet != nullptr && violet->isTicked && automatic != nullptr && ! automatic->isTicked;

        if (safe = findItem (menu, "Solo Safe"); safe != nullptr)
            safe->action();

        settle();
        const auto cleared = ! engine.session.getState()->tracks[1].soloSafe && engine.session.getUndoDescription() == "Not Solo Safe";
        check (offered && made && ticked && coloured && colourTicked && cleared,
               "mixer: a strip's menu makes it solo safe and back and sets its colour, as undo steps, ticking what is set");

        // The polarity switch on a strip and in the header menu's command; kept in session files.
        mixer.findPolarity (busId)->triggerClick();
        settle();
        const auto flipped = engine.session.getState()->tracks[1].invertPolarity && engine.session.getUndoDescription() == "Invert Polarity"
                             && mixer.findPolarity (busId)->getToggleState() && mixer.findPolarity (999) == nullptr;
        MultitrackView view (engine);
        const auto trackId = engine.session.getState()->tracks[0].id;
        view.setTrackPolarityInverted (trackId, true);
        settle();
        const auto both = engine.session.getState()->tracks[0].invertPolarity && mixer.findPolarity (trackId)->getToggleState();

        const auto file = juce::File::createTempFile (".sbsession");
        const auto error = SessionFile::save (*engine.session.getState(), file);
        const auto loaded = SessionFile::load (file, engine.formatManager);
        auto text = file.loadFileAsString();
        file.deleteFile();
        const auto filed = error.isEmpty() && loaded.state.tracks.size() == 2 && loaded.state.tracks[0].invertPolarity
                           && loaded.state.tracks[1].invertPolarity && text.contains ("invert=\"1\"");

        view.setTrackPolarityInverted (trackId, false);
        const auto restored = ! engine.session.getState()->tracks[0].invertPolarity && engine.session.getUndoDescription() == "Restore Polarity";
        check (flipped && both && filed && restored,
               "polarity: the strip's switch and the header's command invert a track or bus as undo steps, shown on the strip, and kept in session files");
    }

    void testRecentFiles()
    {
        const auto folder = juce::File::createTempFile ("recent");
        folder.createDirectory();
        juce::PropertiesFile settings (folder.getChildFile ("settings.xml"), {});
        juce::RecentlyOpenedFilesList list;
        RecentFiles::restore (list, settings);
        std::vector<juce::File> files;

        for (int i = 0; i < 12; ++i)
        {
            files.push_back (folder.getChildFile ("take " + juce::String (i) + (i == 3 ? ".sbsession" : ".wav")));
            files.back().replaceWithText ("x");
            RecentFiles::note (list, files.back(), settings);
        }

        // The newest first, ten kept; opening one again moves it to the top.
        const auto capped = list.getNumFiles() == 10 && list.getFile (0) == files[11] && list.getFile (9) == files[2];
        RecentFiles::note (list, files[5], settings);
        const auto moved = list.getNumFiles() == 10 && list.getFile (0) == files[5] && list.getFile (1) == files[11];

        // Kept in the settings: a fresh list reads the same back.
        juce::RecentlyOpenedFilesList again;
        RecentFiles::restore (again, settings);
        auto same = again.getNumFiles() == list.getNumFiles();

        for (int i = 0; same && i < list.getNumFiles(); ++i)
            same = again.getFile (i) == list.getFile (i);

        // The menu leaves out files that have gone, keeping each item's id at its place in the list.
        files[11].deleteFile();
        const auto menu = RecentFiles::createMenu (list, 100);
        std::vector<std::pair<int, juce::String>> items;

        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
            if (! it.getItem().isSeparator)
                items.emplace_back (it.getItem().itemID, it.getItem().text);

        const auto listed = items.size() == 10 && items[0] == std::pair<int, juce::String> (100, "take 5.wav")
                            && items[1].first == 102 && items.back() == std::pair<int, juce::String> (110, "Clear Menu");
        const auto kinds = RecentFiles::isSession (files[3]) && ! RecentFiles::isSession (files[4]);

        RecentFiles::clear (list, settings);
        juce::RecentlyOpenedFilesList cleared;
        RecentFiles::restore (cleared, settings);
        const auto emptied = list.getNumFiles() == 0 && cleared.getNumFiles() == 0 && RecentFiles::createMenu (list, 100).getNumItems() == 0;
        folder.deleteRecursively();
        check (capped && moved && same && listed && kinds && emptied,
               "open recent: newest first, ten kept, reopening moves to the top, kept in the settings, gone files left off the menu, and cleared");
    }

    void testClipsCsv()
    {
        SessionState s;
        s.sampleRate = 48000.0;
        s.addTrack().name = "Drums, room";
        s.addTrack().name = "Bass";
        SessionEdits::addClip (s, 0, juce::File ("/takes/kick.wav"), constant (1, 24000, 0.5f), 96000);
        const auto quoted = SessionEdits::addClip (s, 1, juce::File ("/takes/riff \"A\".wav"), constant (1, 12000, 0.5f), 48000);
        SessionEdits::addClip (s, 0, juce::File ("/takes/snare.wav"), constant (1, 6000, 0.5f), 48000);
        SessionEdits::setClipLocked (s, quoted, true);
        SessionEdits::setClipMuted (s, quoted, true);

        const auto csv = ClipList::toCsv (s);
        const juce::String expected ("Track,Name,Start (s),End (s),Length (s),File,Locked,Muted,Start (frames),Length (frames)\r\n"
                                     "\"Drums, room\",snare,1.000000,1.125000,0.125000,/takes/snare.wav,,,48000,6000\r\n"
                                     "Bass,\"riff \"\"A\"\"\",1.000000,1.250000,0.250000,\"/takes/riff \"\"A\"\".wav\",yes,yes,48000,12000\r\n"
                                     "\"Drums, room\",kick,2.000000,2.500000,0.500000,/takes/kick.wav,,,96000,24000\r\n");
        check (csv == expected && ClipList::toCsv ({}).upToFirstOccurrenceOf ("\r\n", false, false).startsWith ("Track,Name"),
               "clip list as CSV: a header, clips in the list's order with seconds and frames, locked and muted, and fields quoted where needed"
                   + juce::String (csv == expected ? "" : "\n" + csv));
    }

    void testLoadReadout (AudioEngine& engine)
    {
        LoadReadout::Reading now { 0.123, 0 };
        LoadReadout readout ([&now] { return now; });
        const auto quiet = readout.getText() == "Load 12%  |  0 dropouts" && ! readout.isWarning();

        now = { 0.85, 0 };
        readout.refresh();
        const auto busy = readout.getText() == "Load 85%  |  0 dropouts" && readout.isWarning();

        now = { 0.2, 3 };
        readout.refresh();
        const auto dropped = readout.getText() == "Load 20%  |  3 dropouts" && readout.isWarning();

        readout.resetDropouts();
        const auto reset = readout.getDropouts() == 0 && ! readout.isWarning();
        now = { 0.2, 4 };
        readout.refresh();
        const auto one = readout.getText() == "Load 20%  |  1 dropout" && readout.isWarning();

        // A new device counts from 0 again, and every one of its dropouts shows.
        now = { 0.2, 2 };
        readout.refresh();
        const auto newDevice = readout.getDropouts() == 2;
        now = { 1.7, 2 };
        readout.refresh();
        const auto capped = readout.getText().startsWith ("Load 100%");

        check (quiet && busy && dropped && reset && one && newDevice && capped,
               "load readout: the load in percent, red over 80% or after dropouts; dropouts counted from a reset, and from 0 on a new device");

        // Wired to the real device manager: the default device's count, whatever it is, from 0.
        LoadReadout real ([&engine] { return LoadReadout::Reading { engine.deviceManager.getCpuUsage(), engine.deviceManager.getXRunCount() }; });
        real.resetDropouts();
        check (real.getDropouts() == 0 && real.getText().startsWith ("Load "),
               "load readout: reads the audio device manager's load and dropouts");
    }
}

void runMixerTests (AudioEngine& engine)
{
    testClipProperties (engine);
    testLevels();
    testMixerView (engine);
    testGroups (engine);
    testColours (engine);
    testClipList (engine);
    testNotes (engine);
    testUndoHistory (engine);
    testCollect (engine);
    testMarkerList (engine);
    testRecovery (engine);
    testFadeShapeFile (engine);
    testKeyShortcuts();
    testShortcutEditing();
    testFrozenFile (engine);
    testClipGroups (engine);
    testClipGainFile (engine);
    testFolders (engine);
    testFolderStrips (engine);
    testSessionNotes (engine);
    testLoadReadout (engine);
    testSoloSafe (engine);
    testStripMenu (engine);
    testRecentFiles();
    testClipsCsv();
}
