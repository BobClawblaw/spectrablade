#pragma once

#include <JuceHeader.h>
#include "PitchCorrection.h"

/** The Pitch Editor panel: the pitch of the selection (or a short file) as a curve over a grid of
    semitones, split into notes that can be dragged up and down, then applied as one edit.

    Analyse reads the selection's pitch and finds its notes. Dragging a note moves it to whole
    semitones (with Option held, freely in cents); double-clicking a note snaps it to the nearest
    semitone, and Snap to Notes does that to them all. Cmd-clicking a note splits it where it is
    clicked; right-clicking one offers Split Here, and Join with the note before or after where
    they touch. Dragging a note sideways moves it in time (the audio around it stretched or
    squeezed to fill, never past the next note or more than four times); a drag moves a note in
    pitch or in time, whichever way it first goes. Reset puts every note back where it was, in
    pitch and time (and leaves splits and joins as they are). Apply moves the
    notes in the file (by PSOLA, as Pitch Correction does), as one step to undo. */
class PitchEditor final : public juce::Component
{
public:
    /** What is analysed: the audio, the range, its rate, and the document's version then. */
    struct Source
    {
        AudioDocument::Snapshot audio;
        juce::int64 start = 0, end = 0;
        double sampleRate = 0.0;
        juce::uint32 version = 0;
    };

    std::function<std::optional<Source>()> readSource;   // the selection, or the whole file without one
    std::function<juce::uint32()> readVersion;            // the document's version now
    std::function<void (const Source&, const PitchCorrection::Track&, const std::vector<PitchCorrection::Note>&)> apply;

    static constexpr double longestSeconds = 30.0;

    PitchEditor();

    /** Forgets the analysis (after it has been applied, or when the file changes). */
    void clear (const juce::String& message);

    /** Analyses what readSource gives, as the Analyse button does. */
    void analyse();

    const std::vector<PitchCorrection::Note>& getNotes() const noexcept   { return notes; }

    /** Where a note is drawn: the middle of its box. */
    juce::Point<float> centreOf (int note) const;

    /** The note under a point (-1: none). */
    int noteAt (juce::Point<float>) const;

    /** The menu a right-click at this point shows (empty off the notes): Split Here, Join with
        Previous Note and Join with Next Note, each enabled where it would do something. */
    juce::PopupMenu menuAt (juce::Point<float>);

    /** Splits the note at this point where it is (as Cmd-click does); false when it can't (off
        the notes, or too near an end). */
    bool splitAt (juce::Point<float>);

    /** Joins note `index` with the next where they touch; false when they don't. */
    bool joinWithNext (int index);

    juce::String getStatus() const                                         { return status.getText(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void refreshButtons();
    juce::Rectangle<int> rollArea() const;
    float xOf (double frame) const;
    float yOf (double note) const;
    double pitchAt (float y) const;
    int frameAt (float x) const;

    juce::TextButton analyseButton { "Analyse" }, snapButton { "Snap to Notes" }, resetButton { "Reset" }, applyButton { "Apply" };
    juce::Label status;

    std::optional<Source> source;
    PitchCorrection::Track track;
    std::vector<PitchCorrection::Note> notes;
    double lowNote = 48.0, highNote = 72.0;   // the grid shown, in MIDI numbers
    int dragging = -1;
    double dragShift = 0.0;
    int dragMove = 0;
    enum class Axis { undecided, pitch, time } axis = Axis::undecided;
    PitchCorrection::TimeMap times;   // where the notes' moves in time put each frame

    void refreshTimes();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchEditor)
};
