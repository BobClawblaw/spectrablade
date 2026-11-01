#include "MarkerTable.h"

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
    session.addChangeListener (this);
}

MarkerTable::~MarkerTable()
{
    session.removeChangeListener (this);
}

int MarkerTable::getNumRows()
{
    return (int) session.getState()->markers.size();
}

juce::String MarkerTable::getText (int row, int column) const
{
    const auto s = session.getState();

    if (! juce::isPositiveAndBelow (row, (int) s->markers.size()))
        return {};

    const auto& m = s->markers[(size_t) row];
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
    if (juce::isPositiveAndBelow (row, getNumRows()))
    {
        table.selectRow (row);

        if (actions.choose != nullptr)
            actions.choose (row);
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
    if (juce::isPositiveAndBelow (row, getNumRows()) && actions.rename != nullptr)
        actions.rename (row);
}

void MarkerTable::deleteKeyPressed (int row)
{
    if (juce::isPositiveAndBelow (row, getNumRows()) && actions.remove != nullptr)
        actions.remove (row);
}

void MarkerTable::changeListenerCallback (juce::ChangeBroadcaster*)
{
    table.updateContent();

    if (table.getSelectedRow() >= getNumRows())
        table.deselectAllRows();

    repaint();
}

void MarkerTable::paintRowBackground (juce::Graphics& g, int, int, int, bool isSelected)
{
    g.fillAll (isSelected ? juce::Colour (0xff2f81f7).withAlpha (0.45f) : juce::Colour (0xff161b22));
}

void MarkerTable::paintCell (juce::Graphics& g, int row, int column, int width, int height, bool)
{
    g.setColour (juce::Colours::white);
    g.setFont (13.0f);
    g.drawText (getText (row, column), 6, 0, width - 12, height,
                column == name ? juce::Justification::centredLeft : juce::Justification::centredRight, true);
}

void MarkerTable::resized()
{
    table.setBounds (getLocalBounds());
}
