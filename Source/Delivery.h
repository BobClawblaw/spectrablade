#pragma once

#include "Loudness.h"
#include "AudioEdits.h"

/** Loudness to a delivery spec: a target integrated loudness with its tolerance and a true-peak
    ceiling, met in one step (gain, and a true-peak limiter where the gain alone would pass the
    ceiling), with a report of what the result measures against each limit. */
namespace Delivery
{
    struct Spec
    {
        juce::String name;
        double targetLufs = -23.0;
        double toleranceLu = 0.5;      // the integrated loudness passes within this either side
        double maxTruePeak = -1.0;     // dBTP
    };

    /** The specs offered: EBU R128 (-23 LUFS +/-0.5 LU, -1 dBTP), ATSC A/85 (-24 LKFS +/-2 dB,
        -2 dBTP), and two common online targets (-16 and -14 LUFS, +/-1 LU, -1 dBTP). */
    const std::vector<Spec>& specs();

    /** One line of a report: what is measured, what the spec asks, and whether it passes. A
        line with an empty `required` is for information and always passes. */
    struct Line
    {
        juce::String what, measured, required;
        bool pass = true;
    };

    /** The integrated loudness and true peak against the spec, then the range, the loudest
        momentary and short-term loudness and the sample peak for information. */
    std::vector<Line> check (const Spec&, const Loudness::Result&);

    struct Delivered
    {
        AudioEdits::Snapshot snapshot;     // null when it couldn't be done (silence, cancelled)
        Loudness::Result before, after;
        double gainDb = 0.0;
        bool limited = false;              // a true-peak limiter was needed
        juce::String error;
    };

    /** Brings [start, end) to the spec: gain to the target, or, where that would take the true
        peak past the ceiling, gain into a true-peak limiter at the ceiling (Dynamics::matchLoudness),
        aiming 0.1 dB under it so the result measures within it. Measures before and after.
        progress gets 0..1 for the measuring and returns false to cancel. */
    Delivered deliver (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Spec&,
                       const std::function<bool (float)>& progress = {});

    /** The report as text: the spec, what was done, and each line with PASS or FAIL (and the
        measurement before, for comparison). */
    juce::String report (const Spec&, const Delivered&, const juce::String& title);

    /** Whether every line passes. */
    bool passes (const Spec&, const Loudness::Result&);

    /** Stems to a spec: one gain for them all, the one that brings their mix to the target, so
        they still add up to the mix as delivered. No limiter, as a limiter on each stem would not
        add up to one on the mix: where the gain takes the mix's true peak past the ceiling, the
        report says the mix would need limiting. */
    struct Stems
    {
        double gainDb = 0.0;
        juce::String report, error;     // error: nothing above the gate to measure
    };

    /** Measures the mix, finds the stems' gain and writes the report (the mix's lines after the
        gain, measured on the mix with it). The mix is left as it was. */
    Stems planStems (const Spec&, const juce::AudioBuffer<float>& mix, double sampleRate, int stems, const juce::String& title);
}
