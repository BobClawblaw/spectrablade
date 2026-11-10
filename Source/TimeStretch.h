#pragma once

#include <JuceHeader.h>
#include "AudioEdits.h"

/** Changes the length of audio without changing its pitch, and its pitch without changing its
    length, with Signalsmith Stretch (MIT licence; see the README's Licensing): a phase vocoder that
    keeps each partial's phase moving as it should, so chords stay steady and attacks are neither
    doubled nor dropped. */
namespace TimeStretch
{
    /** How to stretch: Signalsmith Stretch, or SpectraBlade's own phase vocoder (PhaseVocoder.h),
        which keeps attacks as they were. */
    enum class Method { signalsmith, spectrablade };

    /** Shortest and longest lengths, as a fraction of the original. */
    inline constexpr double shortest = 0.5, longest = 2.0;

    /** The input stretched to round (numSamples * ratio) samples (ratio within shortest to
        longest), every channel cut at the same places so they stay in step. progress hears how
        far it is (0 to 1) as it goes; when it returns false, stretching stops and an empty
        buffer comes back. */
    juce::AudioBuffer<float> stretch (const juce::AudioBuffer<float>& input, double ratio, double sampleRate,
                                      const std::function<bool (float)>& progress = {}, Method = Method::signalsmith);

    /** Semitones a pitch shift can go up or down: as far as stretching can go. */
    inline constexpr double widestShift = 12.0;

    /** The input at a pitch `semitones` higher (or lower, when negative; within widestShift),
        just as long. progress as for stretch. With SpectraBlade's method, stretched by the pitch
        ratio, then resampled back to its length. */
    juce::AudioBuffer<float> shiftPitch (const juce::AudioBuffer<float>& input, double semitones, double sampleRate,
                                         const std::function<bool (float)>& progress = {}, Method = Method::signalsmith);

    /** How long the joins are crossfaded where a processed range meets the audio around it. */
    inline constexpr double joinSeconds = 0.005;

    /** [start, end) of the audio, processed with up to a quarter second of the audio either side
        of it (so what comes out at its edges carries on from what is there), that part cut out
        (`ratio` times its length, process mapping every input position to `ratio` times it),
        put back in its place with its joins blended over joinSeconds; null for an empty range, a
        range too long to hold, or when processing stops (an empty result). */
    AudioDocument::Snapshot processRange (const AudioSnapshot&, juce::int64 start, juce::int64 end, double ratio, double sampleRate,
                                          const std::function<juce::AudioBuffer<float> (const juce::AudioBuffer<float>&)>& process);

    /** The audio with [start, end) stretched to `ratio` times its length at the same pitch (the
        rest moving with it), its joins with the audio either side crossfaded over joinSeconds.
        Null for an empty range, or when progress stops it. */
    AudioDocument::Snapshot stretchRange (const AudioSnapshot&, juce::int64 start, juce::int64 end, double ratio, double sampleRate,
                                          const std::function<bool (float)>& progress = {}, Method = Method::signalsmith);

    /** The audio with [start, end) `semitones` higher or lower and just as long, its joins
        crossfaded likewise. Null for an empty range, or when progress stops it. */
    AudioDocument::Snapshot shiftRange (const AudioSnapshot&, juce::int64 start, juce::int64 end, double semitones, double sampleRate,
                                        const std::function<bool (float)>& progress = {}, Method = Method::signalsmith);
}
