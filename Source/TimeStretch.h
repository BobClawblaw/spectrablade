#pragma once

#include <JuceHeader.h>
#include "AudioEdits.h"

/** Changes the length of audio without changing its pitch, by waveform-similarity overlap-add
    (WSOLA, from Verhelst and Roelands' 1993 paper): Hann-windowed frames are laid down half a
    frame apart in the output, each taken from near where it falls in the input, moved within a
    small tolerance to where it best continues the frame before it. */
namespace TimeStretch
{
    /** Shortest and longest lengths, as a fraction of the original. */
    inline constexpr double shortest = 0.5, longest = 2.0;

    /** Frames of about 40 ms: the power of two nearest that length at the rate. */
    int frameLength (double sampleRate);

    /** The input stretched to round (numSamples * ratio) samples (ratio within shortest to
        longest), every channel cut at the same places so they stay in step. progress hears how
        far it is (0 to 1) as it goes; when it returns false, stretching stops and an empty
        buffer comes back. */
    juce::AudioBuffer<float> stretch (const juce::AudioBuffer<float>& input, double ratio, double sampleRate,
                                      const std::function<bool (float)>& progress = {});

    /** Semitones a pitch shift can go up or down: as far as stretching can go. */
    inline constexpr double widestShift = 12.0;

    /** The input at a pitch `semitones` higher (or lower, when negative; within widestShift),
        just as long: stretched by the pitch ratio, then resampled back to its length. progress
        as for stretch. */
    juce::AudioBuffer<float> shiftPitch (const juce::AudioBuffer<float>& input, double semitones, double sampleRate,
                                         const std::function<bool (float)>& progress = {});

    /** How long the joins are crossfaded where a processed range meets the audio around it. */
    inline constexpr double joinSeconds = 0.005;

    /** The audio with [start, end) stretched to `ratio` times its length at the same pitch (the
        rest moving with it), its joins with the audio either side crossfaded over joinSeconds.
        Null for an empty range, or when progress stops it. */
    AudioDocument::Snapshot stretchRange (const AudioSnapshot&, juce::int64 start, juce::int64 end, double ratio, double sampleRate,
                                          const std::function<bool (float)>& progress = {});

    /** The audio with [start, end) `semitones` higher or lower and just as long, its joins
        crossfaded likewise. Null for an empty range, or when progress stops it. */
    AudioDocument::Snapshot shiftRange (const AudioSnapshot&, juce::int64 start, juce::int64 end, double semitones, double sampleRate,
                                        const std::function<bool (float)>& progress = {});
}
