#pragma once

#include "Session.h"

/** The session's notes: a page of free text kept with the session (lyrics, which take is
    which, things to do). Typing changes them as an undo step, one for a run of typing, and
    undo and redo show here as they happen. */
class SessionNotes final : public juce::Component,
                           private juce::ChangeListener
{
public:
    explicit SessionNotes (Session&);
    ~SessionNotes() override;

    juce::TextEditor& getEditor() noexcept          { return editor; }
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    Session& session;
    juce::TextEditor editor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionNotes)
};
