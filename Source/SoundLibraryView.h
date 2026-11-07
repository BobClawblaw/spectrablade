#pragma once

#include "SoundLibrary.h"
#include "SoundLibraryPreview.h"

/** The Sound Library window's contents: the folders you choose to scan (Add Folder..., and the
    Folders menu to rescan, remove or show one), a search box, and the sounds found, a page at a
    time from the index, in a table to sort by clicking its headers.

    Nothing is scanned until a folder is added; adding one scans it, and Rescan brings every
    folder up to date. Scans run on a thread of their own with their own connection to the index,
    and the table and the line below it follow them as they go.

    Dragging selected rows carries their files out as a file drag, as dragging them from the
    Finder does: into the main window's multitrack view or editor, the Finder or another app.
    Double-clicking a row asks for the sound to be opened.

    On the left, the UCS categories: all sounds, those with none, and each category (opening into
    its subcategories), with how many of the sounds found are in each; choosing one shows only
    those. Right-clicking rows sets their category by hand, or clears what was set by hand.

    Your own ratings, favourites and tags: clicking a row's stars rates it (its own rating again
    clears it) and its heart marks it a favourite; with the list in front, the keys 0 to 5 rate
    the selected sounds and F marks them; the right-click menu rates, marks and tags them. The bar
    above the list shows only favourites, only sounds rated at least so many stars, or only those
    with a tag.

    The right-click menu also writes the selected sounds' categories and tags into their WAV files
    (see SoundMetadata.h), after asking, on a thread of its own; the folders are rescanned after.

    Below the table, the preview shows the sound last selected (see SoundLibraryPreview); the space
    bar plays and stops it, unless the search box is typed in. Parts dragged out of the preview are
    written beside the index, in its Parts folder. */
