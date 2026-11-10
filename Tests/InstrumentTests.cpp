#include <JuceHeader.h>
#include "../Source/Instrument.h"
#include "../Source/BuiltInEffects.h"
#include "../Source/AudioEngine.h"
#include "TestUtils.h"
#include "TestPlugins.h"
#include "InstrumentTests.h"

namespace
{
    using TestUtils::check;
    constexpr double rate = 48000.0;

    /** A sine's amplitude in [from, from + n) of a channel. */
    double amplitude (const juce::AudioBuffer<float>& b, int channel, double frequency, int from, int n)
    {
        double re = 0.0, im = 0.0;

        for (int i = from; i < from + n; ++i)
        {
            const auto w = juce::MathConstants<double>::twoPi * frequency * i / rate;
            re += b.getSample (channel, i) * std::cos (w);
            im += b.getSample (channel, i) * std::sin (w);
        }

        return 2.0 * std::sqrt (re * re + im * im) / n;
    }

    /** Sets a parameter by its ID to a plain value (a choice's index). */
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

    float plainOf (juce::AudioPluginInstance& p, const juce::String& id)
    {
        for (auto* param : p.getParameters())
        {
            if (auto* f = dynamic_cast<juce::AudioParameterFloat*> (param); f != nullptr && f->paramID == id)
                return f->get();

            if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (param); c != nullptr && c->paramID == id)
                return (float) c->getIndex();
        }

        return std::numeric_limits<float>::quiet_NaN();
    }

    /** The synth bare: a sine at 0 dB, no envelope, the filter open. */
    void setBare (juce::AudioPluginInstance& synth)
    {
        setPlain (synth, "wave", 0);
        setPlain (synth, "level", 0.0f);
        setPlain (synth, "attack", 0.0f);
        setPlain (synth, "decay", 0.0f);
        setPlain (synth, "sustain", 100.0f);
        setPlain (synth, "release", 0.0f);
        setPlain (synth, "cutoff", 20000.0f);
        setPlain (synth, "resonance", 0.0f);
    }

    bool channelsSame (const juce::AudioBuffer<float>& b)
    {
        for (int ch = 1; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (! juce::exactlyEqual (b.getSample (ch, i), b.getSample (0, i)))
                    return false;

        return true;
    }

    int countNonZero (const juce::AudioBuffer<float>& b, int channel)
    {
        int n = 0;

        for (int i = 0; i < b.getNumSamples(); ++i)
            n += b.getSample (channel, i) != 0.0f ? 1 : 0;

        return n;
    }
}

