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

    /** The range widened by margin frames on each side, kept inside [0, total). */
    juce::Range<juce::int64> withMargins (juce::Range<juce::int64>, juce::int64 margin, juce::int64 total);

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

    /** A gain that moves evenly in decibels from fromDecibels at the first frame of [start, end) to
        toDecibels at the last. */
    Snapshot gainRamp (const AudioSnapshot&, juce::int64 start, juce::int64 end, float fromDecibels, float toDecibels);

    /** A tremolo: the gain falls from 1 to 1 - depth and back, as a raised cosine, `rateHz` times a
        second from the first frame of [start, end). Nullptr for a rate that isn't positive, a depth
        outside 0 to 1, or a sample rate that isn't positive. */
    Snapshot tremolo (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, double rateHz, float depth);

    /** An echo inside [start, end): each frame gets the output delayFrames earlier times feedback
        added, so every repeat is feedback times quieter than the one before; nothing rings past the
        range. Nullptr for a delay that isn't positive or a feedback outside 0 to maxEchoFeedback. */
    Snapshot echo (const AudioSnapshot&, juce::int64 start, juce::int64 end, juce::int64 delayFrames, float feedback);
    constexpr float maxEchoFeedback = 0.95f;
    Snapshot silence (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** Silences everything before start and from end on, keeping the length; nullptr when
        [start, end) is empty or already covers the whole audio. */
    Snapshot silenceOutside (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** Puts frames of silence in at a frame, moving what follows later. frames must fit in an int. */
    Snapshot insertSilence (const AudioSnapshot&, juce::int64 at, juce::int64 frames);
    Snapshot invert (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** Flips the polarity of one channel only; nullptr when the audio has no such channel. */
    Snapshot invertChannel (const AudioSnapshot&, juce::int64 start, juce::int64 end, int channel);

    /** Silences one channel only; nullptr when the audio has no such channel. */
    Snapshot silenceChannel (const AudioSnapshot&, juce::int64 start, juce::int64 end, int channel);

    /** Replaces channel `to` with a copy of channel `from`; nullptr when either is missing or they are the same. */
    Snapshot copyChannel (const AudioSnapshot&, juce::int64 start, juce::int64 end, int from, int to);

    /** Redraws [start, end) as a cubic curve from the frame before it to the frame after it, each end
        keeping the slope of the two frames outside it: for a click or a short dropout. Refused
        (nullptr) for an empty range, one longer than maxRepairFrames, or one without two frames on
        each side. */
    Snapshot repair (const AudioSnapshot&, juce::int64 start, juce::int64 end);
    constexpr juce::int64 maxRepairFrames = 4096;
    Snapshot reverse (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** Takes each channel's average level over [start, end) out of it, so it sits centred on 0. */
    Snapshot removeDcOffset (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** Moves a two-channel range's balance, as a session track's pan does a stereo clip: pan is
        -1 (left) to +1 (right), and the side moved away from is lowered. Other files are unchanged. */
    Snapshot balance (const AudioSnapshot&, juce::int64 start, juce::int64 end, float pan);

    /** The same with the pan moving evenly from fromPan at the first frame of [start, end) to toPan
        at the last; nullptr unless the audio has two channels. */
    Snapshot balanceRamp (const AudioSnapshot&, juce::int64 start, juce::int64 end, float fromPan, float toPan);

    /** Scales the side signal (half the difference of the channels) by width, keeping the mid (half
        their sum): 0 makes both channels the mid, 1 leaves the audio as it is (to float rounding),
        2 doubles the side. Nullptr unless the audio has two channels or for a negative width. */
    Snapshot stereoWidth (const AudioSnapshot&, juce::int64 start, juce::int64 end, float width);

    /** Exchanges the first two channels over [start, end); with fewer than two it changes nothing. */
    Snapshot swapChannels (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** Fades [start, end) up from silence, or down to it, along the shape: the first frame of a
        fade in is silent and its last at full level, and the other way round for a fade out. */
    Snapshot fadeIn (const AudioSnapshot&, juce::int64 start, juce::int64 end, FadeShape = FadeShape::sCurve);
    Snapshot fadeOut (const AudioSnapshot&, juce::int64 start, juce::int64 end, FadeShape = FadeShape::sCurve);

    /** Fades in over the first fadeFrames of [start, end) and out over the last, each at most half
        the range; nullptr for an empty range or no fade. */
    Snapshot fadeEdges (const AudioSnapshot&, juce::int64 start, juce::int64 end, juce::int64 fadeFrames, FadeShape = FadeShape::sCurve);

    /** One channel's levels over a range: largest absolute sample, RMS, and average (DC offset). */
    struct ChannelLevels
    {
        float peak = 0.0f;
        double rms = 0.0, mean = 0.0;
    };

    /** The levels of each channel over [start, end), summed in double precision a block at a time. */
    std::vector<ChannelLevels> channelLevels (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** How alike the two channels of stereo audio are over [start, end): the sum of left times right
        over the square root of the sums of their squares, from +1 (the same) through 0 (unrelated)
        to -1 (one the other inverted). Nothing unless the audio is stereo and both channels have sound. */
    std::optional<double> stereoCorrelation (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** The first run of at least minRun frames at or after `from` where some channel is at or
        above level (absolute), as [first, last + 1); nothing when there is none. A frame counts
        when any channel reaches the level. */
    std::optional<juce::Range<juce::int64>> nextClipping (const AudioSnapshot&, juce::int64 from, float level, int minRun);

    /** The first frame of every such run in the audio, in order; at most maxCount of them. */
    std::vector<juce::int64> clippingStarts (const AudioSnapshot&, float level, int minRun, size_t maxCount);

    /** The first run of at least minFrames frames at or after `from` where every channel is at or
        below level (absolute); nothing when there is none. */
    std::optional<juce::Range<juce::int64>> nextQuiet (const AudioSnapshot&, juce::int64 from, float level, juce::int64 minFrames);

    /** Every such quiet stretch with sound on both sides of it (so silence at the start or end of
        the file is left out), in order; at most maxCount of them. */
    std::vector<juce::Range<juce::int64>> pauses (const AudioSnapshot&, float level, juce::int64 minFrames, size_t maxCount);

    /** A noise gate: every quiet stretch (as nextQuiet finds them) inside [start, end) is silenced,
        with a linear fade of fadeFrames at each edge inside the stretch, so the sound around it is
        left bit for bit. Nothing (nullptr) when the range holds no such stretch. */
    Snapshot gateQuiet (const AudioSnapshot&, juce::int64 start, juce::int64 end, float level, juce::int64 minFrames, juce::int64 fadeFrames);

    /** The stretches of sound between quiet stretches of at least minFrames (as nextQuiet finds
        them, silence at the start and end included), in order; at most maxCount of them. */
    std::vector<juce::Range<juce::int64>> soundsBetween (const AudioSnapshot&, float level, juce::int64 minFrames, size_t maxCount);

    /** The next of those stretches of sound: the one after the first quiet stretch at or after
        `from`, or from frame 0, the first sound when the file doesn't start quiet. Nothing when no
        sound follows. */
    std::optional<juce::Range<juce::int64>> nextSound (const AudioSnapshot&, juce::int64 from, float level, juce::int64 minFrames);

    /** The previous of those stretches of sound: the last one ending before `to`, or at the end of
        the file, the last sound when the file doesn't end quiet. Nothing when no sound comes before. */
    std::optional<juce::Range<juce::int64>> previousSound (const AudioSnapshot&, juce::int64 to, float level, juce::int64 minFrames);

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

    /** Moves each marker to the nearest zero crossing in reach and each region's ends as
        toZeroCrossings moves a range's; returns how many changed. */
    int markersToZeroCrossings (Snapshot, Markers&, juce::int64 within);

    /** Makes [start, end) loop smoothly: its last `fade` frames are crossfaded (equal power) into
        the `fade` frames just before start, so its last frame is the one before start and playing
        on from end back to start continues as the audio did. nullptr when fade is not positive,
        more than half the range, or more than the audio before start. */
    Snapshot crossfadeLoop (const AudioSnapshot&, juce::int64 start, juce::int64 end, juce::int64 fade);

    /** Puts `times` more copies of [start, end) right after it, moving what follows later. The
        copies share the audio, so this takes no memory for samples. */
    Snapshot repeatRange (const AudioSnapshot&, juce::int64 start, juce::int64 end, int times);

    /** Replaces [start, end) with the clip played over and over, the last copy cut short, so the
        length stays the same. Refused (nullptr) for an empty range or clip, or when that would take
        more than maxFillCopies copies. */
    Snapshot fillWithClip (const AudioSnapshot&, juce::int64 start, juce::int64 end, const AudioClip&);
    constexpr juce::int64 maxFillCopies = 10000;

    /** Largest absolute sample value in [start, end) across all channels. */
    float peakLevel (const AudioSnapshot&, juce::int64 start, juce::int64 end);

    /** The span from the first frame to the last whose level on any channel is above threshold
        (a linear gain), or nothing when no frame is. */
    std::optional<juce::Range<juce::int64>> audibleRange (const AudioSnapshot&, float threshold);

    /** The same, looking only inside [start, end); the span is in document frames. */
    std::optional<juce::Range<juce::int64>> audibleRange (const AudioSnapshot&, juce::int64 start, juce::int64 end, float threshold);

    /** The markers with each region shrunk to the audible range inside it (as audibleRange finds
        it); a region with nothing above the threshold is left as it is. Returns how many changed. */
    int fitRegionsToSound (const AudioSnapshot&, Markers&, float threshold);

    /** Scales [start, end) so its peak sits at targetDecibels. Returns nullptr for silence. */
    Snapshot normalize (const AudioSnapshot&, juce::int64 start, juce::int64 end, float targetDecibels);

    /** Normalizes each region in the markers to targetDecibels on its own, in time order. A region
        overlapping one already done, or silent, is skipped and counted in `skipped`. Nullptr when
        no region changed. */
    Snapshot normalizeRegions (const AudioSnapshot&, const Markers&, float targetDecibels, int& skipped);

    /** The same with fadeEdges on each region: a fade in over its first fadeFrames and out over its
        last, each at most half the region. */
    Snapshot fadeRegions (const AudioSnapshot&, const Markers&, juce::int64 fadeFrames, FadeShape, int& skipped);

    /** Scales each channel of [start, end) on its own so its peak sits at targetDecibels; silent
        channels are left as they are. Returns nullptr when every channel is silent. */
    Snapshot normalizeChannels (const AudioSnapshot&, juce::int64 start, juce::int64 end, float targetDecibels);

    /** The whole file with numChannels channels, fitted as matchChannels does. */
    Snapshot convertChannels (const AudioSnapshot&, int numChannels);

    /** Fits a buffer to a channel count: mono is copied to every channel, more channels
        than needed are averaged into mono or truncated. */
    juce::AudioBuffer<float> matchChannels (const juce::AudioBuffer<float>&, int numChannels);
}
