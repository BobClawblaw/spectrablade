#include "ClipList.h"

ClipList::ClipList (AudioEngine& e, std::function<juce::uint32()> selectedClip, std::function<void (juce::uint32)> chooseClip)
    : engine (e), selected (std::move (selectedClip)), choose (std::move (chooseClip))
{
    auto& header = table.getHeader();
    header.addColumn ("Track", track, 110);
    header.addColumn ("Name", name, 150);
    header.addColumn ("Start (s)", start, 80);
    header.addColumn ("Length (s)", length, 80);
    header.addColumn ("File", file, 180);
    header.addColumn ("", state, 90);
    table.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff161b22));
    table.setRowHeight (22);
    addAndMakeVisible (table);

    engine.session.addChangeListener (this);
    rebuild();
    startTimerHz (8);
}

ClipList::~ClipList()
{
    engine.session.removeChangeListener (this);
}

void ClipList::rebuild()
{
    const auto s = engine.session.getState();
    std::vector<std::tuple<juce::int64, int, Row>> found;

    for (size_t t = 0; t < s->tracks.size(); ++t)
    {
        const auto& tr = s->tracks[t];

        for (auto& c : tr.clips)
        {
            juce::StringArray flags;

            if (c.locked)
                flags.add ("Locked");

            if (c.muted)
                flags.add ("Muted");

            Row row { c.id, { tr.name, c.name, juce::String ((double) c.position / s->sampleRate, 3),
                              juce::String ((double) c.length / s->sampleRate, 3), c.source.getFileName(), flags.joinIntoString (", ") } };
            found.emplace_back (c.position, (int) t, std::move (row));
        }
    }

    std::stable_sort (found.begin(), found.end(), [] (auto& a, auto& b)
    {
        return std::tie (std::get<0> (a), std::get<1> (a)) < std::tie (std::get<0> (b), std::get<1> (b));
    });

    rows.clear();

    for (auto& f : found)
        rows.push_back (std::move (std::get<2> (f)));

    table.updateContent();
    timerCallback();
    repaint();
}

juce::String ClipList::getText (int row, int column) const
{
    return juce::isPositiveAndBelow (row, (int) rows.size()) && column >= track && column <= state
               ? rows[(size_t) row].cells[column - track] : juce::String();
}

juce::uint32 ClipList::getClip (int row) const
{
    return juce::isPositiveAndBelow (row, (int) rows.size()) ? rows[(size_t) row].clip : 0;
}

void ClipList::chooseRow (int row)
{
    if (const auto id = getClip (row); id != 0 && choose != nullptr)
        choose (id);

    timerCallback();
}

void ClipList::cellClicked (int row, int, const juce::MouseEvent&)
{
    chooseRow (row);
}

void ClipList::timerCallback()
{
    const auto id = selected != nullptr ? selected() : 0;
    const auto it = std::find_if (rows.begin(), rows.end(), [id] (auto& r) { return r.clip == id; });

    if (it == rows.end())
        table.deselectAllRows();
    else if (const auto row = (int) std::distance (rows.begin(), it); row != table.getSelectedRow())
        table.selectRow (row);
}

void ClipList::changeListenerCallback (juce::ChangeBroadcaster*)
{
    rebuild();
}

void ClipList::paintRowBackground (juce::Graphics& g, int, int, int, bool isSelected)
{
    g.fillAll (isSelected ? juce::Colour (0xff2f81f7).withAlpha (0.45f) : juce::Colour (0xff161b22));
}

void ClipList::paintCell (juce::Graphics& g, int row, int column, int width, int height, bool)
{
    g.setColour (column == file || column == state ? juce::Colours::lightgrey : juce::Colours::white);
    g.setFont (13.0f);
    const auto right = column == start || column == length;
    g.drawText (getText (row, column), 6, 0, width - 12, height, right ? juce::Justification::centredRight : juce::Justification::centredLeft, true);
}

void ClipList::resized()
{
    table.setBounds (getLocalBounds());
}
