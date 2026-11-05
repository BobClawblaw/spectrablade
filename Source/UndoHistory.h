#pragma once

#include "AudioDocument.h"
#include "Session.h"

/** The undo steps in a list, oldest first, from where it started: the session's, or with a
    document given, the open file's while the editor shows (sessionShown says which). The step it
    is at is highlighted and the ones that can be redone are dimmed; clicking a step goes back or
    forward to it. Follows the steps as they are added, undone and redone. */
class UndoHistory final : public juce::Component,
                          private juce::ListBoxModel,
                          private juce::ChangeListener
{
public:
    explicit UndoHistory (Session&);
    UndoHistory (Session&, AudioDocument&, std::function<bool()> sessionShown);
    ~UndoHistory() override;

    int getNumRows() override                 { return steps.size(); }
    juce::String getText (int row) const;     // "Start" for the first
    int getHighlightedRow() const             { return list.getSelectedRow(); }

    /** Clicks a row, as the mouse does. */
    void chooseRow (int row);

    /** Reads the steps again, from the session or the file, whichever shows now. */
    void refresh();

    void resized() override;

private:
    UndoHistory (Session&, AudioDocument*, std::function<bool()> sessionShown);

    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    bool showingSession() const               { return document == nullptr || sessionShown == nullptr || sessionShown(); }
    int position() const                      { return showingSession() ? session.getPosition() : document->getPosition(); }

    Session& session;
    AudioDocument* document = nullptr;
    std::function<bool()> sessionShown;
    juce::StringArray steps;
    juce::ListBox list { {}, this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UndoHistory)
};
