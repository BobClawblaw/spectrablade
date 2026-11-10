#include "SoundLibraryView.h"
#include "AudioFileIO.h"
#include "Platform.h"
#include "Theme.h"
#include "SoundCategories.h"
#include "SoundMetadata.h"

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
        AudioFileIO::registerFormats (formats);
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

    /** Chooses "All Sounds", without telling the view (it has cleared the category itself). */
    void chooseAll()
    {
        if (auto* all = root.getSubItem (0))
            all->setSelected (true, true, juce::dontSendNotification);
    }

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
        juce::String getUniqueName() const override     { return "root"; }   // so the tree's openness can be saved and restored
    };

    SoundLibraryView& view;
    Root root;
};

/** The folders the sounds are in, from the index: each watched folder, opening into the folders
    inside it that hold sounds, with how many sounds are in each (and deeper). Choosing one shows
    only its sounds; right-clicking one shows it in the Finder. */
class SoundLibraryView::Folders final : public juce::TreeView
{
public:
    explicit Folders (SoundLibraryView& v) : view (v)
    {
        setRootItemVisible (false);
        setDefaultOpenness (false);
        setIndentSize (14);
        Theme::follow (*this, juce::TreeView::backgroundColourId, Theme::surface);
        setRootItem (&root);
        rebuild();
    }

    ~Folders() override                 { setRootItem (nullptr); }

    /** Lists the watched folders again (after one is added or removed, or a scan), keeping what
        was open and chosen where it is still there. */
    void rebuild()
    {
        const auto state = getOpennessState (true);
        const juce::ScopedValueSetter<bool> quiet (rebuilding, true);
        root.clearSubItems();
        root.addSubItem (new Item (*this, {}));

        for (const auto& f : view.index.getFolders())
            root.addSubItem (new Item (*this, f));

        if (state != nullptr)
            restoreOpennessState (*state, true);

        if (getNumSelectedItems() == 0)
            root.getSubItem (0)->setSelected (true, false, juce::dontSendNotification);
    }

    /** Chooses "All Folders", without telling the view (it has cleared the folder itself). */
    void chooseAll()
    {
        if (auto* all = root.getSubItem (0))
            all->setSelected (true, true, juce::dontSendNotification);
    }

private:
    struct Item final : public juce::TreeViewItem
    {
        Item (Folders& o, juce::File f) : owner (o), folder (std::move (f)) {}

        bool isAll() const                              { return folder == juce::File(); }
        bool mightContainSubItems() override            { return ! isAll() && (! listed || getNumSubItems() > 0); }
        int getItemHeight() const override              { return 22; }
        juce::String getUniqueName() const override     { return isAll() ? juce::String ("*all") : folder.getFullPathName(); }

        void itemOpennessChanged (bool open) override
        {
            if (open && ! listed)
            {
                listed = true;

                for (const auto& f : owner.view.index.subfoldersOf (folder))
                    addSubItem (new Item (owner, f));

                treeHasChanged();
            }
        }

        void itemSelectionChanged (bool selected) override
        {
            if (selected && ! owner.rebuilding)
                owner.view.showFolder (folder);
        }

        void itemClicked (const juce::MouseEvent& e) override
        {
            if (! e.mods.isPopupMenu() || isAll())
                return;

            juce::PopupMenu menu;
            menu.addItem (Platform::showInFileManager(), folder.exists(), false, [f = folder] { f.revealToUser(); });
            const auto out = owner.view.foldersLeftOut.contains (folder);
            menu.addItem (out ? "Show in the List Again" : "Leave Out of the List", true, false,
                          [v = juce::Component::SafePointer<SoundLibraryView> (&owner.view), f = folder, out]
                          {
                              if (v == nullptr)
                                  return;

                              if (out)
                                  v->showFolderAgain (f);
                              else
                                  v->leaveOutFolder (f);
                          });
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (owner).withMousePosition());
        }

