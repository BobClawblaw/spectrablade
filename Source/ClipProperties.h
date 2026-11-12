#pragma once

#include "AudioEngine.h"

/** The selected clip's settings as numbers to read and type: its name, where it starts, how
    long it is, its gain, its fades and a note; for a note clip, its name, start, length and loop
    length (0 plays the notes once), the other fields put away. Return (or leaving a field)
    applies a field as an undo step; a value that can't apply shows the clip's setting again.
    Follows the session, so undo and edits elsewhere show here at once. */
class ClipProperties final : public juce::Component,
                             private juce::ChangeListener,
                             private juce::Timer
{
public:
    /** clipToShow says which clip is selected, noteClipToShow which note clip (shown in its
        place when there is one); both are asked again a few times a second. */
    ClipProperties (AudioEngine&, std::function<juce::uint32()> clipToShow, std::function<juce::uint32()> noteClipToShow = {});
    ~ClipProperties() override;

    /** The fields, in the order of their rows; a note clip's loop length takes the gain's row. */
    enum class Field { name, start, length, gain, fadeIn, fadeOut, note, loop = gain };

    /** A field's editor, for typing into (as the tests do). */
    juce::TextEditor& getEditor (Field);

    /** The clip shown, or 0 when none is selected (or a note clip is shown). */
    juce::uint32 getShownClip() const noexcept      { return shownClip; }

    /** The note clip shown, or 0. */
    juce::uint32 getShownNoteClip() const noexcept  { return shownNoteClip; }

    /** Applies what a field holds now, as Return does. */
    void apply (Field);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refresh();

    AudioEngine& engine;
    std::function<juce::uint32()> clipToShow, noteClipToShow;
    juce::uint32 shownClip = 0, shownNoteClip = 0;

    struct Row   // in the order of Field
    {
        juce::Label label;
        juce::TextEditor editor;
    };

    std::array<Row, 7> rows;
    juce::Label info;   // the file and take, or "No clip selected"

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipProperties)
};
