#pragma once

#include "PitchCorrection.h"

/** Dialogue contour: the pitch of a phrase's ending reshaped, so a line that trails up can be made
    to land, or a flat ending given a lift. The pitch of the range is tracked (as pitch correction
    tracks it, every 5 ms); the ending is the last stretch of pitched frames before the range's end,
    as long as asked for (shorter where the phrase is), and over it the pitch is moved by a ramp
    from nothing at the ending's start to the full move at its end, by PSOLA, so the voice keeps its
    character. Frames without a pitch, and everything before the ending, are left as they are. */
namespace DialogueContour
{
    struct Settings
    {
        float semitones = -2.0f;      // -12 to +12: where the ending lands, against where it was
        float endingSeconds = 0.4f;   // 0.1 to 1.5: how much of the phrase's end is reshaped
        float curve = 0.0f;           // -1 to 1: the ramp's bend, 0 straight, under 0 most of the move early, over 0 late
    };

    bool isValid (const Settings&);

    /** The shift for each frame of a track over [start, end), in cents, as apply moves them. */
    std::vector<double> shiftsFor (const PitchCorrection::Track&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&);

    /** Reshapes the ending of [start, end). nullptr for settings that aren't valid, a range with no
        pitched frame, or when cancelled. */
    AudioDocument::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&,
                                   const std::function<bool (float)>& progress = {});
}
