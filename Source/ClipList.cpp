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

    search.setTextToShowWhenEmpty ("Search names, files and tracks", juce::Colours::grey);
    search.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0e1013));
    search.onTextChange = [this] { rebuild(); };
    search.onEscapeKey = [this] { setSearch ({}); };
    addAndMakeVisible (search);

    engine.session.addChangeListener (this);
    rebuild();
    startTimerHz (8);
}

ClipList::~ClipList()
{
    engine.session.removeChangeListener (this);
}

std::vector<std::pair<int, const SessionClip*>> ClipList::inOrder (const SessionState& s)
{
    std::vector<std::pair<int, const SessionClip*>> clips;

    for (size_t t = 0; t < s.tracks.size(); ++t)
        for (auto& c : s.tracks[t].clips)
            clips.emplace_back ((int) t, &c);

    std::stable_sort (clips.begin(), clips.end(), [] (auto& a, auto& b)
    {
        return std::tie (a.second->position, a.first) < std::tie (b.second->position, b.first);
    });

    return clips;
}

juce::String ClipList::toCsv (const SessionState& s)
{
    const auto field = [] (juce::String text)
    {
        return text.containsAnyOf (",\"\r\n") ? "\"" + text.replace ("\"", "\"\"") + "\"" : text;
    };
    const auto seconds = [&s] (juce::int64 frames) { return juce::String ((double) frames / s.sampleRate, 6); };
    juce::String csv ("Track,Name,Start (s),End (s),Length (s),File,Locked,Muted,Start (frames),Length (frames)\r\n");

    for (auto [t, c] : inOrder (s))
        csv << field (s.tracks[(size_t) t].name) << "," << field (c->name) << "," << seconds (c->position) << "," << seconds (c->getEnd()) << ","
            << seconds (c->length) << "," << field (c->source.getFullPathName()) << "," << (c->locked ? "yes" : "") << ","
            << (c->muted ? "yes" : "") << "," << juce::String (c->position) << "," << juce::String (c->length) << "\r\n";

    return csv;
}

void ClipList::rebuild()
{
    const auto s = engine.session.getState();
    const auto wanted = search.getText().trim();
    rows.clear();

    for (auto [t, c] : inOrder (*s))
    {
        const auto& tr = s->tracks[(size_t) t];

        if (wanted.isNotEmpty() && ! c->name.containsIgnoreCase (wanted) && ! c->source.getFileName().containsIgnoreCase (wanted)
            && ! tr.name.containsIgnoreCase (wanted))
            continue;

        juce::StringArray flags;

        if (c->locked)
            flags.add ("Locked");

        if (c->muted)
            flags.add ("Muted");

        rows.push_back ({ c->id, { tr.name, c->name, juce::String ((double) c->position / s->sampleRate, 3),
                                   juce::String ((double) c->length / s->sampleRate, 3), c->source.getFileName(), flags.joinIntoString (", ") } });
    }

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

void ClipList::setSearch (const juce::String& text)
{
    search.setText (text, false);
    rebuild();
}

void ClipList::resized()
{
    auto r = getLocalBounds();
    search.setBounds (r.removeFromTop (26).reduced (4, 2));
    table.setBounds (r);
}
