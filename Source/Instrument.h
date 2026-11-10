#pragma once

#include <JuceHeader.h>

/** Playing an instrument plug-in (a synthesizer, built in or a scanned VST3 or Audio Unit
    instrument) offline: notes rendered into audio, as Generate from Instrument puts them in a file. */
namespace Instrument
{
    /** A note: its MIDI pitch (60 is middle C, C4), when it starts and how long it is held, in
        seconds, and its velocity from 0 to 1. */
    struct Note
    {
        int pitch = 60;
        double start = 0.0, hold = 1.0;
        float velocity = 0.8f;

        bool operator== (const Note& o) const noexcept
        {
            return pitch == o.pitch && juce::exactlyEqual (start, o.start) && juce::exactlyEqual (hold, o.hold) && juce::exactlyEqual (velocity, o.velocity);
        }
    };

    /** Notes with the length of the audio they are rendered into: what rings on past it is cut off. */
    struct Phrase
    {
        std::vector<Note> notes;
        double lengthSeconds = 2.0;
    };

    /** The longest phrase rendered, in seconds. */
    constexpr double longestSeconds = 600.0;

    /** The instruments the app knows: the built-in ones first, then every scanned VST3 or Audio
        Unit instrument, by name (then format). */
    juce::Array<juce::PluginDescription> known (const juce::KnownPluginList&);

    /** An instrument's name with where it is from: "Basic Synth (built in)", "Name (Audio Units, Maker)". */
    juce::String title (const juce::PluginDescription&);

    /** Pitches 0 to 127, velocities 0 to 1, starts from 0, holds above 0, and the length above 0
        and up to longestSeconds. */
    bool isValid (const Phrase&);

    /** A pitch from its name (C4, F#3, Bb2, c4, with the octave from -1 to 9: C4 is 60, C-1 is 0,
        G9 is 127) or its MIDI number (0 to 127), spaces around it ignored. Nothing for anything else. */
    std::optional<int> parsePitch (const juce::String&);

    /** Pitches from names or numbers separated by commas or spaces; nothing when any isn't one, or
        there are none. */
    std::optional<std::vector<int>> parsePitches (const juce::String&);

    /** A pitch's name (C4, F#3): sharps, never flats. */
    juce::String pitchName (int pitch);

    /** The pitches played together from the start, each held for `hold` seconds. */
    Phrase together (const std::vector<int>& pitches, double hold, double lengthSeconds, float velocity);

    /** The pitches one after another, each held for `hold` seconds and starting as the one before ends. */
    Phrase inTurn (const std::vector<int>& pitches, double hold, double lengthSeconds, float velocity);

    /** What the phrase sends, at sample positions: each note's note-on at its start and note-off
        where its hold ends, on channel 1. At one position, note-offs come before note-ons, so a note
        taking over from another doesn't lose its voice. Notes starting at or past the length are left out. */
    juce::MidiMessageSequence midiOf (const Phrase&, double sampleRate);

    /** Renders the phrase through the instrument into `out`: numChannels channels, the phrase's
        length in frames (rounded up), replacing what was there. The instrument is prepared for
        offline work at this rate, fed the notes block by block, and its latency is dropped from the
        start, so the first note's sound begins where the note does. A mono instrument plays on every
        channel; a stereo one's left and right go to channels 0 and 1, then 2 and 3, and so on.
        progress gets 0 to 1 and returns false to stop (then out is empty). Returns an error (no audio
        outputs, or stopped), or empty. */
    juce::String render (juce::AudioPluginInstance&, const Phrase&, double sampleRate, int numChannels,
                         juce::AudioBuffer<float>& out, const std::function<bool (float)>& progress = {});

    /** A fresh instance of the instrument, given the state (empty for its defaults) and run as above,
        so the one on the bench keeps its window. Call from a background thread: AUv3 instruments
        must be created off the message thread. */
    juce::String render (juce::AudioPluginFormatManager&, const juce::PluginDescription&, const juce::MemoryBlock& state,
                         const Phrase&, double sampleRate, int numChannels,
                         juce::AudioBuffer<float>& out, const std::function<bool (float)>& progress = {});
}
