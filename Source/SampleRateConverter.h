#pragma once

#include "AudioEdits.h"

/** Offline sample-rate conversion with a Kaiser-windowed sinc filter.

    The passband is flat to 45.5% of the lower of the two rates (20.06 kHz at 44.1 kHz) and
    everything from half that rate up is attenuated by at least 140 dB, so nothing aliases.
    Rates whose ratio reduces to at most 4096 phases (every common pair: 44.1 <-> 48 kHz is
    160/147) use exact polyphase coefficients; other ratios interpolate between 4096 phases.
    Filter output is accumulated in double precision. The filter is linear phase and centred,
    so converted audio does not move in time. */
namespace SampleRateConverter
{
    /** Frames produced from inputFrames: inputFrames * outRate / inRate, rounded up. */
    juce::int64 getOutputLength (juce::int64 inputFrames, double inRate, double outRate);

    /** Converts every channel of input into output (resized to fit). Runs on several threads.
        progress gets 0..1 and returns false to cancel; then this returns false. */
    bool convert (const juce::AudioBuffer<float>& input, double inRate, double outRate,
                  juce::AudioBuffer<float>& output, const std::function<bool (float)>& progress = {});

    /** Converts a snapshot. Returns nullptr when cancelled. */
    AudioDocument::Snapshot convert (const AudioSnapshot&, double inRate, double outRate,
                                     const std::function<bool (float)>& progress = {});

    /** Converts a clip to outRate. Returns an empty clip when cancelled. */
    AudioClip convert (const AudioClip&, double outRate, const std::function<bool (float)>& progress = {});

    /** Marker positions at the new rate. */
    Markers convert (const Markers&, double inRate, double outRate);
}
