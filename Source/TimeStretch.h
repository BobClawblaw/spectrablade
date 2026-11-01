#pragma once

#include <JuceHeader.h>

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
        longest), every channel cut at the same places so they stay in step. */
    juce::AudioBuffer<float> stretch (const juce::AudioBuffer<float>& input, double ratio, double sampleRate);
}
