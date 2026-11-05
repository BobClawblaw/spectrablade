#include "SessionNotes.h"
#include "Theme.h"

SessionNotes::SessionNotes (Session& s) : session (s)
{
    editor.setMultiLine (true, true);
    editor.setReturnKeyStartsNewLine (true);
    editor.setTabKeyUsedAsCharacter (true);
    editor.setScrollbarsShown (true);
    editor.setFont (juce::FontOptions (14.0f));
    editor.setTextToShowWhenEmpty ("Notes for this session: lyrics, takes, things to do...", Theme::dimText);
    editor.setText (session.getState()->notes, false);
    editor.onTextChange = [this]
    {
        auto next = *session.getState();
        next.notes = editor.getText();
        session.apply (std::move (next), "Session Notes", true);
    };

    addAndMakeVisible (editor);
    session.addChangeListener (this);
}

SessionNotes::~SessionNotes()
{
    session.removeChangeListener (this);
}

void SessionNotes::resized()
{
    editor.setBounds (getLocalBounds().reduced (6));
}

void SessionNotes::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Undo, redo or another session: show its notes, the caret kept where it can be.
    const auto& notes = session.getState()->notes;

    if (notes != editor.getText())
    {
        const auto caret = editor.getCaretPosition();
        editor.setText (notes, false);
        editor.setCaretPosition (juce::jmin (caret, notes.length()));
    }
}
