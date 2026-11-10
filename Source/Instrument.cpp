#include "Instrument.h"

#include "BuiltInEffects.h"
#include "PluginMenu.h"

namespace Instrument
{
    juce::Array<juce::PluginDescription> known (const juce::KnownPluginList& list)
    {
        juce::Array<juce::PluginDescription> instruments, scanned;

        for (auto& d : BuiltInEffects::getDescriptions())
            if (d.isInstrument)
                instruments.add (d);

        for (auto& t : list.getTypes())
            if (t.isInstrument && t.pluginFormatName != BuiltInEffects::formatName)
                scanned.add (t);

        std::stable_sort (scanned.begin(), scanned.end(), [] (const juce::PluginDescription& a, const juce::PluginDescription& b)
        {
            if (const auto byName = a.name.compareIgnoreCase (b.name); byName != 0)
                return byName < 0;

            return a.pluginFormatName.compareIgnoreCase (b.pluginFormatName) < 0;
        });

        instruments.addArray (scanned);
        return instruments;
    }

    juce::String title (const juce::PluginDescription& d)
    {
        return d.name + " (" + (d.pluginFormatName == BuiltInEffects::formatName ? juce::String ("built in")
                                                                                  : PluginMenu::formatTitle (d.pluginFormatName) + ", " + d.manufacturerName.trim()) + ")";
    }

namespace
{
    constexpr int blockSize = 1024;

