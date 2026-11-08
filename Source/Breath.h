#pragma once

#include "AudioDocument.h"

/** Breath control: the breaths between words found and lowered. The level is measured in 10 ms
    blocks (RMS over the channels); the speech level around a block is the loudest block within a
    second either side, the floor the quietest within three seconds either side. A block is a
    breath candidate when it lies more than the threshold under the speech level and more than
    6 dB over the floor, and sounds like air rather than voice: its zero-crossing rate (the crossings
    per second over the channels' mix) is above 1,200, where voiced speech sits well under. A run
    of candidates from 80 to 800 ms long is a breath, and is lowered by the reduction, eased in and
    out over 20 ms. Words, pauses that are only room noise, and anything voiced are left alone. */
namespace Breath
{
    struct Settings
    {
        float thresholdDb = 15.0f;    // 6 to 40: how far under the speech around it a breath lies
        float reductionDb = 12.0f;    // 0 to 40: how far a breath is lowered
    };

    bool isValid (const Settings&);

    struct Found
    {
        juce::int64 start = 0, end = 0;   // frames
    };

    /** The breaths in [start, end). Empty for a range under a tenth of a second. */
    std::vector<Found> find (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&);

    /** Lowers the breaths in [start, end); the audio as it was where none are found. Nullptr for
        settings that aren't valid or a range under a tenth of a second. */
    AudioDocument::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&);
}
