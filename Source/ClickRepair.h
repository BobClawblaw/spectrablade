#pragma once

#include "AudioDocument.h"

/** Finds clicks and pops and rebuilds the damaged samples from the audio around them.

    Detection fits an autoregressive model (order 32) to each block and predicts every sample
    from the ones before it and, separately, from the ones after it. A click is where both
    predictions miss by more than the threshold above the block's typical error (a robust
    median estimate), which pins it to the damaged samples themselves. Each click, with a few
    samples of margin, is then filled by least-squares autoregressive interpolation from the
    audio either side (Janssen, Veldhuis and Vries; Vaseghi and Rayner). No other sample changes. */
namespace ClickRepair
{
    struct Settings
    {
        float thresholdDb = 18.0f;    // how far a prediction miss must stand above the background
        float maxLengthMs = 3.0f;     // longer events are left alone and counted as skipped
        bool voice = false;           // speech: its own pulses are not clicks (the error's spread is read off its loudest twentieth)
    };

    struct Result
    {
        AudioDocument::Snapshot snapshot;   // nullptr when nothing was repaired
        int repaired = 0;                   // clicks, counted once however many channels they hit
        int skipped = 0;                    // per channel: too long, or too near the file's ends
    };

    /** Repairs frames [start, end) of every channel. progress gets 0..1 and returns false to
        cancel; then this returns nothing. */
    std::optional<Result> repair (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate,
                                  Settings, const std::function<bool (float)>& progress = {});
}
