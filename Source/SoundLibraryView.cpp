#include "SoundLibraryView.h"
#include "Theme.h"

namespace
{
    constexpr int pageSize = 200, pagesKept = 20;
}

/** Scans on a thread of its own, through its own connection to the index; a scan asked for while
    one runs follows it. */
class SoundLibraryView::Scanner final : private juce::Thread
{
public:
    explicit Scanner (const juce::File& db) : juce::Thread ("Sound Library scan"), database (db)
    {
        formats.registerBasicFormats();
    }

    ~Scanner() override                     { stopThread (10000); }

    void request()
    {
        wanted = true;

        if (! isThreadRunning())
            startThread (juce::Thread::Priority::low);
    }

    bool busy() const                       { return isThreadRunning() || wanted.load(); }

    /** Starts the thread for a scan asked for just as the last one was ending. */
    void poll()
    {
        if (wanted && ! isThreadRunning())
            startThread (juce::Thread::Priority::low);
    }
    int lookedAt() const                    { return looked.load(); }

    /** What the last scan did, taken once. */
    std::optional<SoundLibrary::ScanResult> takeResult()
    {
        const juce::ScopedLock sl (lock);
        auto r = finished;
        finished.reset();
        return r;
    }

private:
    void run() override
    {
        SoundLibrary::Index connection (database);

        while (wanted.exchange (false) && ! threadShouldExit())
        {
            looked = 0;
            const auto result = connection.scan (formats, [this] (int n) { looked = n; return ! threadShouldExit(); });
            const juce::ScopedLock sl (lock);
            finished = result;
        }
    }

    juce::File database;
    juce::AudioFormatManager formats;
    std::atomic<bool> wanted { false };
    std::atomic<int> looked { 0 };
    juce::CriticalSection lock;
    std::optional<SoundLibrary::ScanResult> finished;
};

SoundLibraryView::SoundLibraryView (const juce::File& db)
    : index (db), database (db), scanner (std::make_unique<Scanner> (db))
{
    search.setTextToShowWhenEmpty ("Search names and descriptions", Theme::dimText);
    Theme::follow (search, juce::TextEditor::backgroundColourId, Theme::inset);
    search.onTextChange = [this] { text = search.getText(); refresh(); };
    search.onEscapeKey = [this] { setSearch ({}); };
    addAndMakeVisible (search);

    addButton.onClick = [this] { chooseFolder(); };
    foldersButton.onClick = [this] { showFoldersMenu(); };
    addAndMakeVisible (addButton);
    addAndMakeVisible (foldersButton);

    const auto unsorted = juce::TableHeaderComponent::defaultFlags & ~juce::TableHeaderComponent::sortable;
    auto& header = table.getHeader();
    header.addColumn ("Name", name, 260);
    header.addColumn ("Length", length, 70);
    header.addColumn ("Rate", rate, 70);
    header.addColumn ("Ch", channels, 36);
    header.addColumn ("Bits", bits, 40, 30, -1, unsorted);
    header.addColumn ("Format", format, 60);
    header.addColumn ("Folder", location, 180);
    header.addColumn ("Description", description, 260, 30, -1, unsorted);
    header.setSortColumnId (name, true);
    Theme::follow (table, juce::ListBox::backgroundColourId, Theme::surface);
    table.setRowHeight (22);
    table.setMultipleSelectionEnabled (true);
    table.addMouseListener (this, true);
    addAndMakeVisible (table);

    Theme::follow (summary, juce::Label::textColourId, Theme::dimText);
    addAndMakeVisible (summary);

    onDragOut = [this] (const juce::StringArray& files) { juce::DragAndDropContainer::performExternalDragDropOfFiles (files, false, this); };

    if (! index.isOpen())
        scanNote = index.getError();

    refresh();
    startTimerHz (4);
}

SoundLibraryView::~SoundLibraryView()
{
    table.removeMouseListener (this);
}

bool SoundLibraryView::addFolder (const juce::File& folder)
{
    if (! index.addFolder (folder))
    {
        scanNote = index.getError();
        updateSummary();
        return false;
    }

    scanNote = {};
    rescan();
    return true;
}

bool SoundLibraryView::removeFolder (const juce::File& folder)
{
    const auto removed = index.removeFolder (folder);
    refresh();
    return removed;
}

void SoundLibraryView::rescan()
{
    scanner->request();
    updateSummary();
}

bool SoundLibraryView::isScanning() const
{
    return scanner->busy();
}

void SoundLibraryView::setSearch (const juce::String& t)
{
    search.setText (t, false);
    text = t;
    refresh();
}

