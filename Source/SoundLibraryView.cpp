#include "SoundLibraryView.h"
#include "Theme.h"
#include "SoundCategories.h"

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

/** The categories on the left: All Sounds, No Category, then each UCS category, opening into its
    subcategories, each with its count. */
class SoundLibraryView::Categories final : public juce::TreeView
{
public:
    explicit Categories (SoundLibraryView& v) : view (v)
    {
        setRootItemVisible (false);
        setDefaultOpenness (false);
        setIndentSize (14);
        Theme::follow (*this, juce::TreeView::backgroundColourId, Theme::surface);
        root.addSubItem (new Item (*this, Kind::all, {}, {}));
        root.addSubItem (new Item (*this, Kind::none, {}, {}));

        for (const auto& c : Ucs::categories())
            root.addSubItem (new Item (*this, Kind::category, c, {}));

        setRootItem (&root);
        root.getSubItem (0)->setSelected (true, true, juce::dontSendNotification);
    }

    ~Categories() override              { setRootItem (nullptr); }

private:
    enum class Kind { all, none, category, subCategory };

    struct Item final : public juce::TreeViewItem
    {
        Item (Categories& o, Kind k, juce::String c, juce::String id) : owner (o), kind (k), category (std::move (c)), catId (std::move (id)) {}

        bool mightContainSubItems() override            { return kind == Kind::category; }
        int getItemHeight() const override              { return 22; }
        juce::String getUniqueName() const override     { return juce::String ((int) kind) + category + "/" + catId; }

        void itemOpennessChanged (bool open) override
        {
            if (open && getNumSubItems() == 0)
                for (const auto* e : Ucs::subCategoriesOf (category))
                    addSubItem (new Item (owner, Kind::subCategory, category, e->catId));
        }

        void itemSelectionChanged (bool selected) override
        {
            if (selected)
                owner.view.showCategory (kind == Kind::all || kind == Kind::none ? juce::String() : category, catId, kind == Kind::none);
        }

        void paintItem (juce::Graphics& g, int width, int height) override
        {
            auto& v = owner.view;
            const auto n = kind == Kind::all ? v.found() : kind == Kind::none ? v.countWithCatId ({})
                         : kind == Kind::category ? v.countInCategory (category) : v.countWithCatId (catId);
            const auto* e = kind == Kind::subCategory ? Ucs::byCatId (catId) : nullptr;
            const auto label = kind == Kind::all ? juce::String ("All Sounds") : kind == Kind::none ? juce::String ("No Category")
                             : e != nullptr ? e->subCategory : category;

            if (isSelected())
                g.fillAll (Theme::selection);

            g.setFont (13.0f);
            g.setColour (n > 0 || kind == Kind::all ? Theme::text : Theme::dimText);
            g.drawText (label, 4, 0, width - 60, height, juce::Justification::centredLeft, true);
            g.setColour (Theme::dimText);
            g.drawText (juce::String (n), width - 60, 0, 54, height, juce::Justification::centredRight);
        }

        Categories& owner;
        Kind kind;
        juce::String category, catId;
    };

    struct Root final : public juce::TreeViewItem
    {
        bool mightContainSubItems() override            { return true; }
    };

    SoundLibraryView& view;
    Root root;
};

SoundLibraryView::SoundLibraryView (const juce::File& db)
    : index (db), database (db), scanner (std::make_unique<Scanner> (db)), preview (db.getSiblingFile ("Parts"))
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
    header.addColumn ("Category", category, 170);
    header.setSortColumnId (name, true);
    Theme::follow (table, juce::ListBox::backgroundColourId, Theme::surface);
    table.setRowHeight (22);
    table.setMultipleSelectionEnabled (true);
    table.addMouseListener (this, true);
    addAndMakeVisible (table);
    categories = std::make_unique<Categories> (*this);
    addAndMakeVisible (*categories);

    Theme::follow (summary, juce::Label::textColourId, Theme::dimText);
    addAndMakeVisible (summary);

    onDragOut = [this] (const juce::StringArray& files) { juce::DragAndDropContainer::performExternalDragDropOfFiles (files, false, this); };
    preview.onDragOut = [this] (const juce::StringArray& files) { juce::DragAndDropContainer::performExternalDragDropOfFiles (files, false, &preview); };
    addAndMakeVisible (preview);
    previewPlayer.setSource (&preview.getPlayer());
    setWantsKeyboardFocus (true);

    if (! index.isOpen())
        scanNote = index.getError();

    refresh();
    startTimerHz (4);
}

SoundLibraryView::~SoundLibraryView()
{
    setAudioDevice (nullptr);
    previewPlayer.setSource (nullptr);
    table.removeMouseListener (this);
}

