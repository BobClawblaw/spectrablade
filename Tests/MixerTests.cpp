// Levels are checked against what the mixer itself computes.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioFileIO.h"
#include "../Source/ClipList.h"
#include "../Source/ClipProperties.h"
#include "../Source/KeyShortcuts.h"
#include "../Source/MarkerTable.h"
#include "../Source/MixerView.h"
#include "../Source/MultitrackView.h"
#include "../Source/SessionCollect.h"
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
    testFrozenFile (engine);
    testClipGroups (engine);
    testClipGainFile (engine);
}