void SoundLibraryView::setSort (Column column, bool forwardsNow)
{
    table.getHeader().setSortColumnId (column, forwardsNow);
    sortOrderChanged (column, forwardsNow);
}

void SoundLibraryView::sortOrderChanged (int column, bool forwardsNow)
{
    static const std::map<int, SoundLibrary::Column> sorts {
        { name, SoundLibrary::Column::name }, { length, SoundLibrary::Column::length }, { rate, SoundLibrary::Column::sampleRate },
        { channels, SoundLibrary::Column::channels }, { format, SoundLibrary::Column::format }, { location, SoundLibrary::Column::folder },
    };

    if (const auto it = sorts.find (column); it != sorts.end())
    {
        sortBy = it->second;
        forwards = forwardsNow;
        refresh();
    }
}

void SoundLibraryView::refresh()
{
    pages.clear();
    rows = index.count (text);
    total = text.isEmpty() ? rows : index.count();
    table.deselectAllRows();
    table.updateContent();
    table.repaint();
    updateSummary();
}

void SoundLibraryView::updateSummary()
{
    const auto number = [] (juce::int64 n)
    {
        auto s = juce::String (n);

        for (int i = s.length() - 3; i > 0; i -= 3)
            s = s.substring (0, i) + "," + s.substring (i);

        return s;
    };
    const auto folders = index.getFolders().size();
    juce::String line;

    if (folders == 0)
        line = "No folders yet: Add Folder... chooses the folders of sounds to scan. Nothing else is scanned.";
    else if (text.isEmpty())
        line = number (total) + (total == 1 ? " sound in " : " sounds in ") + juce::String (folders) + (folders == 1 ? " folder" : " folders");
    else
        line = number (rows) + " of " + number (total) + " sounds match";

    if (scanner->busy())
        line << ". Scanning: " << number (scanner->lookedAt()) << " files looked at";
    else if (scanNote.isNotEmpty())
        line << ". " << scanNote;

    summary.setText (line, juce::dontSendNotification);
}

void SoundLibraryView::timerCallback()
{
    // While a scan runs, the table follows it now and then; when it ends, what it did.
    static constexpr int refreshEvery = 8;   // timer ticks, 2 s
    scanner->poll();

    if (auto result = scanner->takeResult())
    {
        scanNote = "Last scan: " + juce::String (result->added) + " added, " + juce::String (result->updated) + " updated, "
                   + juce::String (result->removed) + " removed" + (result->unreadable > 0 ? ", " + juce::String (result->unreadable) + " unreadable" : juce::String())
                   + (result->stopped ? " (stopped)" : "");
        refresh();
        return;
    }

    if (scanner->busy() && ++ticks % refreshEvery == 0)
    {
        const auto selected = table.getSelectedRows();
        refresh();
        table.setSelectedRows (selected, juce::dontSendNotification);
    }
    else
    {
        updateSummary();
    }
}

int SoundLibraryView::getNumRows()
{
    return (int) juce::jmin (rows, (juce::int64) std::numeric_limits<int>::max());
}

const SoundLibrary::Sound* SoundLibraryView::soundAt (int row)
{
    if (! juce::isPositiveAndBelow (row, getNumRows()))
        return nullptr;

    const auto page = row / pageSize;
    auto it = pages.find (page);

    if (it == pages.end())
    {
        if ((int) pages.size() >= pagesKept)
            pages.clear();

        it = pages.emplace (page, index.find ({ text, sortBy, forwards, page * pageSize, pageSize })).first;
    }

    const auto at = (size_t) (row - page * pageSize);
    return at < it->second.size() ? &it->second[at] : nullptr;
}

juce::String SoundLibraryView::getText (int row, int column)
{
    const auto* s = soundAt (row);

    if (s == nullptr)
        return {};

    switch (column)
    {
        case name:        return s->name;
        case length:
        {
            const auto seconds = s->seconds();
            const auto minutes = (int) (seconds / 60.0);
            return minutes > 0 ? juce::String (minutes) + ":" + juce::String (seconds - 60.0 * minutes, 2).paddedLeft ('0', 5)
                               : juce::String (seconds, 2) + " s";
        }
        case rate:
        {
            const auto khz = s->sampleRate / 1000.0;
            return (juce::exactlyEqual (khz, std::round (khz)) ? juce::String ((int) khz) : juce::String (khz, 1)) + " kHz";
        }
        case channels:    return juce::String (s->channels);
        case bits:        return s->bitDepth > 0 ? juce::String (s->bitDepth) : juce::String();
        case format:      return s->format.upToFirstOccurrenceOf (" file", false, true);
        case location:    return s->file.getParentDirectory().getRelativePathFrom (s->folder.getParentDirectory());
        case description: return s->description;
        default:          return {};
    }
}

