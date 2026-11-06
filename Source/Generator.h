#pragma once

#include <JuceHeader.h>
#include "AudioEdits.h"

/** Generating audio: tones and noise, put into a file at a point. */
namespace Generator
{
    enum class Wave { sine, square, triangle, sawtooth };

    struct Tone
    {
        Wave wave = Wave::sine;
        double frequency = 1000.0;      // 1 Hz to below half the sample rate
        double endFrequency = 1000.0;   // a sweep when it differs: the frequency rises or falls evenly in octaves
        double levelDb = -12.0;         // the peak, -96 to 0 dBFS
        double fadeMs = 5.0;            // the start and end faded in and out over this, 0 to 1000 ms
    };

    enum class Colour { white, pink, brown };

    struct Noise
    {
        Colour colour = Colour::white;
        double levelDb = -20.0;         // the RMS level, from -96 to maxLevelDb
        double fadeMs = 5.0;
        juce::int64 seed = 1;           // the same seed gives the same noise
    };

    /** Telephone signalling: each character a pair of tones played together.
        DTMF (touch-tone): 0-9, * and #, and A-D, a row tone (697, 770, 852 or 941 Hz) with a column
        tone (1209, 1336, 1477 or 1633 Hz). MF (R1, between exchanges): 0-9, K for KP (start) and S
        for ST (end), two of 700, 900, 1100, 1300, 1500 and 1700 Hz. A comma is a pause as long as
        a tone. */
    enum class Signalling { dtmf, mf };

    struct ToneSequence
    {
        Signalling kind = Signalling::dtmf;
        juce::String characters;        // the digits to play, from 1 to maxCharacters
        double toneMs = 100.0;          // each pair, from 20 to 2000 ms
        double gapMs = 100.0;           // silence between them, from 0 to 2000 ms
        double levelDb = -6.0;          // the pair's peak, -96 to 0 dBFS: each tone at half of it

        static constexpr int maxCharacters = 256;
        static constexpr double rampMs = 2.0;   // each pair rises and falls over this (raised cosine)
    };

    /** The pair of frequencies a character stands for, or nothing for one the kind doesn't have
        (a comma included). */
    std::optional<std::pair<double, double>> frequenciesOf (Signalling, juce::juce_wchar);

    /** Characters all of the kind's (or commas, not only commas), timings and level in range, and the
        highest tone below half the sample rate. */
    bool isValid (const ToneSequence&, double sampleRate);

    /** numChannels channels of the sequence, every channel the same: each pair toneMs long, a gap
        after each but the last. Empty when invalid. */
    juce::AudioBuffer<float> toneSequence (const ToneSequence&, int numChannels, double sampleRate);

    /** The loudest RMS level allowed: white noise peaks 4.8 dB above its RMS at most, so -6 dBFS
        peaks at -1.2; pink and brown peak about 13 dB above it over seconds and more over hours,
        so -20 dBFS leaves them room. */
    double maxLevelDb (Colour);

    bool isValid (const Tone&, double sampleRate);
    bool isValid (const Noise&);

    /** numChannels channels of the tone, frames long; every channel the same. Square and sawtooth
        have their jumps smoothed (polyBLEP), so they alias less than drawn straight. */
    juce::AudioBuffer<float> tone (const Tone&, int numChannels, int frames, double sampleRate);

    /** numChannels channels of noise, frames long, each its own (so stereo noise is wide), each
        scaled to the RMS level exactly. White is flat; pink falls 3 dB an octave (equal power in
        every octave); brown falls 6 dB an octave. */
    juce::AudioBuffer<float> noise (const Noise&, int numChannels, int frames, double sampleRate);

    /** The snapshot with the generated audio put in at `at`, moving what follows on. Null when the
        buffer is empty or its channel count isn't the snapshot's. */
    AudioEdits::Snapshot insert (const AudioSnapshot&, juce::int64 at, const juce::AudioBuffer<float>&);
}