    /** A note's letter as semitones above C, or -1. */
    int semitoneOf (juce::juce_wchar letter)
    {
        switch (juce::CharacterFunctions::toUpperCase (letter))
        {
            case 'C': return 0;
            case 'D': return 2;
            case 'E': return 4;
            case 'F': return 5;
            case 'G': return 7;
            case 'A': return 9;
            case 'B': return 11;
            default:  return -1;
        }
    }
}

bool isValid (const Phrase& phrase)
{
    if (! (phrase.lengthSeconds > 0.0) || phrase.lengthSeconds > longestSeconds)
        return false;

    for (auto& n : phrase.notes)
        if (! juce::isPositiveAndBelow (n.pitch, 128) || ! (n.velocity >= 0.0f && n.velocity <= 1.0f)
            || ! (n.start >= 0.0) || ! (n.hold > 0.0) || ! std::isfinite (n.start) || ! std::isfinite (n.hold))
            return false;

    return true;
}

std::optional<int> parsePitch (const juce::String& given)
{
    const auto text = given.trim();

    if (text.isEmpty())
        return std::nullopt;

    if (text.containsOnly ("0123456789"))
    {
        const auto number = text.getIntValue();
        return juce::isPositiveAndBelow (number, 128) ? std::optional<int> (number) : std::nullopt;
    }

    auto rest = text;
    const auto semitone = semitoneOf (rest[0]);

    if (semitone < 0)
        return std::nullopt;

    rest = rest.substring (1);
    int accidental = 0;

    if (rest.startsWithChar ('#'))
    {
        accidental = 1;
        rest = rest.substring (1);
    }
    else if (rest.startsWithChar ('b'))
    {
        accidental = -1;
        rest = rest.substring (1);
    }

    // The octave: -1 to 9, nothing else after it.
    if (rest.isEmpty() || ! (rest.containsOnly ("0123456789") || (rest.startsWithChar ('-') && rest.substring (1).containsOnly ("0123456789") && rest.length() > 1)))
        return std::nullopt;

    const auto octave = rest.getIntValue();

    if (octave < -1 || octave > 9)
        return std::nullopt;

    const auto pitch = (octave + 1) * 12 + semitone + accidental;
    return juce::isPositiveAndBelow (pitch, 128) ? std::optional<int> (pitch) : std::nullopt;
}

std::optional<std::vector<int>> parsePitches (const juce::String& text)
{
    juce::StringArray parts;
    parts.addTokens (text, ", ", {});
    parts.removeEmptyStrings();

    if (parts.isEmpty())
        return std::nullopt;

    std::vector<int> pitches;

    for (auto& part : parts)
    {
        const auto pitch = parsePitch (part);

        if (! pitch.has_value())
            return std::nullopt;

        pitches.push_back (*pitch);
    }

    return pitches;
}

juce::String pitchName (int pitch)
{
    static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const auto p = juce::jlimit (0, 127, pitch);
    return juce::String (names[p % 12]) + juce::String (p / 12 - 1);
}

Phrase together (const std::vector<int>& pitches, double hold, double lengthSeconds, float velocity)
{
    Phrase phrase;
    phrase.lengthSeconds = lengthSeconds;

    for (auto pitch : pitches)
        phrase.notes.push_back ({ pitch, 0.0, hold, velocity });

    return phrase;
}

Phrase inTurn (const std::vector<int>& pitches, double hold, double lengthSeconds, float velocity)
{
    Phrase phrase;
    phrase.lengthSeconds = lengthSeconds;

    for (size_t i = 0; i < pitches.size(); ++i)
        phrase.notes.push_back ({ pitches[i], hold * (double) i, hold, velocity });

    return phrase;
}

juce::MidiMessageSequence midiOf (const Phrase& phrase, double sampleRate)
{
    struct Event
    {
        juce::int64 frame;
        bool on;
        juce::MidiMessage message;
    };

    std::vector<Event> events;
    const auto frames = (juce::int64) std::ceil (phrase.lengthSeconds * sampleRate);

    for (auto& n : phrase.notes)
    {
        const auto start = (juce::int64) std::llround (n.start * sampleRate);

        if (start >= frames)
            continue;

        const auto end = juce::jmax (start + 1, (juce::int64) std::llround ((n.start + n.hold) * sampleRate));
        events.push_back ({ start, true, juce::MidiMessage::noteOn (1, n.pitch, n.velocity) });
        events.push_back ({ end, false, juce::MidiMessage::noteOff (1, n.pitch) });
    }

    std::stable_sort (events.begin(), events.end(), [] (const Event& a, const Event& b)
    {
        return a.frame != b.frame ? a.frame < b.frame : (! a.on && b.on);
    });

    juce::MidiMessageSequence sequence;

    for (auto& e : events)
    {
        e.message.setTimeStamp ((double) e.frame);
        sequence.addEvent (e.message);
    }

    return sequence;
}

juce::String render (juce::AudioPluginInstance& instrument, const Phrase& phrase, double sampleRate, int numChannels,
                     juce::AudioBuffer<float>& out, const std::function<bool (float)>& progress)
{
    out.setSize (0, 0);

    if (! isValid (phrase) || numChannels <= 0)
        return "The notes can't be played.";

    instrument.enableAllBuses();
    instrument.setNonRealtime (true);
    instrument.setRateAndBufferSizeDetails (sampleRate, blockSize);
    instrument.prepareToPlay (sampleRate, blockSize);

    const auto ins = instrument.getTotalNumInputChannels();
    const auto outs = instrument.getTotalNumOutputChannels();

    if (outs <= 0)
    {
        instrument.releaseResources();
        return instrument.getName() + " has no audio outputs.";
    }

    const auto latency = juce::jmax (0, instrument.getLatencySamples());
    const auto frames = (int) std::ceil (phrase.lengthSeconds * sampleRate);
    const auto total = (juce::int64) frames + latency;
    const auto sequence = midiOf (phrase, sampleRate);
    int nextEvent = 0;

    juce::AudioBuffer<float> work (juce::jmax (1, ins, outs), blockSize);
    juce::MidiBuffer midi;
    out.setSize (numChannels, frames);
    out.clear();

    for (juce::int64 pos = 0; pos < total; pos += blockSize)
    {
        const auto n = (int) juce::jmin ((juce::int64) blockSize, total - pos);
        work.clear();
        midi.clear();

        for (; nextEvent < sequence.getNumEvents(); ++nextEvent)
        {
            const auto frame = (juce::int64) sequence.getEventPointer (nextEvent)->message.getTimeStamp();

            if (frame >= pos + n)
                break;

            midi.addEvent (sequence.getEventPointer (nextEvent)->message, (int) (frame - pos));
        }

        juce::AudioBuffer<float> block (work.getArrayOfWritePointers(), work.getNumChannels(), n);
        instrument.processBlock (block, midi);

        // The output lands `latency` frames late: drop that much from the start.
        const auto skip = (int) juce::jmax ((juce::int64) 0, (juce::int64) latency - pos);
        const auto destStart = pos + skip - latency;
        const auto count = (int) juce::jmin ((juce::int64) (n - skip), (juce::int64) frames - destStart);

        if (count > 0)
            for (int ch = 0; ch < numChannels; ++ch)
                out.copyFrom (ch, (int) destStart, work, ch % outs, skip, count);

        if (progress != nullptr && ! progress ((float) ((double) (pos + n) / (double) total)))
        {
            instrument.releaseResources();
            out.setSize (0, 0);
            return "Stopped.";
        }
    }

    instrument.releaseResources();
    return {};
}

juce::String render (juce::AudioPluginFormatManager& formats, const juce::PluginDescription& description, const juce::MemoryBlock& state,
                     const Phrase& phrase, double sampleRate, int numChannels,
                     juce::AudioBuffer<float>& out, const std::function<bool (float)>& progress)
{
    out.setSize (0, 0);
    juce::String error;
    auto instrument = formats.createPluginInstance (description, sampleRate, blockSize, error);

    if (instrument == nullptr)
        return error.isNotEmpty() ? error : description.name + " could not be created.";

    if (! state.isEmpty())
        instrument->setStateInformation (state.getData(), (int) state.getSize());

    return render (*instrument, phrase, sampleRate, numChannels, out, progress);
}
}
