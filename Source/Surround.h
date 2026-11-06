#pragma once

#include <JuceHeader.h>

/** 5.1 surround files: six channels in the order WAV, FLAC and most tools use (L R C LFE Ls Rs).

    Folding down to stereo uses the ITU-R BS.775 gains: each side gets its own front and surround
    channel, and the centre and that surround at -3 dB (0.7071); the LFE is left out. The result is
    not scaled down, so a loud mix can pass 0 dBFS. Mono is the average of the two folded sides. */
namespace Surround
{
    constexpr int fiveOneChannels = 6;
    constexpr float foldGain = 0.70710678f;   // -3 dB

    /** The most channels played out of their own interface outputs (7.1). */
    constexpr int maxPlayedChannels = 8;

    /** A channel's short name: L and R for stereo, L R C LFE Ls Rs for 5.1, otherwise its number
        from 1 ("" for mono). */
    juce::String channelName (int numChannels, int channel);

    /** Folds numFrames of 5.1 (six channel pointers in file order) down to stereo. */
    void foldDown (const float* const* fiveOne, float* left, float* right, int numFrames);

    /** The interface outputs (counted from 0) that a file's channels past the first two play out of,
        when it plays to separate outputs: the outputs after the output pair, one each, for up to
        maxPlayedChannels channels. Nothing for mono or stereo, or when not playing to separate outputs. */
    juce::BigInteger extraOutputs (int numChannels, int outputPairFirstChannel, bool separateOutputs);

    /** Files to put side by side as one file's channels, in order: six whose names end in the 5.1
        channel names (after a space, _, - or ., any case) go in 5.1 order; otherwise name order. */
    juce::Array<juce::File> orderForCombining (juce::Array<juce::File>);

    /** The name for a file holding one channel of another: "Mix L.wav" for the left of "Mix.wav"
        (by channelName; mono or numbered channels get their number). */
    juce::String channelFileName (const juce::String& stem, int numChannels, int channel);
}
