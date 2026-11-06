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
