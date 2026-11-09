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

    /** A drawn selection of time and frequency: strokes of the lasso (closed outlines) and the
        brush (dabs), in the order they were drawn. Points are (document frame, octave), the octave
        being log2 of the frequency in Hz, as the spectrogram is drawn on a log scale: a line
        straight on screen is straight here too.

        Each stroke adds to what was drawn before it, or (drawn with Option) takes from it. A
        brush stroke has a strength, 0 to 1: how much it selects, so an effect on the shape applies
        by that much where only it was painted; a lasso selects fully. Where strokes overlap, the
        weight is what the last stroke leaves: an add raises it to its strength at the least, a
        take lowers it to one minus its strength at the most. */
    struct Dab
    {
        double frame = 0.0, octave = 0.0;
        double frameRadius = 0.0, octaveRadius = 0.0;   // an ellipse, round on screen at the zoom it was drawn at
    };

    struct Stroke
    {
        std::vector<juce::Point<double>> lasso;   // a closed outline (three points or more), or empty for a brush stroke
        std::vector<Dab> dabs;                    // the brush's dabs
        float strength = 1.0f;                    // how much a brush stroke selects; a lasso's is 1
        bool subtract = false;                    // takes from what was drawn before it

        bool isEmpty() const noexcept             { return lasso.size() < 3 && dabs.empty(); }

        /** The octave ranges this stroke alone covers at a frame, merged and in order. */
        std::vector<juce::Range<double>> octavesAt (double frame) const;
    };

    struct Shape
    {
        std::vector<Stroke> strokes;

        /** Nothing added: no adding stroke with an outline or a dab (strokes that only take are
            nothing on their own). */
        bool isEmpty() const noexcept;

        /** The weight at a frame over the octaves: ranges in order, each with the weight there
            (above 0); between them there is nothing. */
        std::vector<std::pair<juce::Range<double>, float>> weightsAt (double frame) const;

        /** The octave ranges with any weight at a frame, merged and in order. */
        std::vector<juce::Range<double>> octavesAt (double frame) const;
        float weightAt (double frame, double frequency) const;
        bool contains (double frame, double frequency) const      { return weightAt (frame, frequency) > 0.0f; }

        /** The frames the adding strokes span, and their frequency range in Hz. */
        juce::Range<double> frames() const;
        juce::Range<double> frequencies() const;
    };

    /** The transform drawn shapes are edited with: 2048 points (43 ms at 48 kHz), 75% overlap,
        square-root Hann windows on both sides. Frames are centred on multiples of shapeHop. */
    constexpr int shapeSize = 2048;
    constexpr int shapeHop = shapeSize / 4;

    /** How much each frame and bin is in the shape, 0 to 1 (the strokes' weights): a frame counts
        as in where its window's middle half meets the shape, and the edges then fade out over about
        a frame and two bins so the edit doesn't ring. Rows are frames firstFrame onwards, each
        shapeSize / 2 + 1 bins. */
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
