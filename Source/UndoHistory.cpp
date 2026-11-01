#include "UndoHistory.h"

UndoHistory::UndoHistory (Session& s) : session (s)
{
    list.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff161b22));
    list.setRowHeight (22);
    addAndMakeVisible (list);
    session.addChangeListener (this);
    refresh();
}

UndoHistory::~UndoHistory()
{
    session.removeChangeListener (this);
}

juce::String UndoHistory::getText (int row) const
{
    return row == 0 ? juce::String ("Start") : steps[row];
}

void UndoHistory::chooseRow (int row)
{
    if (juce::isPositiveAndBelow (row, steps.size()))
        session.goTo (row);

    refresh();
}

void UndoHistory::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    chooseRow (row);
}

void UndoHistory::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

void UndoHistory::refresh()
{
    steps = session.getHistory();
    list.updateContent();
    list.selectRow (session.getPosition(), false, true);
    list.repaint();
}

void UndoHistory::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    g.fillAll (selected ? juce::Colour (0xff2f81f7).withAlpha (0.45f) : juce::Colour (0xff161b22));
    g.setColour (row > session.getPosition() ? juce::Colours::grey : juce::Colours::white);
    g.setFont (13.0f);
    g.drawText (getText (row), 8, 0, width - 16, height, juce::Justification::centredLeft, true);
}

void UndoHistory::resized()
{
    list.setBounds (getLocalBounds());
}
