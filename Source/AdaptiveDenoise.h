#pragma once

#include "AudioDocument.h"

/** Voice de-noise, adaptive: noise reduced with no noise print, the noise tracked as it changes.
    The audio is analysed in 4,096-point frames at 75 % overlap (square-root Hann windows, as the
    noise reduction). Each bin's noise is tracked from the least its smoothed power has been over
    the last tracking span (what is always there, even under speech, since speech leaves every bin
    for moments), kept in four sliding sub-windows: the noise is the mean of the frames within 6 dB
    of that least (the frames of noise alone), so it is not the minimum's underestimate. Noise that
    rises is followed within a sub-window, but only where the rise is broad (most bins from 100 Hz
    to 8 kHz rising together), since speech starting raises its own bins alone; noise that falls is
    followed as the sub-windows holding the old level roll off. Only frames wholly within the audio
    there is are tracked (a frame over the silence past an edge would read as noise falling to
    nothing), and the frame before the range is read for the tracking to settle on. Each frame then gets the noise reduction's Wiener gain per bin from its
    signal-to-noise ratio, smoothed over time the decision-directed way, never below the reduction;
    0 dB leaves the audio as it was. */
namespace AdaptiveDenoise
{
    struct Settings
    {
        float reductionDb = 12.0f;    // 0 to 40: how far noise is lowered, at most
        float thresholdDb = 3.0f;     // 0 to 12: how far above the tracked noise a bin must be to count as signal
        float trackingSeconds = 1.5f; // 0.5 to 5: the span the noise is taken as the least over
    };

    bool isValid (const Settings&);

    /** What a reduction found: the share of the range's frames (0 to 1) whose power from 100 Hz to
        8 kHz stood 6 dB or more over the tracked noise in some channel (sound clear of the noise, by
        the tracker's lights), and how far the range's typical block came down (the median over its
        4,096-frame blocks with sound in them, dB, negative or 0). A share near nothing with the
        typical block down by about the reduction means nothing stood clear: the range
        is noise alone, or a sound that never pauses (a room full of voices) was itself taken for
        the noise and lowered as a whole. */
    struct Report { float signalShare = 0.0f; float loweredDb = 0.0f; };

    /** Reduces the tracked noise over frames [start, end), easing in and out over a frame at the
        edges. progress gets 0..1 and returns false to cancel; then this returns nullptr. Nullptr
        too for settings that aren't valid or an empty range. The report, when wanted, is filled
        for a result. */
    AudioDocument::Snapshot reduce (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, Settings,
                                    const std::function<bool (float)>& progress = {}, Report* report = nullptr);
}