class SoundLibraryView final : public juce::Component,
                               private juce::TableListBoxModel,
                               private juce::Timer
{
public:
    explicit SoundLibraryView (const juce::File& database);
    ~SoundLibraryView() override;

    enum Column { name = 1, length, rate, channels, bits, format, location, description, category, favourite, rating, tags };

    /** Adds a folder and scans it: false, with the reason in the summary, when the index refuses it. */
    bool addFolder (const juce::File&);
    bool removeFolder (const juce::File&);
    juce::Array<juce::File> getFolders()                { return index.getFolders(); }

    /** Brings every folder up to date, on the scanning thread (after any scan running now). */
    void rescan();
    bool isScanning() const;

    /** Shows only the sounds of a category (its name), or of a subcategory (a CatID), or with no
        category, as choosing it on the left does; all empty and false: every sound. */
    void showCategory (const juce::String& category, const juce::String& catId = {}, bool withoutCategory = false);

    /** How many of the sounds found (by the search) have this CatID ("" for none), or are in this
        category, as the list on the left shows. */
    juce::int64 countWithCatId (const juce::String& catId) const;
    juce::int64 countInCategory (const juce::String& category) const;
    juce::int64 found() const;          // the sounds the search finds, in any category or none

    /** Sets the selected sounds' category by hand (a CatID), or clears it with an empty one, as
        the rows' right-click menu does. */
    bool setCategoryOfSelection (const juce::String& catId);

    /** Rates, marks and tags the selected sounds, as the stars, heart, keys and menu do. */
    bool setRatingOfSelection (int stars);
    bool setFavouriteOfSelection (bool);
    bool addTagToSelection (const juce::String&);
    bool removeTagFromSelection (const juce::String&);

    /** The filters above the list: only favourites, at least so many stars, only one tag (empty: any). */
    void showFavouritesOnly (bool);
    void showMinRating (int stars);
    void showTag (const juce::String&);

    /** What the tag filter offers: "Any Tag", then each tag with its count. */
    juce::StringArray getTagChoices() const;

    /** What writing categories and tags into files did. */
    struct Written
    {
        int written = 0, skipped = 0, failed = 0;
        juce::String firstFailure;      // "name: why"
        juce::String describe() const;
    };

    /** Writes each sound's category and tags into its file (those with neither are skipped),
        asking before each file whether to go on. */
    static Written writeIntoFiles (const std::vector<SoundLibrary::Sound>&, const std::function<bool()>& keepGoing = {});

    /** Writes the selected sounds' categories and tags into their files, on a thread of its own,
        as the menu does once it is answered; the line below the list says what it did. */
    void writeSelectionIntoFiles();
    bool isWriting() const                              { return writing.load(); }

    /** Clicking a cell, as the mouse does (the x is from the cell's left edge). */
    void clickCell (int row, Column, int x);

    /** Types into the search box, as the keyboard does. */
    void setSearch (const juce::String&);

    /** Sorts by a column, as clicking its header does (bits and description don't sort). */
    void setSort (Column, bool forwards);

    int getNumRows() override;
    juce::String getText (int row, int column);
    const SoundLibrary::Sound* soundAt (int row);

    /** Selects rows, as clicking and shift-clicking does; the files of those selected. */
    void selectRows (const juce::SparseSet<int>&);
    juce::StringArray getSelectedFiles();

    /** "1,234 sounds in 2 folders", "12 of 1,234 sounds match", "No folders yet: ..." or how a scan is going. */
    juce::String getSummary() const                     { return summary.getText(); }

    /** Called with the files being dragged out; by default they go as a file drag. */
    std::function<void (const juce::StringArray&)> onDragOut;

    /** Called with a sound's file when its row is double-clicked. */
    std::function<void (const juce::File&)> onOpen;

    SoundLibraryPreview& getPreview() noexcept          { return preview; }

    /** Plays the preview through this device (alongside whatever else plays through it); null to stop. */
    void setAudioDevice (juce::AudioDeviceManager*);

    bool keyPressed (const juce::KeyPress&) override;

    void resized() override;

private:
    class Scanner;
    class Categories;
    SoundLibrary::Query query (int offset = 0, int limit = 0) const;
    void showCategoryMenu();
    std::vector<juce::int64> selectedIds();
    void updateTagChoices();
    void askForTag();
    static constexpr int starWidth = 14;
    void askToWriteIntoFiles();
    std::atomic<bool> writing { false };
    juce::String writtenNote;   // what writing into files did, said with the scan after it
    std::unique_ptr<juce::Thread> writer;

    void paintRowBackground (juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    void cellDoubleClicked (int row, int column, const juce::MouseEvent&) override;
    void cellClicked (int row, int column, const juce::MouseEvent&) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void sortOrderChanged (int column, bool forwards) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void timerCallback() override;

    void refresh (bool keepSelection = false);
    void updateSummary();
    void chooseFolder();
    void showFoldersMenu();

    SoundLibrary::Index index;
    juce::File database;
    std::unique_ptr<Scanner> scanner;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::String text, filterCategory, filterCatId;
    bool filterWithout = false, filterFavourites = false;
    int filterRating = 0;
    juce::String filterTag;
    std::vector<std::pair<juce::String, juce::int64>> tagCounts;
    std::map<juce::String, juce::int64> counts;               // by CatID, of the sounds the search finds
    std::map<juce::String, juce::int64> categoryCounts;       // by category
    SoundLibrary::Column sortBy = SoundLibrary::Column::name;
    bool forwards = true;
    juce::int64 rows = 0, total = 0;
    std::map<int, std::vector<SoundLibrary::Sound>> pages;   // the pages read, by number
    juce::String scanNote;                                    // what the last scan did, or why a folder was refused
    bool dragStarted = false, dragFromSelection = false;
    int ticks = 0;
    bool refreshing = false;   // the selection changing because the rows did, not by choice

    juce::TextEditor search;
    juce::TextButton addButton { "Add Folder..." }, foldersButton { "Folders" };
    juce::ToggleButton favouritesButton { "Favourites" };
    juce::ComboBox ratingChoice, tagChoice;
    juce::TableListBox table { {}, this };
    std::unique_ptr<Categories> categories;
    juce::Label summary;
    SoundLibraryPreview preview;
    juce::AudioSourcePlayer previewPlayer;
    juce::AudioDeviceManager* device = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundLibraryView)
};

/** The Sound Library in a window of its own. Closing it hides it; the scan, if any, goes on. */
class SoundLibraryWindow final : public juce::DocumentWindow
{
public:
    explicit SoundLibraryWindow (const juce::File& database);

    SoundLibraryView& getView()                         { return *static_cast<SoundLibraryView*> (getContentComponent()); }
    void closeButtonPressed() override                  { setVisible (false); }
};
