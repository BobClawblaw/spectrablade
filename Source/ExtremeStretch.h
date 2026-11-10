#pragma once

#include <JuceHeader.h>
#include "AudioEdits.h"

/** Stretches audio to many times its length (up to a thousand) by smearing it: each window of the
    sound keeps its spectrum and loses its timing, so a texture or a chord becomes a pad that holds
    its pitch and colour while attacks, words and rhythm melt away. The method is the one Paul
    Nasca published for extreme stretching: windows of the input, taken at a pace slower than they
    are laid down, with their spectra kept and their phases thrown away. Written here from that
    description. */
namespace ExtremeStretch
{
    struct Settings
    {
        double factor = 8.0;          // times longer, shortestFactor to longestFactor
        double windowSeconds = 0.25;  // the window smeared, shortestWindow to longestWindow
    };

    inline constexpr double shortestFactor = 1.0, longestFactor = 1000.0;
    inline constexpr double shortestWindow = 0.05, longestWindow = 2.0;

    bool isValid (const Settings&);

    /** The window's length in samples at a rate: the power of two nearest the seconds asked. */
    int windowSize (double windowSeconds, double sampleRate);

    /** The input stretched to round (numSamples * factor) samples. Every channel takes the same
        random phases, so channels alike stay alike. progress hears how far it is (0 to 1); when
        it returns false, stretching stops and an empty buffer comes back. */
    juce::AudioBuffer<float> stretch (const juce::AudioBuffer<float>& input, const Settings&, double sampleRate,
                                      const std::function<bool (float)>& progress = {});

    /** The audio with [start, end) stretched, the rest moving with it, its joins with the audio
        either side crossfaded over TimeStretch::joinSeconds. Null for an empty range, or when
        progress stops it. */
    AudioDocument::Snapshot stretchRange (const AudioSnapshot&, juce::int64 start, juce::int64 end, const Settings&, double sampleRate,
                                          const std::function<bool (float)>& progress = {});
}
