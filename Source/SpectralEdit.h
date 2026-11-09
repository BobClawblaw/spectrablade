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

    /** Changes the level of everything outside band (Hz) over frames [start, end) of every channel
        by gainDb (-inf removes it, leaving the band alone), with the same tapered edges as
        changeBand: what plays when a box is selected in the spectrogram. */
    AudioDocument::Snapshot changeOutsideBand (const AudioSnapshot&, juce::int64 start, juce::int64 end,
                                               juce::Range<double> band, double sampleRate, float gainDb);

    /** A drawn selection of time and frequency: lassos (closed outlines) and brush dabs, together.
        Points are (document frame, octave), the octave being log2 of the frequency in Hz, as the
        spectrogram is drawn on a log scale: a line straight on screen is straight here too. */
    struct Dab
    {
        double frame = 0.0, octave = 0.0;
        double frameRadius = 0.0, octaveRadius = 0.0;   // an ellipse, round on screen at the zoom it was drawn at
    };

    struct Shape
    {
        std::vector<std::vector<juce::Point<double>>> lassos;
        std::vector<Dab> dabs;

        bool isEmpty() const noexcept;

        /** The octave ranges inside the shape at a frame, merged and in order. */
        std::vector<juce::Range<double>> octavesAt (double frame) const;
        bool contains (double frame, double frequency) const;

        /** The frames the shape spans, and its frequency range in Hz. */
        juce::Range<double> frames() const;
        juce::Range<double> frequencies() const;
    };

    /** The transform drawn shapes are edited with: 2048 points (43 ms at 48 kHz), 75% overlap,
        square-root Hann windows on both sides. Frames are centred on multiples of shapeHop. */
    constexpr int shapeSize = 2048;
    constexpr int shapeHop = shapeSize / 4;

    /** How much each frame and bin is in the shape, 0 to 1: a frame counts as in where its
        window's middle half meets the shape, and the edges then fade out over about a frame and
        two bins so the edit doesn't ring. Rows are frames firstFrame onwards, each shapeSize / 2 + 1 bins. */
    struct Mask
    {
        juce::int64 firstFrame = 0;
        std::vector<std::vector<float>> rows;
    };

    Mask maskFor (const Shape&, double sampleRate, juce::int64 numFrames);

    /** Changes the level inside the shape, on every channel, by gainDb (-inf removes it). */
    AudioDocument::Snapshot changeShape (const AudioSnapshot&, const Shape&, double sampleRate, float gainDb);

    /** Changes the level of everything outside the shape by gainDb (-inf removes it, leaving the
        shape alone) over the frames the shape's mask spans (its span in time with its fades either
        side); beyond them the audio is left as it was. What plays when a drawing is selected. */
    AudioDocument::Snapshot changeOutsideShape (const AudioSnapshot&, const Shape&, double sampleRate, float gainDb);

    /** Heals the shape from what surrounds it: in each frequency bin, the level across the shape
        is drawn evenly (in dB) from the level just before it to the level just after it (the
        RMS of four frames on each side), its phase carrying on from the frame before the shape
        at the rate it turned there, as a phase vocoder's does: a steady tone runs on through,
        and nothing of what was inside (a click's phases all lined up on its moment) is kept.
        At the shape's soft edges the original level is blended back in. Sixteen rounds of
        Griffin and Lim's method then bring the healed frames to agree where their windows
        overlap, so the level comes out as asked. A click, a cough or a
        whistle drawn over is replaced by the sound around it. Null for an empty shape. */
    AudioDocument::Snapshot heal (const AudioSnapshot&, const Shape&, double sampleRate);
}
