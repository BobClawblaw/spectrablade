#pragma once

#include <JuceHeader.h>
#include "Session.h"

/** The piano roll: the selected note clip's notes on a grid of pitch (a keyboard down the left,
    middle C marked) against time (the clip's length, a line at each repeat), edited by hand.

    A click on empty grid puts a note there (step entry): at the grid line at or before the mouse
    when snapping, as long as one grid step, at the velocity the slider shows. Dragging a note
    moves it in pitch and time (the time snapped), dragging its right edge changes its length;
    the selected notes move together; dragging the first repeat line sets the loop length. A
    click selects a note (Shift adds or takes it away), a
    drag on empty grid selects a box of them, Option-click removes one, Delete and Backspace
    remove the selected, Cmd-A selects all; the arrow keys move the selected notes a semitone
    or a grid step (Shift: an octave, or a quarter step). The velocity slider sets the selected
    notes'. A controller lane (the Lane choice: the mod wheel, the pitch bend or any CC) shows
    under the notes as a line over the timeline: a click puts a point there (snapped), dragging
    moves it, Option-click removes it; the lane's value is sent with the notes. The grid divides
    the beat at the clip's start by its choice, or is off. The time
    across is the notes' whole timeline: the part before the clip's start (its offset, after a
    trim or split) and the part past the pattern are washed out, as their notes don't play.

    Each change is handed back through `apply` as one undoable step. readClip gives what to show;
    the editor reads it ten times a second and redraws when it changed. */
class NoteEditor final : public juce::Component,
                         private juce::Timer
{
public:
    struct Shown
    {
        NoteClip clip;
        double sampleRate = 48000.0;
        double beatSeconds = 0.5;          // the session's beat at the clip's start
        juce::String trackName;
        bool hasInstrument = true;         // the track can play it

        bool operator== (const Shown& o) const noexcept
        {
            return clip == o.clip && juce::exactlyEqual (sampleRate, o.sampleRate) && juce::exactlyEqual (beatSeconds, o.beatSeconds)
                && trackName == o.trackName && hasInstrument == o.hasInstrument;
        }
    };

    std::function<std::optional<Shown>()> readClip;
    std::function<void (const NoteClip&, const juce::String& description)> apply;

    NoteEditor();
    ~NoteEditor() override;

    /** Reads the clip again now (as the timer does). */
    void refresh();

    /** The clip as shown, edits under way included. */
    const std::optional<Shown>& getShown() const noexcept          { return shown; }
    const std::set<int>& getSelectedNotes() const noexcept         { return selected; }   // indices into the clip's notes

    /** Grid steps per beat: 1, 2, 4, 8, 16 or 32, or 0 for no grid. */
    void setGridDivision (int division);
    int getGridDivision() const noexcept                           { return gridDivision; }
    juce::int64 gridStepFrames() const noexcept;

    void setVelocity (float velocity);                             // for new notes, and the selected ones
    float getVelocity() const noexcept                             { return (float) velocitySlider.getValue() / 127.0f; }

    /** The note under a point (-1: none), and the point of a note's grid cell: pitch and frame. */
    int noteAt (juce::Point<float>) const;
    int pitchAt (float y) const;
    juce::int64 frameAt (float x) const;                           // frames of the notes' timeline, not snapped
    juce::int64 wholeLength() const noexcept;                      // the timeline: the clip's offset and length
    juce::Rectangle<float> noteBounds (const ClipNote&) const;
    juce::Rectangle<int> rollArea() const;

    void selectAll();
    void removeSelected();

    /** The controller lane shown under the notes (ControllerLane::pitchBend, a CC, or -1 for none). */
    void setLane (int controller);
    int getLane() const noexcept                                   { return laneController; }
    juce::Rectangle<int> laneArea() const;                         // empty without a lane
    int lanePointAt (juce::Point<float>) const;                    // a point of the shown lane, or -1
    float laneValueAt (float y) const;
    float yForLaneValue (float value) const;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

    static constexpr int keyboardWidth = 44, rowHeight = 12, topBar = 30, laneHeight = 72;

private:
    enum class Drag { none, move, resize, box, loop, lanePoint, nothing };

    void timerCallback() override;
    void commit (const juce::String& description);
    juce::int64 snapped (juce::int64 frame) const;
    float yOf (int pitch) const;
    float xOf (juce::int64 frame) const;
    void scrollToNotes();
    void moveSelected (int semitones, juce::int64 frames);
    void refreshControls();

    std::optional<Shown> shown, base;      // what is shown, and as it was when a drag began
    std::set<int> selected;
    int gridDivision = 4;
    int lowPitch = 48;                     // the bottom row shown
    juce::int64 lastLength = 0;            // the length new notes take without a grid

    Drag drag = Drag::none;
    int dragNote = -1;
    juce::Point<float> dragStart;
    juce::int64 dragFrame = 0;
    int dragPitch = 0;
    juce::Rectangle<float> box;
    bool dragged = false;

    juce::ComboBox gridBox, laneBox;
    juce::Label laneLabel;
    int laneController = -1;
    int dragPoint = -1;
    juce::Slider velocitySlider;
    juce::Label gridLabel, velocityLabel, status;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteEditor)
};