void SoundLibraryView::selectRows (const juce::SparseSet<int>& selected)
{
    table.setSelectedRows (selected, juce::dontSendNotification);
}

juce::StringArray SoundLibraryView::getSelectedFiles()
{
    juce::StringArray files;
    const auto selected = table.getSelectedRows();

    for (int i = 0; i < selected.size(); ++i)
        if (const auto* s = soundAt (selected[i]))
            files.add (s->file.getFullPathName());

    return files;
}

void SoundLibraryView::paintRowBackground (juce::Graphics& g, int, int, int, bool isSelected)
{
    g.fillAll (isSelected ? Theme::selection : Theme::surface);
}

void SoundLibraryView::paintCell (juce::Graphics& g, int row, int column, int width, int height, bool)
{
    g.setColour (column == name ? Theme::textStrong : Theme::text);
    g.setFont (13.0f);
    const auto right = column == length || column == rate || column == channels || column == bits;
    g.drawText (getText (row, column), 6, 0, width - 12, height, right ? juce::Justification::centredRight : juce::Justification::centredLeft, true);
}

void SoundLibraryView::cellDoubleClicked (int row, int, const juce::MouseEvent&)
{
    if (const auto* s = soundAt (row); s != nullptr && onOpen != nullptr)
        onOpen (s->file);
}

void SoundLibraryView::mouseDown (const juce::MouseEvent& e)
{
    // A drag carries the selection out only when it starts on a selected row.
    const auto at = e.getEventRelativeTo (&table).getPosition();
    const auto row = table.getRowContainingPosition (at.x, at.y);
    dragStarted = false;
    dragFromSelection = e.eventComponent != &table.getHeader() && table.isRowSelected (row);
}

void SoundLibraryView::mouseDrag (const juce::MouseEvent& e)
{
    if (dragStarted || ! dragFromSelection || e.getDistanceFromDragStart() < 6 || onDragOut == nullptr)
        return;

    if (const auto files = getSelectedFiles(); ! files.isEmpty())
    {
        dragStarted = true;
        onDragOut (files);
    }
}

void SoundLibraryView::chooseFolder()
{
    chooser = std::make_unique<juce::FileChooser> ("Add a folder of sounds to the Sound Library",
                                                   juce::File::getSpecialLocation (juce::File::userHomeDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this] (const juce::FileChooser& fc)
                          {
                              if (const auto folder = fc.getResult(); folder != juce::File())
                                  addFolder (folder);
                          });
}

void SoundLibraryView::showFoldersMenu()
{
    juce::PopupMenu menu;
    const auto folders = index.getFolders();
    menu.addItem (1, "Rescan All Folders", ! folders.isEmpty() && ! scanner->busy());

    if (folders.isEmpty())
        menu.addItem (2, "No folders yet", false, false);

    for (int i = 0; i < folders.size(); ++i)
    {
        juce::PopupMenu sub;
        sub.addItem (1000 + i, "Show in Finder");
        sub.addItem (2000 + i, "Remove from the Sound Library", ! scanner->busy());
        menu.addSubMenu (folders[i].getFullPathName(), sub);
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&foldersButton), [this, folders] (int result)
    {
        if (result == 1)
            rescan();
        else if (juce::isPositiveAndBelow (result - 1000, folders.size()))
            folders[result - 1000].revealToUser();
        else if (juce::isPositiveAndBelow (result - 2000, folders.size()))
            removeFolder (folders[result - 2000]);
    });
}

void SoundLibraryView::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (30).reduced (4, 3);
    foldersButton.setBounds (top.removeFromRight (80));
    top.removeFromRight (4);
    addButton.setBounds (top.removeFromRight (110));
    top.removeFromRight (6);
    search.setBounds (top);
    summary.setBounds (r.removeFromBottom (24).reduced (4, 0));
    table.setBounds (r);
}

SoundLibraryWindow::SoundLibraryWindow (const juce::File& database)
    : DocumentWindow ("Sound Library", Theme::base, DocumentWindow::allButtons)
{
    setUsingNativeTitleBar (true);
    setContentOwned (new SoundLibraryView (database), false);
    setResizable (true, false);
    setResizeLimits (480, 260, 10000, 10000);
    centreWithSize (1000, 560);
}
