#pragma once

#include "Session.h"

/** The session's undo steps in a list, oldest first, from where it started. The step it is at
    is highlighted and the ones that can be redone are dimmed; clicking a step goes back or
    forward to it. Follows the session as steps are added, undone and redone. */
class UndoHistory final : public juce::Component,
                          private juce::ListBoxModel,
                          private juce::ChangeListener
{
public:
    explicit UndoHistory (Session&);
    ~UndoHistory() override;

    int getNumRows() override                 { return steps.size(); }
    juce::String getText (int row) const;     // "Start" for the first
    int getHighlightedRow() const             { return list.getSelectedRow(); }

    /** Clicks a row, as the mouse does. */
    void chooseRow (int row);

    void resized() override;

private:
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void refresh();

    Session& session;
    juce::StringArray steps;
    juce::ListBox list { {}, this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UndoHistory)
};
