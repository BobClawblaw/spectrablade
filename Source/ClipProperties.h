#pragma once

#include "AudioEngine.h"

/** The selected clip's settings as numbers to read and type: its name, where it starts, how
    long it is, its gain and its fades. Return (or leaving a field) applies a field as an undo
    step; a value that can't apply shows the clip's setting again. Follows the session, so undo
    and edits elsewhere show here at once. */
class ClipProperties final : public juce::Component,
                             private juce::ChangeListener,
                             private juce::Timer
{
public:
    /** clipToShow says which clip is selected; it is asked again a few times a second. */
    ClipProperties (AudioEngine&, std::function<juce::uint32()> clipToShow);
    ~ClipProperties() override;

    enum class Field { name, start, length, gain, fadeIn, fadeOut };

    /** A field's editor, for typing into (as the tests do). */
    juce::TextEditor& getEditor (Field);

    /** The clip shown, or 0 when none is selected. */
    juce::uint32 getShownClip() const noexcept      { return shownClip; }

    /** Applies what a field holds now, as Return does. */
    void apply (Field);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refresh();

    AudioEngine& engine;
    std::function<juce::uint32()> clipToShow;
    juce::uint32 shownClip = 0;

    struct Row   // in the order of Field
    {
        juce::Label label;
        juce::TextEditor editor;
    };

    std::array<Row, 6> rows;
    juce::Label info;   // the file and take, or "No clip selected"

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipProperties)
};
