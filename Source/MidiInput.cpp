#include "MidiInput.h"
#include "Theme.h"

namespace MidiIn
{
    Live::Live()
    {
        held.fill (-1);
        heldVelocity.fill (0.0f);
        collector.reset (rate);
    }

    void Live::prepare (double sampleRate)
    {
        rate = sampleRate;
        collector.reset (sampleRate);
    }

    void Live::addMessage (const juce::MidiMessage& m)
    {
        auto stamped = m;

        if (stamped.getTimeStamp() <= 0.0)
            stamped.setTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001);

        collector.addMessageToQueue (stamped);
    }

    bool Live::takeBlock (juce::MidiBuffer& into, int numFrames)
    {
        into.clear();
        collector.removeNextBlockOfMessages (into, numFrames);
        const auto any = ! into.isEmpty();

        if (any)
            sounding = (juce::int64) (tailSeconds * rate);
        else if (sounding > 0)
            sounding = juce::jmax ((juce::int64) 0, sounding.load() - numFrames);

        return any;
    }

    void Live::startCapture()
    {
        const juce::SpinLock::ScopedLockType sl (captureLock);
        captured.clear();
        controllers.clear();
        held.fill (-1);
        capturing = true;
    }

    Capture Live::stopCapture (juce::int64 endFrame)
    {
        capturing = false;
        const juce::SpinLock::ScopedLockType sl (captureLock);

        for (int pitch = 0; pitch < 128; ++pitch)
            if (held[(size_t) pitch] >= 0)
            {
                if (endFrame > held[(size_t) pitch])
                    captured.push_back ({ pitch, held[(size_t) pitch], endFrame, heldVelocity[(size_t) pitch] });

                held[(size_t) pitch] = -1;
            }

        return { std::exchange (captured, {}), std::exchange (controllers, {}) };
    }

    void Live::captureBlock (const juce::MidiBuffer& block, juce::int64 blockFrame)
    {
        if (! capturing || block.isEmpty())
            return;

        // The message thread holds the lock only briefly (start, stop); a block that finds it held is lost.
        const juce::SpinLock::ScopedTryLockType sl (captureLock);

        if (! sl.isLocked())
            return;

        for (const auto meta : block)
        {
            const auto m = meta.getMessage();
            const auto frame = blockFrame + meta.samplePosition;

            if (m.isNoteOn())
            {
                const auto pitch = (size_t) m.getNoteNumber();

                if (held[pitch] >= 0 && frame > held[pitch])
                    captured.push_back ({ (int) pitch, held[pitch], frame, heldVelocity[pitch] });

                held[pitch] = frame;
                heldVelocity[pitch] = m.getFloatVelocity();
            }
            else if (m.isController())
            {
                controllers.push_back ({ m.getControllerNumber(), frame, (float) m.getControllerValue() / 127.0f });
            }
            else if (m.isPitchWheel())
            {
                controllers.push_back ({ ControllerLane::pitchBend, frame, (float) m.getPitchWheelValue() / 16383.0f });
            }
            else if (m.isNoteOff() || m.isAllNotesOff())
            {
                for (int pitch = 0; pitch < 128; ++pitch)
                {
                    if (! (m.isAllNotesOff() || pitch == m.getNoteNumber()) || held[(size_t) pitch] < 0)
                        continue;

                    if (frame > held[(size_t) pitch])
                        captured.push_back ({ pitch, held[(size_t) pitch], frame, heldVelocity[(size_t) pitch] });

                    held[(size_t) pitch] = -1;
                }
            }
        }
    }

    //==============================================================================
    NoteClip clipOf (const Capture& capture, juce::int64 from)
    {
        NoteClip clip;
        clip.name = "Recorded";
        const auto& notes = capture.notes;

        if (notes.empty())
            return clip;

        auto first = std::numeric_limits<juce::int64>::max();
        juce::int64 last = 0;

        for (auto& n : notes)
        {
            first = juce::jmin (first, n.on);
            last = juce::jmax (last, n.off);
        }

        for (auto& c : capture.controllers)
            last = juce::jmax (last, c.frame + 1);

        clip.position = juce::jmax ((juce::int64) 0, juce::jmin (first, from));
        clip.length = juce::jmax ((juce::int64) 1, last - clip.position);

        for (auto& n : notes)
            clip.notes.push_back ({ juce::jlimit (0, 127, n.pitch), n.on - clip.position, juce::jmax ((juce::int64) 1, n.off - n.on), juce::jlimit (0.0f, 1.0f, n.velocity) });

        std::sort (clip.notes.begin(), clip.notes.end(), [] (const ClipNote& a, const ClipNote& b) { return a.start < b.start; });

        // Each controller's moves as a lane, in time order (moves before the clip start at 0).
        for (auto& c : capture.controllers)
        {
            auto* lane = const_cast<ControllerLane*> (clip.findLane (c.controller));

            if (lane == nullptr)
            {
                clip.lanes.push_back ({ c.controller, {} });
                lane = &clip.lanes.back();
            }

            lane->envelope.add (juce::jmax ((juce::int64) 0, c.frame - clip.position), juce::jlimit (0.0f, 1.0f, c.value));
        }

        return clip;
    }

    Imported importFile (const juce::File& file, double sampleRate)
    {
        juce::FileInputStream in (file);

        if (! in.openedOk())
            return { {}, "Could not read " + file.getFileName() + "." };

        auto imported = importFile (in, sampleRate);

        if (imported.error.isNotEmpty())
            imported.error = file.getFileName() + ": " + imported.error;

        return imported;
    }

    Imported importFile (juce::InputStream& in, double sampleRate)
    {
        juce::MidiFile midi;

        if (! midi.readFrom (in, true))
            return { {}, "not a Standard MIDI File." };

        midi.convertTimestampTicksToSeconds();
        Imported out;

        for (int t = 0; t < midi.getNumTracks(); ++t)
        {
            const auto* track = midi.getTrack (t);

            if (track == nullptr)
                continue;

            auto sequence = *track;
            sequence.updateMatchedPairs();
            NoteClip clip;
            juce::int64 last = 0;

            for (int i = 0; i < sequence.getNumEvents(); ++i)
            {
                const auto* event = sequence.getEventPointer (i);
                const auto& m = event->message;

                if (m.isTrackNameEvent() && clip.name.isEmpty())
                    clip.name = m.getTextFromTextMetaEvent().trim();

                if (! m.isNoteOn() || event->noteOffObject == nullptr)
                    continue;

                const auto on = (juce::int64) std::llround (m.getTimeStamp() * sampleRate);
                const auto off = juce::jmax (on + 1, (juce::int64) std::llround (event->noteOffObject->message.getTimeStamp() * sampleRate));
                clip.notes.push_back ({ m.getNoteNumber(), on, off - on, m.getFloatVelocity() });
                last = juce::jmax (last, off);
            }

            if (clip.notes.empty())
                continue;

            if (clip.name.isEmpty())
                clip.name = "Track " + juce::String (t + 1);

            clip.length = last;
            std::sort (clip.notes.begin(), clip.notes.end(), [] (const ClipNote& a, const ClipNote& b) { return a.start < b.start; });
            out.clips.push_back (std::move (clip));
        }

        if (out.clips.empty())
            out.error = "no notes in it.";

        return out;
    }

    //==============================================================================
    KeyboardPanel::KeyboardPanel (Live& l) : live (l)
    {
        addAndMakeVisible (keyboard);
        keyboard.setAvailableRange (36, 96);
        keyboard.setOctaveForMiddleC (4);
        state.addListener (this);
        addAndMakeVisible (status);
        status.setFont (juce::Font (juce::FontOptions (12.0f)));
        Theme::follow (status, juce::Label::textColourId, Theme::dimText);
        status.setText ("Plays the selected track's instrument.", juce::dontSendNotification);
    }

    KeyboardPanel::~KeyboardPanel()
    {
        state.removeListener (this);
    }

    void KeyboardPanel::setStatus (const juce::String& text)
    {
        status.setText (text, juce::dontSendNotification);
    }

    void KeyboardPanel::resized()
    {
        auto area = getLocalBounds();
        status.setBounds (area.removeFromTop (22).reduced (6, 0));
        keyboard.setBounds (area.reduced (4));
    }

    void KeyboardPanel::paint (juce::Graphics& g)
    {
        g.fillAll (Theme::base);
    }

    void KeyboardPanel::handleNoteOn (juce::MidiKeyboardState*, int channel, int note, float velocity)
    {
        live.addMessage (juce::MidiMessage::noteOn (channel, note, velocity));
    }

    void KeyboardPanel::handleNoteOff (juce::MidiKeyboardState*, int channel, int note, float velocity)
    {
        live.addMessage (juce::MidiMessage::noteOff (channel, note, velocity));
    }
}
