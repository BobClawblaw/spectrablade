#pragma once

#include "AudioDocument.h"

/** Wind, rustle and bleed reduction for a recording of speech: three jobs that share one look at
    the audio. The level is measured in 10 ms blocks, in the whole, in the lows (below the corner,
    the multiband split) and in the highs (above 1 kHz); every 20 ms a 4,096-point frame is judged
    for voicing as Dialogue Isolation judges it, and a block is voiced when the frame over it is.
    Wind: the lows lowered while they gust, that is while they stand over their typical level
    (the lows' 30th percentile over five seconds either side) by more than 9 dB in an unvoiced
    block; eased over 50 ms. Rustle: the highs lowered during a burst of them (the highs over their
    floor, the quietest block within three seconds, by more than 9 dB) that is unvoiced, lasts 20
    to 400 ms, and is more than 150 ms from any voiced block (a fricative sits next to a vowel;
    rustle does not); eased over 10 ms. Bleed: everything lowered between the words, that is in
    unvoiced blocks more than the threshold under the speech level (the loudest block within a
    second), more than 100 ms from any voiced block; eased over 30 ms. Each job's reduction at 0
    leaves that job out. */
namespace Spill
{
    struct Settings
    {
        double windReductionDb = 12.0;   // 0 to 40
        double windCornerHz = 150.0;     // 60 to 400
        double rustleReductionDb = 12.0; // 0 to 40
        double bleedReductionDb = 12.0;  // 0 to 40
        double bleedThresholdDb = 20.0;  // 6 to 40: how far under the speech bleed lies
    };

    bool isValid (const Settings&, double sampleRate);

    /** Each 10 ms block's gains from the analysis, in dB: the lows', the highs' and the whole's. */
    struct Analysis
    {
        std::vector<float> windDb, rustleDb, bleedDb;
        int blocks = 0;
    };

    Analysis analyse (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&);

    /** Applies the three over [start, end). progress gets 0..1 and returns false to cancel; then this
        returns nullptr. Nullptr too for settings that aren't valid or a range under a tenth of a second. */
    AudioDocument::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&,
                                   const std::function<bool (float)>& progress = {});
}