        void paintItem (juce::Graphics& g, int width, int height) override
        {
            if (count < 0)
                count = isAll() ? owner.view.total : owner.view.index.countWithin (folder);

            const auto top = getParentItem() == &owner.root;
            const auto out = ! isAll() && owner.view.foldersLeftOut.contains (folder);
            const auto label = isAll() ? juce::String ("All Folders") : folder.getFileName() + (out ? " (left out)" : "");

            if (isSelected())
                g.fillAll (Theme::selection);

            const juce::Font font (juce::FontOptions (13.0f));
            g.setFont (font);
            const auto nameColour = (count > 0 || isAll()) && ! out ? Theme::text : Theme::dimText;
            auto x = 4;
            const auto room = width - 64;

            if (top && ! isAll())
            {
                // A watched folder: where it is, dimmed, before its name; cut from its start (the
                // part nearest the name is the telling part) when the room is short, and dropped
                // when none of it fits. The tooltip holds the whole path.
                auto where = folder.getParentDirectory().getFullPathName();

                if (! where.endsWithChar ('/'))
                    where += "/";

                const auto nameWidth = (int) std::ceil (juce::GlyphArrangement::getStringWidth (font, label));
                const auto whereRoom = room - nameWidth - 2;
                auto cut = false;

                while (where.isNotEmpty() && juce::GlyphArrangement::getStringWidth (font, (cut ? juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6")) : juce::String()) + where) > (float) whereRoom)
                {
                    const auto slash = where.indexOfChar (1, '/');
                    where = slash > 0 ? where.substring (slash) : juce::String();
                    cut = true;
                }

                if (where.isNotEmpty())
                {
                    const auto shown = (cut ? juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6")) : juce::String()) + where;
                    const auto shownWidth = (int) std::ceil (juce::GlyphArrangement::getStringWidth (font, shown));
                    g.setColour (Theme::dimText);
                    g.drawText (shown, x, 0, shownWidth, height, juce::Justification::centredLeft, false);
                    x += shownWidth;
                }
            }

            g.setColour (nameColour);
            g.drawText (label, x, 0, juce::jmax (0, room + 4 - x), height, juce::Justification::centredLeft, true);
            g.setColour (Theme::dimText);
            g.drawText (juce::String (count), width - 60, 0, 54, height, juce::Justification::centredRight);
        }

        juce::String getTooltip() override              { return folder.getFullPathName(); }

        Folders& owner;
        juce::File folder;   // none: all folders
        bool listed = false;
        juce::int64 count = -1;
    };

    struct Root final : public juce::TreeViewItem
    {
        bool mightContainSubItems() override            { return true; }
        juce::String getUniqueName() const override     { return "root"; }   // so the tree's openness can be saved and restored
    };

    SoundLibraryView& view;
    Root root;
    bool rebuilding = false;
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
    header.addColumn ({}, favourite, 26, 26, 26, unsorted);
    header.addColumn ("Rating", rating, 5 * starWidth + 12, 5 * starWidth + 12, 5 * starWidth + 12);
    header.addColumn ("Length", length, 70);
    header.addColumn ("Rate", rate, 70);
    header.addColumn ("Ch", channels, 36);
    header.addColumn ("Bits", bits, 40, 30, -1, unsorted);
    header.addColumn ("Format", format, 60);
    header.addColumn ("Folder", location, 180);
    header.addColumn ("Description", description, 260, 30, -1, unsorted);
    header.addColumn ("Category", category, 170);
    header.addColumn ("Tags", tags, 160, 30, -1, unsorted);

    favouritesButton.onClick = [this] { showFavouritesOnly (favouritesButton.getToggleState()); };
    addAndMakeVisible (favouritesButton);
    ratingChoice.addItemList ({ "Any Rating", "1 Star or More", "2 Stars or More", "3 Stars or More", "4 Stars or More", "5 Stars" }, 1);
    ratingChoice.setSelectedId (1, juce::dontSendNotification);
    ratingChoice.onChange = [this] { showMinRating (ratingChoice.getSelectedId() - 1); };
    addAndMakeVisible (ratingChoice);
    tagChoice.onChange = [this]
    {
        const auto i = tagChoice.getSelectedId() - 2;
        showTag (juce::isPositiveAndBelow (i, (int) tagCounts.size()) ? tagCounts[(size_t) i].first : juce::String());
    };
    addAndMakeVisible (tagChoice);
    formatsButton.setTooltip ("Which formats to list: WAV to start with, so the same sounds as WAV and AIFF show once; add the rest here");
    formatsButton.setButtonText (describeShownFormats());
    formatsButton.onClick = [this] { showFormatsMenu(); };
    addAndMakeVisible (formatsButton);
    header.setSortColumnId (name, true);
    Theme::follow (table, juce::ListBox::backgroundColourId, Theme::surface);
    table.setRowHeight (22);
    table.setMultipleSelectionEnabled (true);
    table.addMouseListener (this, true);
    addAndMakeVisible (table);
    categories = std::make_unique<Categories> (*this);
    addChildComponent (*categories);
    folderTree = std::make_unique<Folders> (*this);
    addAndMakeVisible (*folderTree);

    for (auto* b : { &byFolderButton, &byCategoryButton })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (1);
        addAndMakeVisible (*b);
    }

    byFolderButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    byCategoryButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    byFolderButton.setToggleState (true, juce::dontSendNotification);
    byFolderButton.onClick = [this] { if (byFolderButton.getToggleState()) showPane (Pane::folders); };
    byCategoryButton.onClick = [this] { if (byCategoryButton.getToggleState()) showPane (Pane::categories); };

    divider.onDrag = [this] (int x) { paneWidth = juce::jmax (120, x - 3); resized(); };
    divider.onDragEnd = [this] { if (onPaneWidthChanged != nullptr) onPaneWidthChanged (paneWidth); };
    addAndMakeVisible (divider);

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
    if (writer != nullptr)
        writer->stopThread (-1);

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
    if (search.hasKeyboardFocus (false))
        return false;

    if (key == juce::KeyPress::returnKey)
        return insertSelection() || true;

    if (key == juce::KeyPress::spaceKey)
    {
        preview.togglePlay();
        return true;
    }

    // 0 to 5 rate the selected sounds; F marks them favourites (or, when all are, unmarks them).
    if (const auto c = key.getTextCharacter(); c >= '0' && c <= '5' && ! key.getModifiers().isAnyModifierKeyDown())
        return setRatingOfSelection (c - '0') || true;

    if (key == juce::KeyPress ('f') || key == juce::KeyPress ('F', juce::ModifierKeys::shiftModifier, 'F'))
    {
        auto allFavourites = true;
        const auto selected = table.getSelectedRows();

        for (int i = 0; i < selected.size(); ++i)
            if (const auto* s = soundAt (selected[i]))
                allFavourites = allFavourites && s->favourite;

        return setFavouriteOfSelection (! allFavourites) || true;
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
    folderTree->rebuild();
    rescan();
    return true;
}

bool SoundLibraryView::removeFolder (const juce::File& folder)
{
    const auto removed = index.removeFolder (folder);

    if (removed && (filterWithin == folder || filterWithin.isAChildOf (folder)))
        filterWithin = juce::File();

    folderTree->rebuild();
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
        { category, SoundLibrary::Column::category }, { rating, SoundLibrary::Column::rating },
    };

    if (const auto it = sorts.find (column); it != sorts.end())
    {
        sortBy = it->second;
        forwards = forwardsNow;
        refresh();
    }
}

void SoundLibraryView::refresh (bool keepSelection)
{
    pages.clear();
    const auto before = rows;
    rows = index.count (query());
    total = index.count();
    counts = index.countByCategory (text);
    categoryCounts.clear();

    for (const auto& [id, n] : counts)
        if (const auto* e = Ucs::byCatId (id))
            categoryCounts[e->category] += n;

    if (categories != nullptr)
        categories->repaint();

    if (folderTree != nullptr)
        folderTree->repaint();

    tagCounts = index.tagCounts();
    updateTagChoices();
    const juce::ScopedValueSetter<bool> quiet (refreshing, true);

    // Rating or tagging the selection keeps it, unless that changed which sounds are listed.
    if (! keepSelection || rows != before)
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
    else if (text.isEmpty() && filterCategory.isEmpty() && filterCatId.isEmpty() && ! filterWithout && ! filterFavourites
             && filterRating == 0 && filterTag.isEmpty() && filterWithin == juce::File() && foldersLeftOut.isEmpty() && rows == total)
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
        scanNote = (writtenNote.isNotEmpty() ? writtenNote + ". " : juce::String()) + "Last scan: " + juce::String (result->added) + " added, " + juce::String (result->updated) + " updated, "
                   + juce::String (result->removed) + " removed" + (result->unreadable > 0 ? ", " + juce::String (result->unreadable) + " unreadable" : juce::String())
                   + (result->stopped ? " (stopped)" : "");
        writtenNote = {};
        folderTree->rebuild();
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
        case favourite:   return s->favourite ? juce::String (juce::CharPointer_UTF8 ("\xe2\x99\xa5")) : juce::String();
        case rating:      return s->rating > 0 ? juce::String (s->rating) : juce::String();
        case tags:        return s->tags.joinIntoString (", ");
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
    if (column == rating || column == favourite)
    {
        const auto* s = soundAt (row);

        if (s == nullptr)
            return;

        const auto cy = (float) height * 0.5f;

        if (column == favourite)
        {
            // A heart: two lobes and a point.
            const auto cx = (float) width * 0.5f, r = 3.2f;
            juce::Path heart;
            heart.startNewSubPath (cx, cy + 5.5f);
            heart.cubicTo (cx - 2.0f * r - 1.0f, cy + 1.0f, cx - 2.0f * r, cy - 2.5f * r, cx, cy - 1.2f);
            heart.cubicTo (cx + 2.0f * r, cy - 2.5f * r, cx + 2.0f * r + 1.0f, cy + 1.0f, cx, cy + 5.5f);
            heart.closeSubPath();
            g.setColour (s->favourite ? Theme::accent : Theme::dimText.withAlpha (0.35f));

            if (s->favourite)
                g.fillPath (heart);
            else
                g.strokePath (heart, juce::PathStrokeType (1.0f));

            return;
        }

        for (int i = 0; i < 5; ++i)
        {
            juce::Path star;
            star.addStar ({ 6.0f + (float) starWidth * ((float) i + 0.5f), cy }, 5, 2.4f, 5.6f);
            g.setColour (i < s->rating ? Theme::accent : Theme::dimText.withAlpha (0.3f));
            g.fillPath (star);
        }

        return;
    }

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
    q.favouritesOnly = filterFavourites;
    q.minRating = filterRating;
    q.tag = filterTag;
    q.within = filterWithin;
    q.formats = shownFormats;
    q.foldersLeftOut = foldersLeftOut;
    return q;
}

void SoundLibraryView::setPaneWidth (int width)
{
    paneWidth = juce::jmax (0, width);
    resized();
}

void SoundLibraryView::showFolder (const juce::File& folder)
{
    filterWithin = folder;
    refresh();
}

void SoundLibraryView::showPane (Pane pane)
{
    // Each pane chooses on its own: changing over shows every sound again.
    const auto folders = pane == Pane::folders;
    byFolderButton.setToggleState (folders, juce::dontSendNotification);
    byCategoryButton.setToggleState (! folders, juce::dontSendNotification);
    folderTree->setVisible (folders);
    categories->setVisible (! folders);

    if (folders)
    {
        categories->chooseAll();
        filterCategory = filterCatId = {};
        filterWithout = false;
    }
    else
    {
        folderTree->chooseAll();
        filterWithin = juce::File();
    }

    refresh();
}

SoundLibraryView::Pane SoundLibraryView::getPane() const
{
    return folderTree->isVisible() ? Pane::folders : Pane::categories;
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

void SoundLibraryView::cellClicked (int row, int column, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        showCategoryMenu();
    else if (column == rating || column == favourite)
        clickCell (row, (Column) column, juce::roundToInt (e.getEventRelativeTo (&table).position.x) - table.getCellPosition (column, row, true).getX());
}

void SoundLibraryView::clickCell (int row, Column column, int x)
{
    const auto* s = soundAt (row);

    if (s == nullptr)
        return;

    if (column == favourite)
        index.setFavourite ({ s->id }, ! s->favourite);
    else if (column == rating)
    {
        const auto stars = juce::jlimit (1, 5, (x - 6) / starWidth + 1);
        index.setRating ({ s->id }, stars == s->rating ? 0 : stars);
    }

    refresh (true);
}

std::vector<juce::int64> SoundLibraryView::selectedIds()
{
    std::vector<juce::int64> ids;
    const auto selected = table.getSelectedRows();

    for (int i = 0; i < selected.size(); ++i)
        if (const auto* s = soundAt (selected[i]))
            ids.push_back (s->id);

    return ids;
}

bool SoundLibraryView::setRatingOfSelection (int stars)
{
    const auto ids = selectedIds();
    const auto done = ! ids.empty() && index.setRating (ids, stars);
    refresh (true);
    return done;
}

bool SoundLibraryView::setFavouriteOfSelection (bool on)
{
    const auto ids = selectedIds();
    const auto done = ! ids.empty() && index.setFavourite (ids, on);
    refresh (true);
    return done;
}

bool SoundLibraryView::addTagToSelection (const juce::String& tag)
{
    const auto ids = selectedIds();
    const auto done = ! ids.empty() && index.addTag (ids, tag);
    refresh (true);
    return done;
}

bool SoundLibraryView::removeTagFromSelection (const juce::String& tag)
{
    const auto ids = selectedIds();
    const auto done = ! ids.empty() && index.removeTag (ids, tag);
    refresh (true);
    return done;
}

void SoundLibraryView::showFavouritesOnly (bool on)
{
    filterFavourites = on;
    favouritesButton.setToggleState (on, juce::dontSendNotification);
    refresh();
}

void SoundLibraryView::showMinRating (int stars)
{
    filterRating = juce::jlimit (0, 5, stars);
    ratingChoice.setSelectedId (filterRating + 1, juce::dontSendNotification);
    refresh();
}

void SoundLibraryView::showTag (const juce::String& tag)
{
    filterTag = tag;
    refresh();
}

void SoundLibraryView::showFormats (const juce::StringArray& formatNames)
{
    juce::StringArray kept;

    for (const auto& choice : SoundLibrary::formatChoices())
        if (formatNames.contains (choice.name))
            kept.add (choice.name);

    shownFormats = kept.isEmpty() ? juce::StringArray { "WAV file" } : kept;
    formatsButton.setButtonText (describeShownFormats());
    refresh();
}

juce::String SoundLibraryView::describeShownFormats() const
{
    const auto choices = SoundLibrary::formatChoices();
    juce::StringArray labels;

    for (const auto& choice : choices)
        if (shownFormats.contains (choice.name))
            labels.add (choice.label);

    if (labels.size() == (int) choices.size())
        return "All Formats";

    if (labels.size() <= 2)
        return labels.joinIntoString (", ");

    return juce::String (labels.size()) + " Formats";
}

void SoundLibraryView::toggleFormat (const juce::String& formatName)
{
    auto wanted = shownFormats;

    if (formatName.isEmpty())
    {
        wanted.clear();

        for (const auto& choice : SoundLibrary::formatChoices())
            wanted.add (choice.name);
    }
    else if (! wanted.contains (formatName))
    {
        wanted.add (formatName);
    }
    else if (wanted.size() > 1)
    {
        wanted.removeString (formatName);
    }
    else
    {
        return;   // the last format shown stays: a list of nothing helps no one
    }

    showFormats (wanted);

    if (onShownFormatsChanged != nullptr)
        onShownFormatsChanged (shownFormats);
}

void SoundLibraryView::setFoldersLeftOut (const juce::Array<juce::File>& folders)
{
    foldersLeftOut.clear();

    for (const auto& f : folders)
        if (f != juce::File())
            foldersLeftOut.addIfNotAlreadyThere (f);

    refresh();
}

void SoundLibraryView::leaveOutFolder (const juce::File& folder)
{
    if (folder == juce::File() || foldersLeftOut.contains (folder))
        return;

    auto wanted = foldersLeftOut;
    wanted.add (folder);
    setFoldersLeftOut (wanted);

    if (onFoldersLeftOutChanged != nullptr)
        onFoldersLeftOutChanged (foldersLeftOut);
}

void SoundLibraryView::showFolderAgain (const juce::File& folder)
{
    if (! foldersLeftOut.contains (folder))
        return;

    auto wanted = foldersLeftOut;
    wanted.removeFirstMatchingValue (folder);
    setFoldersLeftOut (wanted);

    if (onFoldersLeftOutChanged != nullptr)
        onFoldersLeftOutChanged (foldersLeftOut);
}

void SoundLibraryView::showFormatsMenu()
{
    juce::PopupMenu menu;
    const auto choices = SoundLibrary::formatChoices();
    menu.addItem (1, "All Formats", true, shownFormats.size() == (int) choices.size());
    menu.addSeparator();

    for (size_t i = 0; i < choices.size(); ++i)
    {
        const auto shown = shownFormats.contains (choices[i].name);
        menu.addItem ((int) i + 2, choices[i].label + (choices[i].detail.isEmpty() ? juce::String() : " (" + choices[i].detail + ")"),
                      ! (shown && shownFormats.size() == 1), shown);
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&formatsButton),
                        [safe = juce::Component::SafePointer<SoundLibraryView> (this), choices] (int result)
    {
        if (safe == nullptr || result == 0)
            return;

        if (result == 1)
            safe->toggleFormat ({});
        else if (juce::isPositiveAndBelow (result - 2, (int) choices.size()))
            safe->toggleFormat (choices[(size_t) (result - 2)].name);
    });
}

juce::StringArray SoundLibraryView::getTagChoices() const
{
    juce::StringArray choices;

    for (int i = 0; i < tagChoice.getNumItems(); ++i)
        choices.add (tagChoice.getItemText (i));

    return choices;
}

void SoundLibraryView::updateTagChoices()
{
    juce::StringArray wanted { "Any Tag" };
    auto chosen = 1;

    for (size_t i = 0; i < tagCounts.size(); ++i)
    {
        wanted.add (tagCounts[i].first + " (" + juce::String (tagCounts[i].second) + ")");

        if (tagCounts[i].first.equalsIgnoreCase (filterTag))
            chosen = (int) i + 2;
    }

    if (wanted != getTagChoices())
    {
        tagChoice.clear (juce::dontSendNotification);
        tagChoice.addItemList (wanted, 1);
    }

    tagChoice.setSelectedId (chosen, juce::dontSendNotification);
}

juce::StringArray SoundLibraryView::getFilesToInsert()
{
    auto files = getSelectedFiles();
    const auto part = preview.getSelection();

    if (files.size() == 1 && ! part.isEmpty() && juce::File (files[0]) == preview.getPlayer().getFile())
    {
        const auto written = SoundLibraryPreview::writePart (preview.getPlayer().getFile(), part, preview.getPartsFolder());
        return written != juce::File() ? juce::StringArray { written.getFullPathName() } : juce::StringArray();
    }

    return files;
}

bool SoundLibraryView::insertSelection()
{
    if (onInsert == nullptr)
        return false;

    const auto files = getFilesToInsert();
    return ! files.isEmpty() && onInsert (files);
}

juce::String SoundLibraryView::Written::describe() const
{
    auto line = "Wrote " + juce::String (written) + (written == 1 ? " file" : " files");

    if (skipped > 0)
        line << ", skipped " << skipped << " with no category or tag";

    if (failed > 0)
        line << ", " << failed << " not written (" << firstFailure << (failed > 1 ? ", and others" : "") << ")";

    return line;
}

SoundLibraryView::Written SoundLibraryView::writeIntoFiles (const std::vector<SoundLibrary::Sound>& sounds, const std::function<bool()>& keepGoing)
{
    Written w;

    for (const auto& s : sounds)
    {
        if (keepGoing != nullptr && ! keepGoing())
            break;

        if (s.catId.isEmpty() && s.tags.isEmpty())
        {
            ++w.skipped;
            continue;
        }

        if (const auto why = SoundMetadata::write (s.file, { s.catId, s.tags }); why.isEmpty())
            ++w.written;
        else if (++w.failed == 1)
            w.firstFailure = s.file.getFileName() + ": " + why;
    }

    return w;
}

void SoundLibraryView::writeSelectionIntoFiles()
{
    if (writing.exchange (true))
        return;

    std::vector<SoundLibrary::Sound> sounds;
    const auto selected = table.getSelectedRows();

    for (int i = 0; i < selected.size(); ++i)
        if (const auto* s = soundAt (selected[i]))
            sounds.push_back (*s);

    struct Job final : public juce::Thread
    {
        Job (SoundLibraryView& v, std::vector<SoundLibrary::Sound> s) : juce::Thread ("Sound Library writing"), view (&v), sounds (std::move (s)) {}

        void run() override
        {
            const auto result = writeIntoFiles (sounds, [this] { return ! threadShouldExit(); });
            juce::MessageManager::callAsync ([v = view, result]
            {
                if (v == nullptr)
                    return;

                v->scanNote = result.describe();
                v->writtenNote = v->scanNote;
                v->writing = false;
                v->rescan();
            });
        }

        juce::Component::SafePointer<SoundLibraryView> view;
        std::vector<SoundLibrary::Sound> sounds;
    };

    if (writer != nullptr)
        writer->stopThread (-1);

    scanNote = "Writing into " + juce::String ((int) sounds.size()) + " files...";
    updateSummary();
    writer = std::make_unique<Job> (*this, std::move (sounds));
    writer->startThread();
}

void SoundLibraryView::askToWriteIntoFiles()
{
    const auto many = table.getNumSelectedRows();
    juce::AlertWindow::showAsync (juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle ("Write into Files")
                                      .withMessage ("Write the categories and tags of " + juce::String (many) + (many == 1 ? " sound" : " sounds")
                                                    + " into their files?\n\nOnly WAV files are written, in their iXML. Each is rewritten through a "
                                                      "checked copy that replaces it only when whole; its audio and other chunks are kept byte for byte.")
                                      .withButton ("Write").withButton ("Cancel").withAssociatedComponent (this),
                                  [safe = juce::Component::SafePointer<SoundLibraryView> (this)] (int result)
                                  {
                                      if (safe != nullptr && result == 1)
                                          safe->writeSelectionIntoFiles();
                                  });
}

void SoundLibraryView::askForTag()
{
    auto* window = new juce::AlertWindow ("Add Tag", "A tag for the selected sounds:", juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor ("tag", {}, {});
    window->addButton ("Add", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (true, juce::ModalCallbackFunction::create ([this, window] (int result)
    {
        if (result == 1)
            addTagToSelection (window->getTextEditorContents ("tag"));
    }), true);
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
    juce::PopupMenu ratings, removeTags;
    ratings.addItem (10, "No Rating");

    for (int stars = 1; stars <= 5; ++stars)
        ratings.addItem (10 + stars, juce::String (stars) + (stars == 1 ? " Star" : " Stars"));

    juce::StringArray selectedTags;
    const auto chosenRows = table.getSelectedRows();

    for (int i = 0; i < chosenRows.size(); ++i)
        if (const auto* s = soundAt (chosenRows[i]))
            for (const auto& t : s->tags)
                if (! selectedTags.contains (t, true))
                    selectedTags.add (t);

    selectedTags.sortNatural();

    for (int i = 0; i < selectedTags.size(); ++i)
        removeTags.addItem (40 + i, selectedTags[i]);

    menu.addSubMenu ("Rating", ratings);
    menu.addItem (20, "Mark as Favourite");
    menu.addItem (21, "Not a Favourite");
    menu.addItem (30, "Add Tag...");
    menu.addSubMenu ("Remove Tag", removeTags, ! selectedTags.isEmpty());
    menu.addSeparator();
    menu.addSubMenu ("Set Category", set);
    menu.addItem (1, "Clear Category Set by Hand");
    menu.addSeparator();
    menu.addItem (60, "Insert at Cursor", onInsert != nullptr);
    menu.addSeparator();
    juce::File rowFolder;

    if (const auto* s = chosenRows.size() > 0 ? soundAt (chosenRows[0]) : nullptr)
        rowFolder = s->file.getParentDirectory();

    menu.addItem (70, "Leave \"" + rowFolder.getFileName() + "\" Out of the List", rowFolder != juce::File() && ! foldersLeftOut.contains (rowFolder));
    menu.addSeparator();
    menu.addItem (50, "Write Category and Tags into Files...", ! writing);
    menu.showMenuAsync (juce::PopupMenu::Options(), [this, selectedTags, rowFolder] (int result)
    {
        if (result == 70)
            leaveOutFolder (rowFolder);
        else if (result >= 10 && result <= 15)
            setRatingOfSelection (result - 10);
        else if (result == 20 || result == 21)
            setFavouriteOfSelection (result == 20);
        else if (result == 30)
            askForTag();
        else if (result == 50)
            askToWriteIntoFiles();
        else if (result == 60)
            insertSelection();
        else if (juce::isPositiveAndBelow (result - 40, selectedTags.size()))
            removeTagFromSelection (selectedTags[result - 40]);
        else if (result == 1)
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
        sub.addItem (1000 + i, Platform::showInFileManager());
        sub.addItem (2000 + i, "Remove from the Sound Library", ! scanner->busy());
        menu.addSubMenu (folders[i].getFullPathName(), sub);
    }

    const auto leftOut = foldersLeftOut;

    if (! leftOut.isEmpty())
    {
        menu.addSeparator();
        menu.addSectionHeader ("Left out of the list");

        for (int i = 0; i < leftOut.size(); ++i)
        {
            juce::PopupMenu sub;
            sub.addItem (3000 + i, "Show in the List Again");
            menu.addSubMenu (leftOut[i].getFullPathName(), sub);
        }
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&foldersButton), [this, folders, leftOut] (int result)
    {
        if (juce::isPositiveAndBelow (result - 3000, leftOut.size()))
            showFolderAgain (leftOut[result - 3000]);
        else if (result == 1)
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
    auto filters = r.removeFromTop (26).reduced (4, 2);
    favouritesButton.setBounds (filters.removeFromLeft (100));
    filters.removeFromLeft (6);
    ratingChoice.setBounds (filters.removeFromLeft (150));
    filters.removeFromLeft (6);
    tagChoice.setBounds (filters.removeFromLeft (220));
    filters.removeFromLeft (6);
    formatsButton.setBounds (filters.removeFromLeft (150));
    summary.setBounds (r.removeFromBottom (24).reduced (4, 0));
    preview.setBounds (r.removeFromBottom (juce::jlimit (110, 220, r.getHeight() / 3)));
    const auto widest = juce::jmax (120, r.getWidth() - 320);
    auto left = r.removeFromLeft (juce::jlimit (120, widest, paneWidth > 0 ? paneWidth : juce::jlimit (170, 280, r.getWidth() / 5)));
    divider.setBounds (r.removeFromLeft (6));
    auto tabs = left.removeFromTop (26).reduced (4, 2);
    byFolderButton.setBounds (tabs.removeFromLeft (tabs.getWidth() / 2));
    byCategoryButton.setBounds (tabs);
    categories->setBounds (left);
    folderTree->setBounds (left);
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