void runInstrumentTests (AudioEngine& engine)
{
    using namespace Instrument;

    // Pitches by name and number.
    check (parsePitch ("C4") == 60 && parsePitch ("c4") == 60 && parsePitch ("C-1") == 0 && parsePitch ("G9") == 127 && parsePitch ("F#3") == 54
               && parsePitch ("Bb2") == 46 && parsePitch ("bb2") == 46 && parsePitch (" 69 ") == 69 && parsePitch ("A4") == 69 && parsePitch ("0") == 0
               && ! parsePitch ("H2") && ! parsePitch ("C") && ! parsePitch ("G#9") && ! parsePitch ("128") && ! parsePitch ("C10") && ! parsePitch ("")
               && ! parsePitch ("C4x") && ! parsePitch ("-5") && ! parsePitch ("C-") && ! parsePitch ("#4")
               && pitchName (60) == "C4" && pitchName (61) == "C#4" && pitchName (0) == "C-1" && pitchName (127) == "G9" && pitchName (46) == "A#2",
           "instrument: pitches from names (C4 is 60, sharps and flats, octaves -1 to 9) and numbers, the rest refused, and named back with sharps");

    const auto chord = parsePitches ("C4, E4,G4");
    const auto spaced = parsePitches ("60 64 67");
    check (chord.has_value() && *chord == std::vector<int> { 60, 64, 67 } && spaced.has_value() && *spaced == *chord
               && ! parsePitches ("") && ! parsePitches ("C4, X") && ! parsePitches (", ,") && parsePitches ("C4,,E4").has_value(),
           "instrument: pitches split at commas or spaces, an empty list or a bad one refusing the lot");

    // Phrases: together, in turn, their limits and their MIDI.
    const auto tog = together ({ 60, 64 }, 0.5, 2.0, 0.8f);
    const auto turn = inTurn ({ 60, 64, 67 }, 0.25, 1.0, 1.0f);
    check (tog.notes.size() == 2 && tog.notes[1] == Note { 64, 0.0, 0.5, 0.8f } && juce::exactlyEqual (tog.lengthSeconds, 2.0)
               && turn.notes.size() == 3 && turn.notes[1] == Note { 64, 0.25, 0.25, 1.0f } && turn.notes[2] == Note { 67, 0.5, 0.25, 1.0f }
               && isValid (tog) && isValid (turn)
               && ! isValid (together ({ 128 }, 1.0, 1.0, 1.0f)) && ! isValid (together ({ 60 }, 0.0, 1.0, 1.0f)) && ! isValid (together ({ 60 }, 1.0, 0.0, 1.0f))
               && ! isValid (together ({ 60 }, 1.0, 601.0, 1.0f)) && ! isValid (together ({ 60 }, 1.0, 1.0, 1.5f)) && isValid (together ({}, 1.0, 1.0, 1.0f)),
           "instrument: notes together all start at once, in turn each as the last ends; pitches, holds, lengths and velocities out of range refused");

    {
        const auto midi = midiOf (turn, 1000.0);   // a frame a millisecond
        std::vector<std::tuple<int, bool, int>> seen;   // frame, on, pitch

        for (int i = 0; i < midi.getNumEvents(); ++i)
        {
            const auto& m = midi.getEventPointer (i)->message;
            seen.push_back ({ (int) m.getTimeStamp(), m.isNoteOn(), m.getNoteNumber() });
        }

        const std::vector<std::tuple<int, bool, int>> expected { { 0, true, 60 }, { 250, false, 60 }, { 250, true, 64 }, { 500, false, 64 }, { 500, true, 67 }, { 750, false, 67 } };
        const auto past = midiOf (inTurn ({ 60, 62 }, 1.0, 1.0, 1.0f), 1000.0);
        const auto held = midiOf (together ({ 60 }, 5.0, 1.0, 0.5f), 1000.0);
        check (seen == expected && midi.getEventPointer (0)->message.getChannel() == 1
                   && std::abs (midi.getEventPointer (0)->message.getFloatVelocity() - 1.0f) < 0.01f
                   && past.getNumEvents() == 2 && held.getNumEvents() == 2 && held.getEventPointer (1)->message.getTimeStamp() == 5000.0,
               "instrument: a note-on at each start and a note-off where its hold ends, the off before the on at the same frame, "
               "a note starting at the length left out, one held past it ending past it");
    }

    // The built-in synth, made from its description by the app's format manager.
    juce::PluginDescription synthDescription;

    for (auto& d : BuiltInEffects::getDescriptions())
        if (d.fileOrIdentifier == BuiltInEffects::basicSynthId)
            synthDescription = d;

    juce::String error;
    auto instance = engine.pluginFormatManager.createPluginInstance (synthDescription, rate, 512, error);
    auto* synth = dynamic_cast<BuiltInEffects::BasicSynthProcessor*> (instance.get());
    check (synth != nullptr && error.isEmpty() && synthDescription.isInstrument && synthDescription.numInputChannels == 0
               && synth->acceptsMidi() && ! synth->producesMidi() && synth->getTotalNumInputChannels() == 0 && synth->getTotalNumOutputChannels() == 2
               && synth->getPluginDescription().isInstrument && synth->getParameters().size() == 8,
           "instrument: the basic synth is made from its description, is an instrument that takes MIDI, has no audio input and a stereo output, and eight parameters");

    if (synth == nullptr)
        return;

    {
        // Bare, A4 for a second in a second and a half: a sine at exactly the velocity, from the start of its cycle, silent after its hold.
        setBare (*synth);
        juce::AudioBuffer<float> out;
        const auto err = render (*synth, together ({ 69 }, 1.0, 1.5, 1.0f), rate, 2, out);
        const auto amp = out.getNumSamples() == 72000 ? amplitude (out, 0, 440.0, 0, 48000) : 0.0;
        auto worst = 0.0f;

        for (int i = 0; i < 480 && out.getNumSamples() == 72000; ++i)
            worst = juce::jmax (worst, std::abs (out.getSample (0, i) - (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / rate)));

        check (err.isEmpty() && out.getNumChannels() == 2 && out.getNumSamples() == 72000 && std::abs (amp - 1.0) < 1e-3 && worst < 1e-5f
                   && out.getMagnitude (0, 48000, 24000) == 0.0f && out.getMagnitude (0, 0, 48000) > 0.99f && channelsSame (out),
               "instrument: the bare synth plays A4 as a sine at exactly its velocity, from its cycle's start, silent once the hold ends, the same on both channels");

        // Velocity and level scale it; the release rings on after the hold and then stops.
        setPlain (*synth, "level", -6.0f);
        setPlain (*synth, "release", 100.0f);
        const auto err2 = render (*synth, together ({ 69 }, 0.5, 1.0, 0.5f), rate, 1, out);
        const auto amp2 = out.getNumSamples() == 48000 ? amplitude (out, 0, 440.0, 0, 24000) : 0.0;
        const auto expected2 = 0.5 * juce::Decibels::decibelsToGain (-6.0);
        check (err2.isEmpty() && out.getNumChannels() == 1 && std::abs (amp2 - expected2) < 2e-3
                   && out.getMagnitude (0, 24000, 2400) > 0.05f && out.getMagnitude (0, 24000 + 4800, 48000 - 24000 - 4800) == 0.0f,
               "instrument: velocity and the level scale the sine; a 100 ms release rings past the hold and is silent 100 ms later");

        // A chord's partials and notes in turn.
        setBare (*synth);
        const auto err3 = render (*synth, together ({ 60, 64, 67 }, 1.0, 1.0, 1.0f), rate, 1, out);
        const auto c = amplitude (out, 0, juce::MidiMessage::getMidiNoteInHertz (60), 0, 48000);
        const auto e = amplitude (out, 0, juce::MidiMessage::getMidiNoteInHertz (64), 0, 48000);
        const auto g = amplitude (out, 0, juce::MidiMessage::getMidiNoteInHertz (67), 0, 48000);
        const auto err4 = render (*synth, inTurn ({ 60, 72 }, 0.5, 1.0, 1.0f), rate, 1, out);
        const auto lowFirst = amplitude (out, 0, juce::MidiMessage::getMidiNoteInHertz (60), 0, 24000);
        const auto highFirst = amplitude (out, 0, juce::MidiMessage::getMidiNoteInHertz (72), 0, 24000);
        const auto lowSecond = amplitude (out, 0, juce::MidiMessage::getMidiNoteInHertz (60), 24000, 24000);
        const auto highSecond = amplitude (out, 0, juce::MidiMessage::getMidiNoteInHertz (72), 24000, 24000);
        check (err3.isEmpty() && err4.isEmpty() && std::abs (c - 1.0) < 0.02 && std::abs (e - 1.0) < 0.02 && std::abs (g - 1.0) < 0.02
                   && lowFirst > 0.95 && highFirst < 0.05 && lowSecond < 0.05 && highSecond > 0.95,
               "instrument: a chord plays each of its notes at full amplitude; notes in turn play one after another");

        // Sixteen voices: a seventeenth note takes the oldest voice over.
        synth->prepareToPlay (rate, 512);
        juce::AudioBuffer<float> block (2, 512);
        juce::MidiBuffer midi;

        for (int i = 0; i < 17; ++i)
            midi.addEvent (juce::MidiMessage::noteOn (1, 40 + i, 1.0f), i);

        synth->processBlock (block, midi);
        const auto sounding = synth->getNumSounding();
        midi.clear();
        midi.addEvent (juce::MidiMessage::noteOff (1, 40), 0);   // the first note's voice was taken: nothing to end
        synth->processBlock (block, midi);
        const auto stillSounding = synth->getNumSounding();
        midi.clear();
        midi.addEvent (juce::MidiMessage::allNotesOff (1), 0);
        synth->processBlock (block, midi);
        midi.clear();
        synth->processBlock (block, midi);
        check (sounding == 16 && stillSounding == 16 && synth->getNumSounding() == 0,
               "instrument: the synth plays sixteen voices, the oldest taken over by a seventeenth note, and All Notes Off ends them all");

        // Its state round-trips, and the sawtooth and square keep their peaks near full scale (no aliasing blow-up).
        setPlain (*synth, "wave", 3);
        setPlain (*synth, "cutoff", 1234.0f);
        juce::MemoryBlock state;
        synth->getStateInformation (state);
        auto second = engine.pluginFormatManager.createPluginInstance (synthDescription, rate, 512, error);
        second->setStateInformation (state.getData(), (int) state.getSize());
        setBare (*synth);
        setPlain (*synth, "wave", 2);
        const auto err5 = render (*synth, together ({ 57 }, 1.0, 1.0, 1.0f), rate, 1, out);
        const auto sawPeak = out.getMagnitude (0, 0, out.getNumSamples());
        const auto sawFundamental = amplitude (out, 0, juce::MidiMessage::getMidiNoteInHertz (57), 0, 48000);
        setPlain (*synth, "wave", 3);
        const auto err6 = render (*synth, together ({ 57 }, 1.0, 1.0, 1.0f), rate, 1, out);
        const auto squarePeak = out.getMagnitude (0, 0, out.getNumSamples());
        const auto squareFundamental = amplitude (out, 0, juce::MidiMessage::getMidiNoteInHertz (57), 0, 48000);
        check (second != nullptr && plainOf (*second, "wave") == 3.0f && std::abs (plainOf (*second, "cutoff") - 1234.0f) < 0.5f
                   && err5.isEmpty() && err6.isEmpty() && sawPeak > 0.9f && sawPeak < 1.2f && std::abs (sawFundamental - 2.0 / juce::MathConstants<double>::pi) < 0.03
                   && squarePeak > 0.9f && squarePeak < 1.3f && std::abs (squareFundamental - 4.0 / juce::MathConstants<double>::pi) < 0.04,
               "instrument: the synth's state restores its wave and cutoff; a sawtooth's and a square's fundamentals are 2/pi and 4/pi of their peaks");
    }

    // Rendering through any instrument: latency dropped, a mono output on every channel, the notes delivered in order, stopping.
    {
        TestPlugins::ClickInstrument click (100);
        juce::AudioBuffer<float> out;
        const auto err = render (click, inTurn ({ 60, 62 }, 0.01, 0.05, 1.0f), rate, 2, out);   // notes at frames 0 and 480
        check (err.isEmpty() && out.getNumChannels() == 2 && out.getNumSamples() == 2400
                   && out.getSample (0, 0) == 1.0f && out.getSample (1, 0) == 1.0f && out.getSample (0, 480) == 1.0f && out.getSample (1, 480) == 1.0f
                   && countNonZero (out, 0) == 2 && countNonZero (out, 1) == 2
                   && click.notes == std::vector<std::pair<int, juce::int64>> { { 60, 0 }, { 62, 480 } }
                   && click.offs == std::vector<std::pair<int, juce::int64>> { { 60, 480 }, { 62, 960 } }
                   && click.prepared == 1 && click.released == 1 && juce::exactlyEqual (click.preparedRate, rate) && click.wasNonRealtime,
               "instrument: an instrument's latency is dropped so its sound starts where the note does, a mono output plays on every channel, "
               "the notes arrive at their frames, and it is prepared offline at the rate and released");

        TestPlugins::ClickInstrument late (3000);   // longer than the phrase and than a block
        const auto err2 = render (late, together ({ 60 }, 0.01, 0.02, 1.0f), rate, 1, out);
        check (err2.isEmpty() && out.getNumSamples() == 960 && out.getSample (0, 0) == 1.0f && countNonZero (out, 0) == 1,
               "instrument: a latency longer than the sound itself is still dropped");

        TestPlugins::ClickInstrument stopped (0);
        const auto err3 = render (stopped, together ({ 60 }, 1.0, 2.0, 1.0f), rate, 2, out, [] (float) { return false; });
        check (err3.isNotEmpty() && out.getNumSamples() == 0 && stopped.released == 1,
               "instrument: a render stopped by its progress callback gives nothing and releases the instrument");

        const auto err4 = render (click, together ({ 60 }, 1.0, 0.0, 1.0f), rate, 2, out);
        check (err4.isNotEmpty() && out.getNumSamples() == 0, "instrument: a phrase that can't be played is refused");
    }

    // A fresh instance from a description and a state, so the one with its window open isn't touched.
    {
        setBare (*synth);
        setPlain (*synth, "wave", 1);
        setPlain (*synth, "level", -3.0f);
        juce::MemoryBlock state;
        synth->getStateInformation (state);
        juce::AudioBuffer<float> fromInstance, fromDescription, fromDefaults;
        const auto phrase = together ({ 64 }, 0.5, 0.75, 0.9f);
        const auto err1 = render (*synth, phrase, rate, 2, fromInstance);
        const auto err2 = render (engine.pluginFormatManager, synthDescription, state, phrase, rate, 2, fromDescription);
        const auto err3 = render (engine.pluginFormatManager, synthDescription, {}, phrase, rate, 2, fromDefaults);
        auto same = fromInstance.getNumSamples() == fromDescription.getNumSamples() && fromInstance.getNumSamples() > 0;

        for (int i = 0; same && i < fromInstance.getNumSamples(); ++i)
            same = juce::exactlyEqual (fromInstance.getSample (0, i), fromDescription.getSample (0, i));

        auto differs = false;

        for (int i = 0; ! differs && i < juce::jmin (fromDefaults.getNumSamples(), fromInstance.getNumSamples()); ++i)
            differs = ! juce::exactlyEqual (fromDefaults.getSample (0, i), fromInstance.getSample (0, i));

        juce::PluginDescription unknown = synthDescription;
        unknown.fileOrIdentifier = "spectrablade:no-such-instrument";
        juce::AudioBuffer<float> none;
        const auto err4 = render (engine.pluginFormatManager, unknown, {}, phrase, rate, 2, none);
        check (err1.isEmpty() && err2.isEmpty() && err3.isEmpty() && same && differs && err4.isNotEmpty() && none.getNumSamples() == 0,
               "instrument: a fresh instance from the description and state plays exactly what the instance plays, the defaults play otherwise, "
               "and an unknown instrument is an error");
    }
}
