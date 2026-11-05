#include "ClipList.h"
#include "Theme.h"

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
    Theme::follow (table, juce::ListBox::backgroundColourId, Theme::surface);
    table.setRowHeight (22);
    addAndMakeVisible (table);

    search.setTextToShowWhenEmpty ("Search names, files and tracks", Theme::dimText);
    Theme::follow (search, juce::TextEditor::backgroundColourId, Theme::inset);
    search.onTextChange = [this] { rebuild(); };
    search.onEscapeKey = [this] { setSearch ({}); };
    addAndMakeVisible (search);

    Theme::follow (summary, juce::Label::textColourId, Theme::dimText);
    addAndMakeVisible (summary);

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
                                   juce::String ((double) c->length / s->sampleRate, 3), c->source.getFileName(), flags.joinIntoString (", ") },
                          t, c->position, c->length });
    }

    // The count line: every clip, its locked and muted ones, and the shown clips' total length.
    auto total = 0, locked = 0, muted = 0;
    juce::int64 shownFrames = 0;

    for (auto& t : s->tracks)
        for (auto& c : t.clips)
        {
            ++total;
            locked += c.locked ? 1 : 0;
            muted += c.muted ? 1 : 0;
        }

    for (auto& r : rows)
        shownFrames += r.length;

    auto text = juce::String (total) + (total == 1 ? " clip" : " clips");

    if (locked > 0)
        text << ", " << locked << " locked";

    if (muted > 0)
        text << ", " << muted << " muted";

    text << (rows.size() < (size_t) total ? "; " + juce::String ((int) rows.size()) + " shown, " : juce::String ("; "))
         << juce::String ((double) shownFrames / s->sampleRate, 3) << " s in all";
    summary.setText (text, juce::dontSendNotification);

    if (sortColumn != 0)
    {
        std::stable_sort (rows.begin(), rows.end(), [this] (const Row& a, const Row& b)
        {
            const auto order = [this, &a, &b]() -> int
            {
                switch (sortColumn)
                {
                    case track:  return a.trackIndex < b.trackIndex ? -1 : (a.trackIndex > b.trackIndex ? 1 : 0);
                    case start:  return a.position < b.position ? -1 : (a.position > b.position ? 1 : 0);
                    case length: return a.length < b.length ? -1 : (a.length > b.length ? 1 : 0);
                    default:     return a.cells[sortColumn - track].compareNatural (b.cells[sortColumn - track]);
                }
            }();

            return sortForwards ? order < 0 : order > 0;
        });
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

void ClipList::setSort (int column, bool forwards)
{
    // The header tells the table later; sorting now keeps this in step with what the caller sees.
    table.getHeader().setSortColumnId (column, forwards);
    sortOrderChanged (column, forwards);
}

void ClipList::sortOrderChanged (int column, bool forwards)
{
    sortColumn = column;
    sortForwards = forwards;
    rebuild();
}

void ClipList::changeListenerCallback (juce::ChangeBroadcaster*)
{
    rebuild();
}

void ClipList::paintRowBackground (juce::Graphics& g, int, int, int, bool isSelected)
{
    g.fillAll (isSelected ? Theme::selection : Theme::surface);
}

void ClipList::paintCell (juce::Graphics& g, int row, int column, int width, int height, bool)
{
    g.setColour (column == file || column == state ? Theme::text : Theme::textStrong);
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
    summary.setBounds (r.removeFromBottom (24).reduced (4, 0));
    table.setBounds (r);
}
