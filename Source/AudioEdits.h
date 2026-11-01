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

    /** One channel's levels over a range: largest absolute sample, RMS, and average (DC offset). */
    struct ChannelLevels
    {
        float peak = 0.0f;
        double rms = 0.0, mean = 0.0;
    };

    /** The levels of each channel over [start, end), summed in double precision a block at a time. */
    std::vector<ChannelLevels> channelLevels (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** The first run of at least minRun frames at or after `from` where some channel is at or
        above level (absolute), as [first, last + 1); nothing when there is none. A frame counts
        when any channel reaches the level. */
    std::optional<juce::Range<juce::int64>> nextClipping (const AudioSnapshot&, juce::int64 from, float level, int minRun);

    /** The first run of at least minFrames frames at or after `from` where every channel is at or
        below level (absolute); nothing when there is none. */
    std::optional<juce::Range<juce::int64>> nextQuiet (const AudioSnapshot&, juce::int64 from, float level, juce::int64 minFrames);

    /** Every such quiet stretch with sound on both sides of it (so silence at the start or end of
        the file is left out), in order; at most maxCount of them. */
    std::vector<juce::Range<juce::int64>> pauses (const AudioSnapshot&, float level, juce::int64 minFrames, size_t maxCount);

    /** The middle frame of each of those pauses. */
    std::vector<juce::int64> quietMiddles (const AudioSnapshot&, float level, juce::int64 minFrames, size_t maxCount);

    /** The parts to cut so that each pause longer than keepFrames is keepFrames long, keeping
        its two ends (half of keepFrames each side, the odd frame after); in order. */
    std::vector<juce::Range<juce::int64>> pauseCuts (const std::vector<juce::Range<juce::int64>>& pauses, juce::int64 keepFrames);

    /** A selection moved later (or earlier) by its own length, stopping at the file's start or end
        with its length kept; the same range when it is empty or already there. */
    juce::Range<juce::int64> stepSelection (juce::Range<juce::int64>, juce::int64 total, bool later);

    /** A selection twice as long (cut at the file's end) or half as long (at least one frame),
        from the same start; the same range when it is empty. */
    juce::Range<juce::int64> resizeSelection (juce::Range<juce::int64>, juce::int64 total, bool longer);

    /** The range with each end moved to the nearest zero crossing of the channels' sum at most
        `within` frames away (as a session's Cut at Zero Crossings finds them); an end with none
        nearby stays, and the range is kept from turning inside out. */
    juce::Range<juce::int64> toZeroCrossings (Snapshot, juce::Range<juce::int64>, juce::int64 within);

    /** Largest absolute sample value in [start, end) across all channels. */
    float peakLevel (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** The span from the first frame to the last whose level on any channel is above threshold
        (a linear gain), or nothing when no frame is. */
    std::optional<juce::Range<juce::int64>> audibleRange (const AudioSnapshot&, float threshold);

    /** The markers with each region shrunk to the audible range inside it (as audibleRange finds
        it); a region with nothing above the threshold is left as it is. Returns how many changed. */
    int fitRegionsToSound (const AudioSnapshot&, Markers&, float threshold);

    /** Scales [start, end) so its peak sits at targetDecibels. Returns nullptr for silence. */
    Snapshot normalize (const AudioSnapshot&, juce::int64 start, juce::int64 end, float targetDecibels);

    /** Scales each channel of [start, end) on its own so its peak sits at targetDecibels; silent
        channels are left as they are. Returns nullptr when every channel is silent. */
    Snapshot normalizeChannels (const AudioSnapshot&, juce::int64 start, juce::int64 end, float targetDecibels);

    /** The whole file with numChannels channels, fitted as matchChannels does. */
    Snapshot convertChannels (const AudioSnapshot&, int numChannels);

    /** Fits a buffer to a channel count: mono is copied to every channel, more channels
        than needed are averaged into mono or truncated. */
    juce::AudioBuffer<float> matchChannels (const juce::AudioBuffer<float>&, int numChannels);
}
