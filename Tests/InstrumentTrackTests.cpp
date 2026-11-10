// Chains with an instrument are expected to be exact: mixes are compared bit for bit.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/Session.h"
#include "../Source/SessionMixer.h"
#include "../Source/TrackEffects.h"
#include "../Source/OfflineRender.h"
#include "../Source/BuiltInEffects.h"
#include "../Source/AudioEngine.h"
#include "TestPlugins.h"
#include "TestUtils.h"
#include "InstrumentTrackTests.h"

namespace
{
    using TestUtils::check;
    using TestPlugins::ClickInstrument;
    using TestPlugins::GainPlugin;

    constexpr double rate = 48000.0;

    /** (frame, note on, pitch) for each event, in the order the clip gives them over blocks of this size. */
    using Event = std::tuple<juce::int64, bool, int>;

    std::vector<Event> eventsOf (const NoteClip& c, juce::int64 upTo, int blockSize)
    {
        std::vector<Event> out;
        juce::MidiBuffer midi;

        for (juce::int64 from = 0; from < upTo; from += blockSize)
        {
            midi.clear();
            c.addMidi (from, (int) juce::jmin ((juce::int64) blockSize, upTo - from), midi);

            for (const auto meta : midi)
                out.emplace_back (from + meta.samplePosition, meta.getMessage().isNoteOn(), meta.getMessage().getNoteNumber());
        }

        return out;
    }

