#pragma once

#include "AudioDocument.h"

/** Edits inside a box of time and frequency, by short-time Fourier analysis and resynthesis
    (4096-point frames, 75% overlap, square-root Hann windows on both sides, which add back to
    exactly the input where nothing is changed). */
namespace SpectralEdit
{
    /** Changes the level of frequencies in band (Hz) over frames [start, end) of every channel
        by gainDb (-inf removes them). Edges are tapered over a few bins and about one frame
        of time so the edit does not click or ring. */
    AudioDocument::Snapshot changeBand (const AudioSnapshot&, juce::int64 start, juce::int64 end,
                                        juce::Range<double> band, double sampleRate, float gainDb);
}
