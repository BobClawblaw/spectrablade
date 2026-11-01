#pragma once

#include "AudioEngine.h"

/** The session's markers and regions in a table, in time order: name, start, end and length.
    Clicking a row asks for that marker to be shown; double-clicking it or pressing Return asks
    to rename it, and Delete or Backspace to delete it. Follows the session, so edits and undo
    show here at once. The search box above it shows only the markers whose name contains what
    is typed, ignoring case. Clicking a column header sorts by that column, and again reverses it;
    names sort ignoring case with numbers in order, and points count as ending where they start. */
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

    int getNumRows() override                          { return (int) shownMarkers.size(); }
    juce::String getText (int row, int column) const;

    /** The index in the session's markers of the marker a row shows, or -1. */
    int getMarkerIndex (int row) const;

    /** Clicks a row, as the mouse does. */
    void chooseRow (int row);

    /** Types into the search box, as the keyboard does. */
    void setSearch (const juce::String&);

    /** Sorts by a column, as clicking its header does; 0 goes back to time order. */
    void setSort (int column, bool forwards);

    void resized() override;

private:
    void paintRowBackground (juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    void cellClicked (int row, int column, const juce::MouseEvent&) override;
    void cellDoubleClicked (int row, int column, const juce::MouseEvent&) override;
    void deleteKeyPressed (int lastRowSelected) override;
    void returnKeyPressed (int lastRowSelected) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void sortOrderChanged (int column, bool forwards) override;
    void refilter();

    Session& session;
    Actions actions;
    std::vector<int> shownMarkers;   // indices in the session's markers, one per row
    int sortColumn = 0;              // 0: the session's order, which is time order
    bool sortForwards = true;
    juce::TextEditor search;
    juce::TableListBox table { {}, this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MarkerTable)
};