    /** A stereo clip of one value, `frames` long. */
    AudioDocument::Snapshot constant (int frames, float value)
    {
        juce::AudioBuffer<float> b (2, frames);

        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::fill (b.getWritePointer (ch), value, frames);

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, frames } };
        return std::make_shared<const AudioSnapshot> (2, std::move (segments));
    }

    juce::AudioBuffer<float> play (const SessionState& s, juce::int64 start, int frames, int blockSize, SessionMixer::Inserts* inserts)
    {
        juce::AudioBuffer<float> out (2, frames);
        SessionMixer::Buffers buffers;
        buffers.reserve (blockSize);

        if (inserts != nullptr)
            SessionMixer::prime (s, start, buffers, *inserts);

        for (int p = 0; p < frames; p += blockSize)
            SessionMixer::render (s, start + p, out, p, juce::jmin (blockSize, frames - p), buffers, inserts);

        return out;
    }

    bool same (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        if (a.getNumSamples() != b.getNumSamples())
            return false;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
                if (a.getSample (ch, i) != b.getSample (ch, i))
                    return false;

        return true;
    }

    double rms (const juce::AudioBuffer<float>& b, int from, int n)
    {
        double sum = 0.0;

        for (int i = from; i < from + n; ++i)
            sum += (double) b.getSample (0, i) * b.getSample (0, i);

        return std::sqrt (sum / n);
    }

    void setPlain (juce::AudioPluginInstance& p, const juce::String& id, float plain)
    {
        for (auto* param : p.getParameters())
        {
            if (auto* f = dynamic_cast<juce::AudioParameterFloat*> (param); f != nullptr && f->paramID == id)
                BuiltInEffects::setExactly (*f, plain);
            else if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (param); c != nullptr && c->paramID == id)
                *c = (int) plain;
        }
    }

    void testMidi()
    {
        // Three notes, the pattern repeating every 2,000 frames over a 5,000-frame clip from 1,000: three
        // passes, the last cut by the clip's end, and a note at the pattern's length never played.
        NoteClip c;
        c.position = 1000;
        c.length = 5000;
        c.loopLength = 2000;
        c.notes = { { 60, 0, 500, 0.5f }, { 64, 500, 500, 0.5f }, { 67, 1500, 1000, 0.5f }, { 72, 2000, 100, 0.5f } };

        const std::vector<Event> expected {
            { 1000, true, 60 }, { 1500, false, 60 }, { 1500, true, 64 }, { 2000, false, 64 }, { 2500, true, 67 },
            { 3000, true, 60 }, { 3500, false, 67 }, { 3500, false, 60 }, { 3500, true, 64 }, { 4000, false, 64 }, { 4500, true, 67 },
            { 5000, true, 60 }, { 5500, false, 67 }, { 5500, false, 60 }, { 5500, true, 64 }, { 6000, false, 64 } };

        const auto one = eventsOf (c, 7000, 1), odd = eventsOf (c, 7000, 997), big = eventsOf (c, 7000, 4096);
        check (one == expected && odd == one && big == one,
               "note clips: the MIDI over blocks of 1, 997 and 4,096 frames is the same 16 events: each pass's note-offs before its "
               "note-ons at one frame, the last pass's last note turned off at the clip's end, a note at the pattern's length left out");

        // A clip that doesn't repeat: one pass, and the note at 2,000 is inside the clip now.
        c.loopLength = 0;
        const auto once = eventsOf (c, 7000, 64);
        check (once.size() == 8 && once.front() == Event { 1000, true, 60 } && once.back() == Event { 3500, false, 67 }
                   && std::count (once.begin(), once.end(), Event { 3000, true, 72 }) == 1,
               "note clips: without a loop length the notes play once (8 events, the fourth note in, the last note off at 3,500)");

        // Velocity and pitch limits.
        NoteClip loud;
        loud.length = 100;
        loud.notes = { { 200, 0, 10, 3.0f } };
        juce::MidiBuffer midi;
        loud.addMidi (0, 100, midi);
        auto first = true;
        auto limited = false;

        for (const auto meta : midi)
            if (std::exchange (first, false))
                limited = meta.getMessage().getNoteNumber() == 127 && meta.getMessage().getFloatVelocity() >= 0.99f;

        check (limited, "note clips: a pitch or velocity out of range is kept to 127 and 1");
    }

    void testEdits()
    {
        SessionState s;
        s.sampleRate = rate;
        const auto trackId = s.addTrack().id;
        const auto busId = s.addBus().id;
        const auto folderId = s.addFolder().id;

        NoteClip raw;
        raw.position = -5;
        raw.length = 0;
        raw.loopLength = 99;
        raw.notes = { { 200, -1, 0, 2.0f }, { -3, 10, 5, 0.0f } };

        const auto id = SessionEdits::addNoteClip (s, trackId, raw);
        const auto onBus = SessionEdits::addNoteClip (s, busId, raw), onFolder = SessionEdits::addNoteClip (s, folderId, raw);
        const auto where = s.findNoteClip (id);
        const auto* c = where.has_value() ? &s.tracks[(size_t) where->first].noteClips[(size_t) where->second] : nullptr;
        check (id != 0 && onBus == 0 && onFolder == 0 && c != nullptr && c->name == "Notes" && c->position == 0 && c->length == 1
                   && c->loopLength == 1 && c->notes.size() == 2 && c->notes[0] == ClipNote { 127, 0, 1, 1.0f }
                   && c->notes[1].pitch == 0 && c->notes[1].start == 10 && c->notes[1].length == 5 && c->notes[1].velocity > 0.0f,
               "note clips: added to a track (not a bus or folder) with its values kept to what plays");

        NoteClip values;
        values.name = "Chord";
        values.position = 123456;   // not taken
        values.length = 48000;
        values.loopLength = 12000;
        values.notes = { { 60, 0, 6000, 0.5f }, { 64, 0, 6000, 0.5f } };
        values.colour = 3;
        check (SessionEdits::replaceNoteClip (s, id, values) && SessionEdits::moveNoteClip (s, id, 700)
                   && s.tracks[0].noteClips[0].name == "Chord" && s.tracks[0].noteClips[0].position == 700 && s.tracks[0].noteClips[0].length == 48000
                   && s.tracks[0].noteClips[0].loopLength == 12000 && s.tracks[0].noteClips[0].notes == values.notes && s.tracks[0].noteClips[0].colour == 3
                   && s.tracks[0].noteClips[0].id == id && SessionEdits::moveNoteClip (s, id, -9) && s.tracks[0].noteClips[0].position == 0
                   && s.getLength() == 48000 && ! SessionEdits::replaceNoteClip (s, 999999, values) && ! SessionEdits::moveNoteClip (s, 999999, 0),
               "note clips: replaced (keeping its id and place) and moved (not before 0); the session's length counts it");

        check (SessionEdits::setNoteClipMuted (s, id, true) && ! SessionEdits::setNoteClipMuted (s, id, true) && s.tracks[0].noteClips[0].muted
                   && SessionEdits::setNoteClipMuted (s, id, false) && ! s.tracks[0].noteClips[0].muted,
               "note clips: muted and unmuted, a repeat refused");

        // Time edits: clips inside deleted time go, later ones move back, one across stays; inserted
        // time moves the clips from it on.
        auto& track = s.tracks[0];
        track.noteClips.clear();
        NoteClip a;
        a.length = 1000;

        for (auto position : { 10000, 20000, 30000 })
        {
            a.position = position;
            SessionEdits::addNoteClip (s, trackId, a);
        }

        a.position = 5000;
        a.length = 10000;
        const auto across = SessionEdits::addNoteClip (s, trackId, a);
        const auto deleted = SessionEdits::deleteTime (s, 9500, 21000);
        const auto& after = s.tracks[0].noteClips;
        check (deleted && after.size() == 2 && after[0].position == 18500 && after[1].id == across && after[1].position == 5000
                   && SessionEdits::insertTime (s, 6000, 1000) && after[0].position == 19500 && after[1].position == 5000,
               "note clips: deleting time removes the clips inside it and moves the later ones back, inserting time moves them on, "
               "and one across the edit stays where it is");

        const auto copyId = SessionEdits::duplicateTrack (s, 0);
        check (copyId != 0 && s.tracks[1].noteClips.size() == 2 && s.tracks[1].noteClips[0].id != s.tracks[0].noteClips[0].id
                   && s.tracks[1].noteClips[0].position == 19500 && SessionEdits::removeNoteClip (s, s.tracks[1].noteClips[0].id)
                   && s.tracks[1].noteClips.size() == 1 && ! SessionEdits::removeNoteClip (s, 999999),
               "note clips: a duplicated track's copies have ids of their own; removed by id");
    }

    void testFile (juce::AudioFormatManager& formats)
    {
        SessionState s;
        s.sampleRate = rate;
        auto& t = s.addTrack();
        NoteClip c;
        c.name = "Riff";
        c.position = 96000;
        c.length = 48000;
        c.loopLength = 24000;
        c.notes = { { 60, 0, 6000, 0.5f }, { 67, 12000, 6000, 0.25f } };
        c.muted = true;
        c.colour = 2;
        const auto id = SessionEdits::addNoteClip (s, t.id, c);

        const auto file = juce::File::createTempFile (".sbsession");
        const auto error = SessionFile::save (s, file);
        const auto loaded = SessionFile::load (file, formats);
        file.deleteFile();

        const auto* back = loaded.state.tracks.size() == 1 && loaded.state.tracks[0].noteClips.size() == 1 ? &loaded.state.tracks[0].noteClips[0] : nullptr;
        check (id != 0 && error.isEmpty() && loaded.error.isEmpty() && back != nullptr && back->name == "Riff" && back->position == 96000
                   && back->length == 48000 && back->loopLength == 24000 && back->notes == c.notes && back->muted && back->colour == 2,
               "note clips: saved in the session file and read back (name, place, length, loop, notes, mute, colour)");
    }

    /** A track with a constant stereo clip from 0 to 9,000 and a repeating note clip. */
    SessionState clicksSession (juce::uint32& trackId)
    {
        SessionState s;
        s.sampleRate = rate;
        auto& t = s.addTrack();
        trackId = t.id;
        SessionClip clip;
        clip.id = s.nextId++;
        clip.audio = constant (9000, 0.25f);
        clip.length = 9000;
        t.clips.push_back (clip);

        NoteClip notes;
        notes.position = 2000;
        notes.length = 4000;
        notes.loopLength = 1000;
        notes.notes = { { 60, 0, 100, 0.5f }, { 64, 300, 100, 0.5f } };
        SessionEdits::addNoteClip (s, trackId, notes);
        return s;
    }

    void testChain()
    {
        juce::uint32 trackId = 0;
        auto s = clicksSession (trackId);

        // A gain after the instrument (added first, the instrument still goes in front): the clip's
        // audio and the clicks summed, then halved.
        TrackEffects effects;
        effects.prepare (rate);
        auto gain = std::make_unique<GainPlugin>();
        gain->gain->value = 0.25f;
        effects.add (trackId, std::move (gain));
        auto click = std::make_unique<ClickInstrument> (37);
        auto* clickPtr = click.get();
        const auto instrumentSlot = effects.add (trackId, std::move (click));
        effects.move (trackId, 0, 1);
        effects.move (trackId, 1, 0);

        check (effects.getInstrument (trackId) == clickPtr && effects.getPlugin (trackId, 0) == clickPtr && effects.getSlotId (trackId, 0) == instrumentSlot
                   && effects.getNumSlots (trackId) == 2 && effects.getChainLatency (trackId) == 37 && TrackEffects::isInstrument (*clickPtr),
               "instrument tracks: an instrument added after an effect goes first in the chain and stays there through moves");

        const auto out = play (s, 0, 9000, 64, &effects);
        const std::vector<int> clicks { 2000, 2300, 3000, 3300, 4000, 4300, 5000, 5300 };
        auto placed = true, quiet = true;

        for (auto at : clicks)
            placed = placed && out.getSample (0, at) == 0.625f && out.getSample (1, at) == 0.625f
                            && out.getSample (0, at - 1) == 0.125f && out.getSample (0, at + 1) == 0.125f;

        for (auto at : { 6000, 6300, 1000, 8000 })
            quiet = quiet && out.getSample (0, at) == 0.125f;

        check (placed && quiet && clickPtr->notes.size() == 8 && clickPtr->offs.size() == 8,
               "instrument tracks: each note's click lands on its session frame through the instrument's 37-frame latency, added to "
               "the clip's audio and halved by the gain after it; nothing past the clip's end");

        const auto odd = play (s, 0, 9000, 1000, &effects);
        const auto ones = play (s, 0, 9000, 64, &effects);
        check (same (out, odd) && same (out, ones), "instrument tracks: the mix is the same in blocks of 64 and 1,000 frames, and again from the start");

        // A jump: the instrument hears every note off, and the clicks after the jump still land.
        clickPtr->allOff = 0;
        const auto later = play (s, 4000, 2000, 64, &effects);
        check (clickPtr->allOff >= 1 && later.getSample (0, 300) == 0.625f && later.getSample (0, 1300) == 0.625f && later.getSample (0, 299) == 0.125f,
               "instrument tracks: after a jump the instrument is told all notes off, and plays in time from there");

        // Muted notes play nothing; bypassing the instrument leaves the clip alone.
        auto muted = s;
        SessionEdits::setNoteClipMuted (muted, muted.tracks[0].noteClips[0].id, true);
        const auto silent = play (muted, 0, 9000, 64, &effects);
        effects.setBypassed (trackId, 0, true);
        const auto bypassed = play (s, 0, 9000, 64, &effects);
        effects.setBypassed (trackId, 0, false);
        check (silent.getSample (0, 2000) == 0.125f && bypassed.getSample (0, 2000) == 0.125f && effects.getChainLatency (trackId) == 37,
               "instrument tracks: a muted note clip and a bypassed instrument both leave the clip's audio alone");

        // On a 5.1 track the instrument needs no copies and plays into L and R alone.
        TrackEffects wide;
        wide.prepare (rate);
        wide.makeCopy = [] (const juce::PluginDescription&) -> std::unique_ptr<juce::AudioPluginInstance> { return nullptr; };
        wide.add (trackId, std::make_unique<ClickInstrument> (0));
        const auto failed = wide.setWideTracks ({ trackId });
        auto track = s.tracks[0];
        track.fiveOne = true;
        juce::AudioBuffer<float> six (6, 64);

        for (int ch = 0; ch < 6; ++ch)
            juce::FloatVectorOperations::fill (six.getWritePointer (ch), 0.5f, 64);

        wide.process (track, 2000, six, 64, {});
        check (failed.isEmpty() && six.getSample (0, 0) == 1.5f && six.getSample (1, 0) == 1.5f && six.getSample (0, 1) == 0.5f
                   && six.getSample (2, 0) == 0.5f && six.getSample (5, 0) == 0.5f,
               "instrument tracks: on a 5.1 track the instrument is not copied, and plays into L and R, the other channels untouched");
    }

    void testSynth (AudioEngine& engine)
    {
        juce::PluginDescription synthDescription;

        for (auto& d : BuiltInEffects::getDescriptions())
            if (d.fileOrIdentifier == BuiltInEffects::basicSynthId)
                synthDescription = d;

        juce::String error;
        auto synth = engine.pluginFormatManager.createPluginInstance (synthDescription, rate, 512, error);

        if (synth == nullptr)
        {
            check (false, "instrument tracks: the basic synth could not be made: " + error);
            return;
        }

        // A bare sine from the synth on a track, one note over the first half second.
        setPlain (*synth, "wave", 0);
        setPlain (*synth, "level", 0.0f);
        setPlain (*synth, "attack", 0.0f);
        setPlain (*synth, "decay", 0.0f);
        setPlain (*synth, "sustain", 100.0f);
        setPlain (*synth, "release", 0.0f);

        SessionState s;
        s.sampleRate = rate;
        auto& t = s.addTrack();
        const auto trackId = t.id;
        NoteClip notes;
        notes.length = 48000;
        notes.notes = { { 69, 0, 24000, 1.0f } };
        SessionEdits::addNoteClip (s, trackId, notes);

        TrackEffects effects;
        effects.prepare (rate);
        const auto slotId = effects.add (trackId, std::move (synth));

        const auto plain = play (s, 0, 48000, 512, &effects);
        const auto sounding = rms (plain, 2000, 20000), after = rms (plain, 30000, 18000);
        check (sounding > 0.3 && after < 1.0e-6,
               "instrument tracks: the basic synth on a track plays a note clip (RMS " + juce::String (sounding, 3) + " through the note, "
                   + juce::String (after, 9) + " after it)");

        // Its level automated down to the bottom of its range (-40 dB): a hundredth of the sound.
        auto automated = s;
        automated.tracks[0].effectEnvelopes.push_back ({ slotId, "level", Envelope { { { 0, 0.0f }, { 47999, 0.0f } } } });
        const auto low = play (automated, 0, 48000, 512, &effects);
        const auto ratio = rms (low, 2000, 20000) / sounding;
        check (std::abs (ratio - 0.01) < 0.0005,
               "instrument tracks: an envelope on the synth's level follows it like any effect parameter (ratio " + juce::String (ratio, 5) + " for -40 dB)");

        // Freezing renders the note clips (they are in the source), and a bounce or mixdown loads
        // the same chain afresh from its XML (the synth's state as the envelope left it), instrument
        // first, and mixes the same as the live chain does now.
        const auto live = play (s, 0, 48000, 512, &effects);
        const auto frozen = SessionEdits::frozenSource (s, trackId);
        const auto* frozenTrack = frozen.findTrack (trackId);
        std::map<juce::uint32, std::unique_ptr<juce::XmlElement>> xml;
        xml[trackId] = effects.createXml (trackId);
        TrackEffects offline;
        const auto errors = OfflineRender::loadTrackEffects (xml, engine.pluginFormatManager, rate, offline);
        const auto again = play (s, 0, 48000, 512, &offline);
        check (frozenTrack != nullptr && frozenTrack->noteClips.size() == 1 && xml[trackId] != nullptr && errors.isEmpty()
                   && offline.getInstrument (trackId) != nullptr && same (live, again),
               "instrument tracks: what freezing renders keeps the note clips, and the chain a bounce or mixdown loads from the "
                   "session's XML has the instrument first and mixes the same, bit for bit");
    }
}

void runInstrumentTrackTests (AudioEngine& engine)
{
    testMidi();
    testEdits();
    testFile (engine.formatManager);
    testChain();
    testSynth (engine);
}
