#include "UndoHistory.h"
#include "Theme.h"

UndoHistory::UndoHistory (Session& s) : UndoHistory (s, nullptr, {})
{
}

UndoHistory::UndoHistory (Session& s, AudioDocument& d, std::function<bool()> shown) : UndoHistory (s, &d, std::move (shown))
{
}

UndoHistory::UndoHistory (Session& s, AudioDocument* d, std::function<bool()> shown)
    : session (s), document (d), sessionShown (std::move (shown))
{
    Theme::follow (list, juce::ListBox::backgroundColourId, Theme::surface);
    list.setRowHeight (22);
    addAndMakeVisible (list);
    session.addChangeListener (this);

    if (document != nullptr)
        document->addChangeListener (this);

    refresh();
}

UndoHistory::~UndoHistory()
{
    session.removeChangeListener (this);

    if (document != nullptr)
        document->removeChangeListener (this);
}

juce::String UndoHistory::getText (int row) const
{
    return row == 0 ? juce::String ("Start") : steps[row];
}

void UndoHistory::chooseRow (int row)
{
    if (juce::isPositiveAndBelow (row, steps.size()))
    {
        if (showingSession())
            session.goTo (row);
        else
            document->goTo (row);
    }

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
    steps = showingSession() ? session.getHistory() : document->getHistory();
    list.updateContent();
    list.selectRow (position(), false, true);
    list.repaint();
}

void UndoHistory::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    g.fillAll (selected ? Theme::selection : Theme::surface);
    g.setColour (row > position() ? Theme::dimText : Theme::textStrong);
    g.setFont (13.0f);
    g.drawText (getText (row), 8, 0, width - 16, height, juce::Justification::centredLeft, true);
}

void UndoHistory::resized()
{
    list.setBounds (getLocalBounds());
}
