#pragma once

#include "AudioEngine.h"

/** Every clip in the session in a table, in time order: its track, name, start, length, file and
    whether it is locked or muted. The row of the selected clip is highlighted; clicking a row
    asks for that clip to be shown. Follows the session, so edits and undo show here at once.
    The search box above it shows only the clips whose name, file or track contains what is
    typed, ignoring case. */
class ClipList final : public juce::Component,
                       private juce::TableListBoxModel,
                       private juce::ChangeListener,
                       private juce::Timer
{
public:
    /** selected says which clip is selected (asked a few times a second); choose is called with
        the clip of a row that was clicked. */
    ClipList (AudioEngine&, std::function<juce::uint32()> selected, std::function<void (juce::uint32)> choose);
    ~ClipList() override;

    enum Column { track = 1, name, start, length, file, state };

    /** Every clip as (track index, clip), in the list's order: by start, then by track. */
    static std::vector<std::pair<int, const SessionClip*>> inOrder (const SessionState&);

    /** Every clip as CSV (RFC 4180, with a header line), in the list's order: track, name, start,
        end and length in seconds, the file's full path, locked and muted (yes or empty), then
        start and length in frames. Fields holding a comma, quote or line break are quoted. */
    static juce::String toCsv (const SessionState&);

    int getNumRows() override                           { return (int) rows.size(); }
    juce::String getText (int row, int column) const;
    juce::uint32 getClip (int row) const;
    int getHighlightedRow() const                       { return table.getSelectedRow(); }

    /** Clicks a row, as the mouse does. */
    void chooseRow (int row);

    /** Types into the search box, as the keyboard does. */
    void setSearch (const juce::String&);
    juce::String getSearch() const                      { return search.getText(); }

    void resized() override;

private:
    struct Row
    {
        juce::uint32 clip;
        juce::StringArray cells;   // in the order of Column
    };

    void paintRowBackground (juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    void cellClicked (int row, int column, const juce::MouseEvent&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void rebuild();

    AudioEngine& engine;
    std::function<juce::uint32()> selected;
    std::function<void (juce::uint32)> choose;
    std::vector<Row> rows;
    juce::TextEditor search;
    juce::TableListBox table { {}, this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipList)
};
