#pragma once

#include "AudioDocument.h"
#include "FadeShape.h"

/** Copied or cut audio. Holds references to blocks, so copying is free. */
struct AudioClip
{
    int numChannels = 0;
    double sampleRate = 0.0;
    std::vector<AudioSnapshot::Segment> segments;

    juce::int64 getNumFrames() const;
    bool isEmpty() const                          { return getNumFrames() == 0; }
};

/** Edit operations. Each returns the new snapshot; the source is never modified.
    Ranges are document frames [start, end). */
namespace AudioEdits
{
    using Snapshot = AudioDocument::Snapshot;

    Snapshot fromBuffer (juce::AudioBuffer<float>&& buffer);

    AudioClip copy (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate);
    Snapshot remove (const AudioSnapshot&, juce::int64 start, juce::int64 end);
    Snapshot crop (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** Replaces [start, end) with the clip (an insert when start == end).
        A clip with a different channel count is mixed down or spread to fit. */
    Snapshot replaceWithClip (const AudioSnapshot&, juce::int64 start, juce::int64 end, const AudioClip&);

    /** Adds the clip into the audio from frame `at` on (fitted to its channels as replaceWithClip
        does), lengthening the file when the clip runs past its end. Nothing moves; nullptr when
        the clip is too long to mix in one pass. */
    Snapshot mixClip (const AudioSnapshot&, juce::int64 at, const AudioClip&);

    /** Reads [start, end), runs process on it in place and puts the result back. */
    Snapshot processRange (const AudioSnapshot&, juce::int64 start, juce::int64 end,
                           const std::function<void (juce::AudioBuffer<float>&)>& process);

    Snapshot gain (const AudioSnapshot&, juce::int64 start, juce::int64 end, float decibels);
    Snapshot silence (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** Puts frames of silence in at a frame, moving what follows later. frames must fit in an int. */
    Snapshot insertSilence (const AudioSnapshot&, juce::int64 at, juce::int64 frames);
    Snapshot invert (const AudioSnapshot&, juce::int64 start, juce::int64 end);
    Snapshot reverse (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** Takes each channel's average level over [start, end) out of it, so it sits centred on 0. */
    Snapshot removeDcOffset (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** Moves a two-channel range's balance, as a session track's pan does a stereo clip: pan is
        -1 (left) to +1 (right), and the side moved away from is lowered. Other files are unchanged. */
    Snapshot balance (const AudioSnapshot&, juce::int64 start, juce::int64 end, float pan);

    /** Exchanges the first two channels over [start, end); with fewer than two it changes nothing. */
    Snapshot swapChannels (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** Fades [start, end) up from silence, or down to it, along the shape: the first frame of a
        fade in is silent and its last at full level, and the other way round for a fade out. */
    Snapshot fadeIn (const AudioSnapshot&, juce::int64 start, juce::int64 end, FadeShape = FadeShape::sCurve);
    Snapshot fadeOut (const AudioSnapshot&, juce::int64 start, juce::int64 end, FadeShape = FadeShape::sCurve);

    /** Largest absolute sample value in [start, end) across all channels. */
    float peakLevel (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** The span from the first frame to the last whose level on any channel is above threshold
        (a linear gain), or nothing when no frame is. */
    std::optional<juce::Range<juce::int64>> audibleRange (const AudioSnapshot&, float threshold);

    /** Scales [start, end) so its peak sits at targetDecibels. Returns nullptr for silence. */
    Snapshot normalize (const AudioSnapshot&, juce::int64 start, juce::int64 end, float targetDecibels);

    /** The whole file with numChannels channels, fitted as matchChannels does. */
    Snapshot convertChannels (const AudioSnapshot&, int numChannels);

    /** Fits a buffer to a channel count: mono is copied to every channel, more channels
        than needed are averaged into mono or truncated. */
    juce::AudioBuffer<float> matchChannels (const juce::AudioBuffer<float>&, int numChannels);
}
