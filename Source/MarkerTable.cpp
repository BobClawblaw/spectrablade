#include "MarkerTable.h"
#include "Theme.h"
#include "TimeFormat.h"

#include "TrackColours.h"

MarkerTable::MarkerTable (Session& s, Actions a) : session (s), actions (std::move (a))
{
    auto& header = table.getHeader();
    header.addColumn ("Name", name, 180);
    header.addColumn ("Start", start, 90);
    header.addColumn ("End", end, 90);
    header.addColumn ("Length", length, 90);
    Theme::follow (table, juce::ListBox::backgroundColourId, Theme::surface);
    table.setRowHeight (22);
    addAndMakeVisible (table);

    search.setTextToShowWhenEmpty ("Search names", Theme::dimText);
    Theme::follow (search, juce::TextEditor::backgroundColourId, Theme::inset);
    search.onTextChange = [this] { refilter(); };
    search.onEscapeKey = [this] { setSearch ({}); };
    addAndMakeVisible (search);

    Theme::follow (summary, juce::Label::textColourId, Theme::dimText);
    addAndMakeVisible (summary);

    rename.onClick = [this] { returnKeyPressed (table.getSelectedRow()); };
    remove.onClick = [this] { deleteKeyPressed (table.getSelectedRow()); };

    for (auto* b : { &rename, &remove })
    {
        b->setEnabled (false);
        addAndMakeVisible (*b);
    }

    session.addChangeListener (this);
    TimeFormat::broadcaster().addChangeListener (this);
    refilter();
}

MarkerTable::~MarkerTable()
{
    session.removeChangeListener (this);
    TimeFormat::broadcaster().removeChangeListener (this);
}

void MarkerTable::refilter()
{
    const auto& markers = session.getState()->markers;
    const auto wanted = search.getText().trim();
    shownMarkers.clear();

    for (size_t i = 0; i < markers.size(); ++i)
        if (wanted.isEmpty() || markers[i].name.containsIgnoreCase (wanted))
            shownMarkers.push_back ((int) i);

    if (sortColumn != 0)
    {
        // Ties keep time order, whichever way the column runs.
        const auto key = [this, &markers] (int i) -> juce::int64
        {
            const auto& m = markers[(size_t) i];
            return sortColumn == start ? m.start : sortColumn == end ? m.getEnd() : m.length;
        };

        std::stable_sort (shownMarkers.begin(), shownMarkers.end(), [this, &markers, &key] (int a, int b)
        {
            if (sortColumn == name)
            {
                const auto order = markers[(size_t) a].name.compareNatural (markers[(size_t) b].name);
                return sortForwards ? order < 0 : order > 0;
            }

            return sortForwards ? key (a) < key (b) : key (a) > key (b);
        });
    }

    table.updateContent();

    if (table.getSelectedRow() >= getNumRows())
        table.deselectAllRows();

    updateSummary();
    selectedRowsChanged (table.getSelectedRow());
    repaint();
}

void MarkerTable::updateSummary()
{
    const auto& markers = session.getState()->markers;
    const auto regions = (int) std::count_if (markers.begin(), markers.end(), [] (const Marker& m) { return m.isRegion(); });
    const auto points = (int) markers.size() - regions;
    const auto count = [] (int n, const char* word) { return juce::String (n) + " " + word + (n == 1 ? "" : "s"); };
    auto text = count (points, "marker") + ", " + count (regions, "region");

    if ((size_t) getNumRows() < markers.size())
        text << " (" << getNumRows() << " shown)";

    summary.setText (text, juce::dontSendNotification);
}

void MarkerTable::highlightRow (int row)
{
    if (juce::isPositiveAndBelow (row, getNumRows()))
        table.selectRow (row);
    else
        table.deselectAllRows();

    selectedRowsChanged (table.getSelectedRow());
}

void MarkerTable::selectedRowsChanged (int lastRowSelected)
{
    const auto any = getMarkerIndex (lastRowSelected) >= 0;
    rename.setEnabled (any && actions.rename != nullptr);
    remove.setEnabled (any && actions.remove != nullptr);
}

void MarkerTable::setSort (int column, bool forwards)
{
    // The header tells the table later; sorting now keeps this in step with what the caller sees.
    table.getHeader().setSortColumnId (column, forwards);
    sortOrderChanged (column, forwards);
}

void MarkerTable::sortOrderChanged (int column, bool forwards)
{
    sortColumn = column;
    sortForwards = forwards;
    refilter();
}

void MarkerTable::setSearch (const juce::String& text)
{
    search.setText (text, false);
    refilter();
}

int MarkerTable::getMarkerIndex (int row) const
{
    const auto count = (int) session.getState()->markers.size();
    const auto index = juce::isPositiveAndBelow (row, (int) shownMarkers.size()) ? shownMarkers[(size_t) row] : -1;
    return juce::isPositiveAndBelow (index, count) ? index : -1;
}

juce::String MarkerTable::getText (int row, int column) const
{
    const auto s = session.getState();
    const auto index = getMarkerIndex (row);

    if (index < 0)
        return {};

    const auto& m = s->markers[(size_t) index];
    const auto seconds = [&s] (juce::int64 frames) { return TimeFormat::format (frames, s->sampleRate); };

    switch (column)
    {
        case name:   return m.name;
        case start:  return seconds (m.start);
        case end:    return m.isRegion() ? seconds (m.getEnd()) : juce::String();
        case length: return m.isRegion() ? seconds (m.length) : juce::String();
        default:     return {};
    }
}

void MarkerTable::chooseRow (int row)
{
    if (const auto index = getMarkerIndex (row); index >= 0)
    {
        table.selectRow (row);

        if (actions.choose != nullptr)
            actions.choose (index);
    }
}

void MarkerTable::cellClicked (int row, int, const juce::MouseEvent&)
{
    chooseRow (row);
}

void MarkerTable::cellDoubleClicked (int row, int, const juce::MouseEvent&)
{
    returnKeyPressed (row);
}

void MarkerTable::returnKeyPressed (int row)
{
    if (const auto index = getMarkerIndex (row); index >= 0 && actions.rename != nullptr)
        actions.rename (index);
}

void MarkerTable::deleteKeyPressed (int row)
{
    if (const auto index = getMarkerIndex (row); index >= 0 && actions.remove != nullptr)
        actions.remove (index);
}

void MarkerTable::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refilter();
}

void MarkerTable::paintRowBackground (juce::Graphics& g, int, int, int, bool isSelected)
{
    g.fillAll (isSelected ? Theme::selection : Theme::surface);
}

void MarkerTable::paintCell (juce::Graphics& g, int row, int column, int width, int height, bool)
{
    // The name cell starts with the marker's colour.
    if (column == name)
        if (const auto index = getMarkerIndex (row); index >= 0)
        {
            g.setColour (TrackColours::of (session.getState()->markers[(size_t) index]));
            g.fillRect (2, 3, 3, height - 6);
        }

    g.setColour (Theme::textStrong);
    g.setFont (13.0f);
    const auto left = column == name ? 10 : 6;
    g.drawText (getText (row, column), left, 0, width - left - 6, height,
                column == name ? juce::Justification::centredLeft : juce::Justification::centredRight, true);
}

void MarkerTable::resized()
{
    auto r = getLocalBounds();
    search.setBounds (r.removeFromTop (26).reduced (4, 2));
    auto bottom = r.removeFromBottom (28).reduced (4, 3);
    remove.setBounds (bottom.removeFromRight (70));
    bottom.removeFromRight (6);
    rename.setBounds (bottom.removeFromRight (70));
    summary.setBounds (bottom);
    table.setBounds (r);
}