void SoundLibraryView::setAudioDevice (juce::AudioDeviceManager* manager)
{
    if (device != nullptr)
        device->removeAudioCallback (&previewPlayer);

    device = manager;

    if (device != nullptr)
        device->addAudioCallback (&previewPlayer);
}

bool SoundLibraryView::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey && ! search.hasKeyboardFocus (false))
    {
        preview.togglePlay();
        return true;
    }

    return false;
}

void SoundLibraryView::selectedRowsChanged (int lastRowSelected)
{
    if (refreshing)
        return;

    const auto* s = soundAt (lastRowSelected);
    const auto file = s != nullptr ? s->file : juce::File();

    if (file != preview.getPlayer().getFile())
        preview.show (file);
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
        { category, SoundLibrary::Column::category },
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
    rows = index.count (query());
    total = index.count();
    counts = index.countByCategory (text);
    categoryCounts.clear();

    for (const auto& [id, n] : counts)
        if (const auto* e = Ucs::byCatId (id))
            categoryCounts[e->category] += n;

    if (categories != nullptr)
        categories->repaint();
    const juce::ScopedValueSetter<bool> quiet (refreshing, true);
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
    else if (text.isEmpty() && filterCategory.isEmpty() && filterCatId.isEmpty() && ! filterWithout)
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
        const juce::ScopedValueSetter<bool> quiet (refreshing, true);
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

        it = pages.emplace (page, index.find (query (page * pageSize, pageSize))).first;
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
        case category:
        {
            const auto* e = Ucs::byCatId (s->catId);
            return e != nullptr ? e->describe() : juce::String();
        }
        default:          return {};
    }
}

void SoundLibraryView::selectRows (const juce::SparseSet<int>& selected)
{
    table.setSelectedRows (selected, juce::sendNotification);
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

SoundLibrary::Query SoundLibraryView::query (int offset, int limit) const
{
    SoundLibrary::Query q { text, sortBy, forwards, offset, limit };
    q.category = filterCategory;
    q.catId = filterCatId;
    q.withoutCategory = filterWithout;
    return q;
}

void SoundLibraryView::showCategory (const juce::String& cat, const juce::String& catId, bool withoutCategory)
{
    filterCategory = catId.isEmpty() ? cat : juce::String();
    filterCatId = catId;
    filterWithout = withoutCategory;
    refresh();
}

juce::int64 SoundLibraryView::countWithCatId (const juce::String& catId) const
{
    const auto it = counts.find (catId);
    return it != counts.end() ? it->second : 0;
}

juce::int64 SoundLibraryView::found() const
{
    juce::int64 n = 0;

    for (const auto& [id, count] : counts)
        n += count;

    return n;
}

juce::int64 SoundLibraryView::countInCategory (const juce::String& cat) const
{
    const auto it = categoryCounts.find (cat);
    return it != categoryCounts.end() ? it->second : 0;
}

bool SoundLibraryView::setCategoryOfSelection (const juce::String& catId)
{
    std::vector<juce::int64> ids;
    const auto selected = table.getSelectedRows();

    for (int i = 0; i < selected.size(); ++i)
        if (const auto* s = soundAt (selected[i]))
            ids.push_back (s->id);

    const auto done = ! ids.empty() && index.setCategory (ids, catId);
    refresh();
    return done;
}

void SoundLibraryView::cellClicked (int, int, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        showCategoryMenu();
}

void SoundLibraryView::showCategoryMenu()
{
    // Item ids: 1 clears, 100 + n sets the n-th subcategory of the list.
    juce::PopupMenu menu, set;
    const auto& entries = Ucs::list();
    juce::String current;
    juce::PopupMenu sub;

    for (size_t n = 0; n < entries.size(); ++n)
    {
        if (entries[n].category != current)
        {
            if (current.isNotEmpty())
                set.addSubMenu (current, sub);

            current = entries[n].category;
            sub = {};
        }

        sub.addItem (100 + (int) n, entries[n].subCategory + "  (" + entries[n].catId + ")");
    }

    set.addSubMenu (current, sub);
    const auto many = table.getNumSelectedRows();
    menu.addSectionHeader (juce::String (many) + (many == 1 ? " sound" : " sounds"));
    menu.addSubMenu ("Set Category", set);
    menu.addItem (1, "Clear Category Set by Hand");
    menu.showMenuAsync (juce::PopupMenu::Options(), [this] (int result)
    {
        if (result == 1)
            setCategoryOfSelection ({});
        else if (result >= 100 && (size_t) (result - 100) < Ucs::list().size())
            setCategoryOfSelection (Ucs::list()[(size_t) (result - 100)].catId);
    });
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
    preview.setBounds (r.removeFromBottom (juce::jlimit (110, 220, r.getHeight() / 3)));
    categories->setBounds (r.removeFromLeft (juce::jlimit (150, 240, r.getWidth() / 5)));
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
