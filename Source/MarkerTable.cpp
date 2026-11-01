#include "MarkerTable.h"

#include "TrackColours.h"

MarkerTable::MarkerTable (Session& s, Actions a) : session (s), actions (std::move (a))
{
    auto& header = table.getHeader();
    header.addColumn ("Name", name, 180);
    header.addColumn ("Start (s)", start, 90);
    header.addColumn ("End (s)", end, 90);
    header.addColumn ("Length (s)", length, 90);
    table.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff161b22));
    table.setRowHeight (22);
    addAndMakeVisible (table);

    search.setTextToShowWhenEmpty ("Search names", juce::Colours::grey);
    search.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0e1013));
    search.onTextChange = [this] { refilter(); };
    search.onEscapeKey = [this] { setSearch ({}); };
    addAndMakeVisible (search);

    session.addChangeListener (this);
    refilter();
}

MarkerTable::~MarkerTable()
{
    session.removeChangeListener (this);
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

    repaint();
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
    const auto seconds = [&s] (juce::int64 frames) { return juce::String ((double) frames / s->sampleRate, 3); };

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
    g.fillAll (isSelected ? juce::Colour (0xff2f81f7).withAlpha (0.45f) : juce::Colour (0xff161b22));
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

    g.setColour (juce::Colours::white);
    g.setFont (13.0f);
    const auto left = column == name ? 10 : 6;
    g.drawText (getText (row, column), left, 0, width - left - 6, height,
                column == name ? juce::Justification::centredLeft : juce::Justification::centredRight, true);
}

void MarkerTable::resized()
{
    auto r = getLocalBounds();
    search.setBounds (r.removeFromTop (26).reduced (4, 2));
    table.setBounds (r);
}
