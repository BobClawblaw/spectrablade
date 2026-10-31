#pragma once

#include "AudioDocument.h"

/** Noise reduction from a noise print, by short-time Fourier analysis and resynthesis
    (4096-point frames, 75% overlap, square-root Hann windows, as in SpectralEdit).

    The print is the average power of each frequency bin over a stretch of noise alone. Each
    frame of the audio then gets a Wiener gain per bin from its signal-to-noise ratio, with the
    "decision-directed" estimate of Ephraim and Malah smoothing that ratio over time so the
    leftover noise does not warble. The gain never goes below the chosen reduction, so 0 dB
    leaves the audio as it was. */
namespace NoiseReduction
{
    constexpr int frameSize = 4096;

    struct Print
    {
        double sampleRate = 0.0;
        std::vector<std::vector<float>> power;   // per channel, frameSize / 2 + 1 bins
    };

    struct Settings
    {
        float reductionDb = 12.0f;   // how far noise is lowered, at most
        float thresholdDb = 3.0f;    // how far above the print a bin must be to count as signal
    };

    /** The print of frames [start, end) of every channel, or nothing if that is shorter than a frame. */
    std::optional<Print> capture (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate);

    /** Reduces the printed noise over frames [start, end), easing in and out over about a frame at
        the edges. A print with another channel count is averaged over its channels. progress gets
        0..1 and returns false to cancel; then this returns nullptr. */
    AudioDocument::Snapshot reduce (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate,
                                    const Print&, Settings, const std::function<bool (float)>& progress = {});
}
