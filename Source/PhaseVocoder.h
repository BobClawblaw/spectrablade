#pragma once

#include <JuceHeader.h>

/** SpectraBlade's own time stretch, alongside Signalsmith Stretch: a phase vocoder that keeps
    attacks as they were.

    - Frames of about 85 ms (Hann windowed, four to a frame's length) are taken from the input
      where a time map says and laid down evenly in the output.
    - Each frequency's speed comes from two transforms a hop apart, so it holds however far apart
      the frames are taken. All channels share one phase turn per frequency, so they keep their
      phase relations exactly; each frequency follows the turn of the peak it sits under (Laroche
      and Dolson's phase locking, 1999), so a note's partials move together.
    - Attacks are found first (rising spectral flux, against the median round it, so quiet hits
      between loud ones count), each placed where its rise starts. Each stays where the stretch
      puts it: up to half a frame either side of it plays at its own speed with the phases set back
      to the input's (attacks too close for that share the room between them), and is copied in
      from the input, so the attack comes out as it went in. Frames outside those stretches hold
      none of an attack's other side. The audio between stretches to make up the length. */
namespace PhaseVocoder
{
    /** Where attacks start in the input (frames in [from, to)), in order. */
    std::vector<int> onsets (const juce::AudioBuffer<float>& input, int from, int to, double sampleRate);

    /** The input stretched to round (numSamples * ratio) samples (ratio from 0.25 to 4), its ends
        carried on as for TimeStretch::stretch. progress hears how far it is (0 to 1); when it
        returns false, stretching stops and an empty buffer comes back. */
    juce::AudioBuffer<float> stretch (const juce::AudioBuffer<float>& input, double ratio, double sampleRate,
                                      const std::function<bool (float)>& progress = {});
}
