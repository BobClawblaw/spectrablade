#pragma once

#include "AudioEngine.h"

/** The session's markers and regions in a table, in time order: name, start, end and length.
    Clicking a row asks for that marker to be shown; double-clicking it or pressing Return asks
    to rename it, and Delete or Backspace to delete it. Follows the session, so edits and undo
    show here at once. */
class MarkerTable final : public juce::Component,
                         private juce::TableListBoxModel,
                         private juce::ChangeListener
{
public:
    /** Each is called with the marker's index in the session's markers. */
    struct Actions
    {
        std::function<void (int)> choose, rename, remove;
    };

    MarkerTable (Session&, Actions);
    ~MarkerTable() override;

    enum Column { name = 1, start, end, length };

    int getNumRows() override;
    juce::String getText (int row, int column) const;

    /** Clicks a row, as the mouse does. */
    void chooseRow (int row);

    void resized() override;

private:
    void paintRowBackground (juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    void cellClicked (int row, int column, const juce::MouseEvent&) override;
    void cellDoubleClicked (int row, int column, const juce::MouseEvent&) override;
    void deleteKeyPressed (int lastRowSelected) override;
    void returnKeyPressed (int lastRowSelected) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    Session& session;
    Actions actions;
    juce::TableListBox table { {}, this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MarkerTable)
};
