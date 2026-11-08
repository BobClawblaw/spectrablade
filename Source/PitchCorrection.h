#pragma once

#include "AudioDocument.h"

/** Pitch correction for one voice or instrument at a time (a singer, a solo line): each moment's
    pitch is found and moved to the nearest note of a scale, as fast or as gently as asked.

    The pitch is found with YIN (de Cheveigné and Kawahara, 2002) every 5 ms, from 60 to 1000 Hz:
    the difference between the sound and itself a period later, normalised, at its first dip below
    0.15, refined between frames. Moments with no clear period (breaths, consonants, noise, silence)
    are left as they are.

    The pitch is moved by time-domain pitch-synchronous overlap-add (PSOLA): grains two periods
    long, cut around marks one period apart (from the loudest point of a note's first period), are
    put back closer together or further apart, each at its exact place between frames. The periods
    are spaced differently rather than played faster, and the length doesn't change. Every channel
    is moved the same, by the pitch of their mix. A note keeps its target until the pitch strays a tenth of a semitone past halfway to the
    next, so vibrato near the middle doesn't flip between two notes. Notes are tuned to A = 440 Hz. */
namespace PitchCorrection
{
    enum class Scale { chromatic, major, minor };

    struct Settings
    {
        int key = 0;                       // the scale's first note, 0 (C) to 11 (B)
        Scale scale = Scale::chromatic;
        float speedMs = 20.0f;             // how long a correction takes to settle, 0 (at once) to slowest
        float strengthPercent = 100.0f;    // how much of the way to the note, 0 to 100

        static constexpr float slowestMs = 500.0f;
    };

    bool isValid (const Settings&);

    /** The pitch every `hop` frames from `first` on (0 where there is no clear one), in Hz. */
    struct Track
    {
        juce::int64 first = 0;
        int hop = 0;
        std::vector<float> hz;
    };

    constexpr double lowestHz = 60.0, highestHz = 1000.0;

    /** The pitch of the channels' mix over [start, end). Empty when cancelled (progress gets 0..1
        and returns false to stop) or for an empty range. */
    Track detect (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress = {});

    /** Whether a note (in MIDI numbers, 69 = A 440 Hz) is in the scale. */
    bool inScale (int note, const Settings&);

    // ---- By hand: notes found in the pitch, each moved on its own.

    /** A note: a stretch of a track's frames [first, end) holding near one pitch, its pitch the
        median of theirs (in MIDI numbers, 69 = A 440 Hz, with fractions), and how far it is to be
        moved, in cents. */
    struct Note
    {
        int first = 0, end = 0;
        double pitch = 0.0;
        double shiftCents = 0.0;
        int moveFrames = 0;   // how far it is to be moved in time, in the track's frames (later: positive)
    };

    /** Where notes moved in time put each moment: knots of (frame in the track, frame it moves to),
        in order, with straight lines between them and no change outside them. A note moved starts
        moveFrames later and lasts as long, unless the next note touches it: then it ends where that
        one starts, so it is stretched or squeezed to meet it; the audio between notes that don't
        touch, and before the first and after the last moved, is stretched or squeezed to fill. The
        range's ends stay where they are. */
    struct TimeMap
    {
        std::vector<std::pair<double, double>> knots;

        bool isIdentity() const noexcept                    { return knots.empty(); }
        double toOutput (double inputFrame) const noexcept;
        double toInput (double outputFrame) const noexcept;
    };

    /** The most a stretch of audio between knots is stretched or squeezed (4: to a quarter, or four times as long). */
    constexpr double mostStretch = 4.0;

    /** The time map the notes' moves give over [start, end) (the range the track was analysed for);
        notes not wholly inside it don't move. Nothing if the moves would put two knots out of order,
        a stretch shorter than a frame, or stretch anything by more than mostStretch. */
    std::optional<TimeMap> timeMapFor (const Track&, juce::int64 start, juce::int64 end, const std::vector<Note>&);

    /** The audio's pitch over [start, end) and 50 ms either side (so the edges have context), as
        correcting needs it. Empty when cancelled or for an empty range. */
    Track analyse (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress = {});

    /** The notes in a track: each run of frames with a pitch, split where the pitch (smoothed over
        five frames) moves more than 0.6 of a semitone from the note's for 20 ms or more, so vibrato
        and small slides stay in one note and a step to another note starts a new one. */
    std::vector<Note> findNotes (const Track&);

    /** The pitch of a track's frames [first, end), as findNotes gives a note's: the median of
        theirs that have one (in MIDI numbers, with fractions); 0 when none has. */
    double pitchOf (const Track&, int first, int end);

    /** The shortest note splitNote leaves, in frames (10 ms). */
    constexpr int shortestNote = 2;

    /** Note `index` cut in two at frame `at`: two notes either side, each with the pitch of its own
        frames and the note's shift and move in time. Unchanged when `at` would leave either shorter than shortestNote. */
    std::vector<Note> splitNote (const Track&, std::vector<Note>, int index, int at);

    /** Whether note `index` ends where the next begins, so the two can be joined. */
    bool canJoin (const std::vector<Note>&, int index);

    /** Note `index` and the next made one, with the pitch of all their frames and the shift of the
        longer (the first's when they are as long), moved in time as the first was. Unchanged when
        they don't touch (canJoin). */
    std::vector<Note> joinNotes (const Track&, std::vector<Note>, int index);

    /** Each note moved to the nearest semitone (A = 440 Hz). */
    std::vector<Note> snapped (std::vector<Note>);

    /** The shift for each frame of the track, in cents: each note's own, eased from one note to
        the next over 25 ms where they touch, 0 elsewhere. */
    std::vector<double> shiftsFor (const Track&, const std::vector<Note>&);

    /** Moves the notes over [start, end) as their shifts and moves say (the track from analyse over
        the same range), easing at the range's edges as correct does. A moved note's grains are put
        where the time map says, so it keeps its pitch, and the audio around it is stretched or
        squeezed (by repeating or leaving out periods) to fill; the length doesn't change. nullptr
        for an empty range, a track that doesn't fit, moves timeMapFor refuses, or when cancelled. */
    AudioDocument::Snapshot moveNotes (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Track&,
                                       const std::vector<Note>&, const std::function<bool (float)>& progress = {});

    /** Moves the pitch over [start, end) by the shift given for each frame of the track (in cents;
        frames without a pitch are left), the track from analyse over the same range. nullptr for an
        empty range, a shift list of another length, or when cancelled. */
    AudioDocument::Snapshot shiftBy (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Track&,
                                     std::vector<double> shiftCents, const std::function<bool (float)>& progress = {});

    /** Moves the pitch over [start, end) to the scale, easing from the audio as it was over 10 ms at
        the edges of a range that doesn't reach the file's. nullptr for an empty range, invalid
        settings, or when cancelled. */
    AudioDocument::Snapshot correct (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, Settings,
                                     const std::function<bool (float)>& progress = {});
}
