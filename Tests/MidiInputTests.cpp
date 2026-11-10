#include <JuceHeader.h>
#include "../Source/MidiInput.h"
#include "../Source/TrackEffects.h"
#include "TestPlugins.h"
#include "TestUtils.h"
#include "MidiInputTests.h"

namespace
{
    using TestUtils::check;
    using TestPlugins::ClickInstrument;

    constexpr double rate = 48000.0;

    std::vector<std::pair<int, int>> onsIn (const juce::MidiBuffer& b)
    {
        std::vector<std::pair<int, int>> out;

        for (const auto meta : b)
            if (meta.getMessage().isNoteOn())
                out.emplace_back (meta.samplePosition, meta.getMessage().getNoteNumber());

        return out;
    }

    void testLive()
    {
        MidiIn::Live live;
        live.prepare (rate);
        const auto now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        live.addMessage (juce::MidiMessage::noteOn (1, 60, 0.8f).withTimeStamp (now));
        live.addMessage (juce::MidiMessage::noteOn (1, 64, 0.8f).withTimeStamp (now + 0.001));
        live.addMessage (juce::MidiMessage::noteOff (1, 60).withTimeStamp (now + 0.002));

        juce::MidiBuffer block;
        const auto any = live.takeBlock (block, 4096);
        const auto ons = onsIn (block);
        const auto sounding = live.getSoundingFrames();
        juce::MidiBuffer empty;
        const auto none = live.takeBlock (empty, 4096);
        check (any && ons.size() == 2 && ons[0].second == 60 && ons[1].second == 64 && ons[0].first <= ons[1].first && block.getNumEvents() == 3
                   && sounding == (juce::int64) (MidiIn::Live::tailSeconds * rate) && ! none && empty.isEmpty()
                   && live.getSoundingFrames() == sounding - 4096,
               "live MIDI: queued messages come out of the next block in order, then nothing, and the instrument is kept running for the tail");

        // A capture keeps a block's notes at their session frames; a note still held ends at the stop.
        live.startCapture();
        juce::MidiBuffer played;
        played.addEvent (juce::MidiMessage::noteOn (1, 60, 0.5f), 3);
        played.addEvent (juce::MidiMessage::noteOff (1, 60), 100);
        played.addEvent (juce::MidiMessage::noteOn (1, 64, 1.0f), 150);
        played.addEvent (juce::MidiMessage::controllerEvent (1, 1, 64), 20);
        played.addEvent (juce::MidiMessage::pitchWheel (1, 16383), 160);
        played.addEvent (juce::MidiMessage::controllerEvent (1, 1, 127), 170);
        live.captureBlock (played, 1000);
        const auto capture = live.stopCapture (2000);
        const auto& notes = capture.notes;
        const auto clip = MidiIn::clipOf (capture, 900);
        const auto* wheel = clip.findLane (1);
        const auto* bend = clip.findLane (ControllerLane::pitchBend);
        check (notes.size() == 2 && notes[0].pitch == 60 && notes[0].on == 1003 && notes[0].off == 1100 && std::abs (notes[0].velocity - 0.5f) < 0.01f
                   && notes[1].pitch == 64 && notes[1].on == 1150 && notes[1].off == 2000 && ! live.isCapturing()
                   && clip.position == 900 && clip.length == 1100 && clip.notes.size() == 2 && clip.notes[0] == ClipNote { 60, 103, 97, notes[0].velocity }
                   && clip.notes[1].start == 250 && clip.notes[1].length == 850 && MidiIn::clipOf ({}, 5).notes.empty()
                   && capture.controllers.size() == 3 && wheel != nullptr && wheel->envelope.points.size() == 2 && wheel->envelope.points[0].frame == 120
                   && std::abs (wheel->envelope.points[0].value - 64.0f / 127.0f) < 1.0e-5f && wheel->envelope.points[1] == AutomationPoint { 270, 1.0f }
                   && bend != nullptr && bend->envelope.points.size() == 1 && bend->envelope.points[0] == AutomationPoint { 260, 1.0f },
               "live MIDI: captured notes keep their session frames (a note still held ends at the stop), and make a clip from the record start, "
               "the wheel's and the bend's moves as lanes");

        // Blocks taken while not capturing are not kept; a note-off with nothing held is nothing.
        live.captureBlock (played, 5000);
        live.startCapture();
        juce::MidiBuffer offOnly;
        offOnly.addEvent (juce::MidiMessage::noteOff (1, 72), 0);
        live.captureBlock (offOnly, 0);
        const auto nothing = live.stopCapture (100);
        check (nothing.notes.empty() && nothing.controllers.empty(), "live MIDI: nothing is kept outside a capture, nor a note-off with no note on");
    }

    void testImport()
    {
        // Two tracks: one named, with two notes; one of nothing but a name. At 120 BPM (the
        // default) a quarter note is half a second.
        juce::MidiFile file;
        file.setTicksPerQuarterNote (960);
        juce::MidiMessageSequence lead, empty;
        lead.addEvent (juce::MidiMessage::textMetaEvent (3, "Lead"), 0.0);
        lead.addEvent (juce::MidiMessage::noteOn (1, 60, 0.75f), 0.0);
        lead.addEvent (juce::MidiMessage::noteOff (1, 60), 960.0);
        lead.addEvent (juce::MidiMessage::noteOn (1, 67, 1.0f), 1920.0);
        lead.addEvent (juce::MidiMessage::noteOff (1, 67), 2400.0);
        lead.addEvent (juce::MidiMessage::endOfTrack(), 2400.0);
        empty.addEvent (juce::MidiMessage::textMetaEvent (3, "Nothing"), 0.0);
        empty.addEvent (juce::MidiMessage::endOfTrack(), 10.0);
        file.addTrack (lead);
        file.addTrack (empty);

        juce::MemoryOutputStream out;
        file.writeTo (out);
        juce::MemoryInputStream in (out.getData(), out.getDataSize(), false);
        const auto imported = MidiIn::importFile (in, rate);
        const auto* clip = imported.clips.size() == 1 ? &imported.clips[0] : nullptr;
        check (imported.error.isEmpty() && clip != nullptr && clip->name == "Lead" && clip->notes.size() == 2 && clip->length == 60000
                   && clip->notes[0].pitch == 60 && clip->notes[0].start == 0 && clip->notes[0].length == 24000
                   && clip->notes[1].pitch == 67 && clip->notes[1].start == 48000 && clip->notes[1].length == 12000
                   && std::abs (clip->notes[0].velocity - 0.75f) < 0.01f,
               "MIDI files: a file's tracks with notes become note clips named by the track, at the session rate through the tempo (a quarter note 24,000 frames at 120)");

        juce::MemoryInputStream junk ("not midi at all", 14, false);
        juce::MidiFile nothing;
        nothing.addTrack (empty);
        juce::MemoryOutputStream noNotes;
        nothing.writeTo (noNotes);
        juce::MemoryInputStream noNotesIn (noNotes.getData(), noNotes.getDataSize(), false);
        check (MidiIn::importFile (junk, rate).error.isNotEmpty() && MidiIn::importFile (noNotesIn, rate).error.isNotEmpty()
                   && MidiIn::importFile (juce::File ("/no/such/file.mid"), rate).error.isNotEmpty(),
               "MIDI files: what isn't one, one with no notes, and a missing file are refused with a reason");
    }

    void testKeyboard()
    {
        MidiIn::Live live;
        live.prepare (rate);
        MidiIn::KeyboardPanel panel (live);
        panel.setSize (600, 100);
        juce::MidiBuffer block;
        live.takeBlock (block, 1024);   // nothing yet
        const auto quiet = block.isEmpty();

        // The keyboard's state is what the mouse and keys drive: a note on it reaches the live input.
        panel.getState().noteOn (1, 60, 0.8f);
        panel.getState().noteOff (1, 60, 0.0f);
        live.takeBlock (block, 1024);
        const auto ons = onsIn (block);
        check (quiet && ons.size() == 1 && ons[0].second == 60 && block.getNumEvents() == 2,
               "keyboard: a key pressed on the on-screen keyboard reaches the live input as its note on and off");
    }

    void testChain()
    {
        // The live messages reach the instrument of the track they are for, at their frames,
        // through a block mixed in two runs; another track's instrument hears nothing.
        TrackEffects effects;
        effects.prepare (rate);
        auto click = std::make_unique<ClickInstrument> (0);
        auto* clickPtr = click.get();
        auto other = std::make_unique<ClickInstrument> (0);
        auto* otherPtr = other.get();
        effects.add (7, std::move (click));
        effects.add (8, std::move (other));

        SessionTrack track, second;
        track.id = 7;
        second.id = 8;
        juce::MidiBuffer live;
        live.addEvent (juce::MidiMessage::noteOn (1, 60, 0.9f), 10);
        live.addEvent (juce::MidiMessage::noteOn (1, 62, 0.9f), 700);
        live.addEvent (juce::MidiMessage::noteOff (1, 60), 900);
        effects.setLiveMidi (7, &live);

        juce::AudioBuffer<float> a (2, 512), b (2, 512), c (2, 512);
        a.clear(); b.clear(); c.clear();
        effects.process (track, 0, a, 512, {});
        effects.process (track, 512, b, 512, {});
        effects.process (second, 0, c, 512, {});

        check (a.getSample (0, 10) == 1.0f && b.getSample (0, 700 - 512) == 1.0f && clickPtr->notes.size() == 2 && clickPtr->offs.size() == 1
                   && clickPtr->offs[0].second == 900 && otherPtr->notes.empty() && c.getSample (0, 10) == 0.0f,
               "live MIDI: the track's instrument plays the block's messages at their frames across two runs; another track's hears nothing");

        // The next block without messages: nothing more.
        effects.setLiveMidi (7, nullptr);
        a.clear();
        effects.process (track, 1024, a, 512, {});
        check (clickPtr->notes.size() == 2 && a.getSample (0, 10) == 0.0f, "live MIDI: a block without messages plays nothing new");
    }
}

void runMidiInputTests()
{
    std::cout << "MIDI input:" << std::endl;
    testLive();
    testImport();
    testKeyboard();
    testChain();
}
