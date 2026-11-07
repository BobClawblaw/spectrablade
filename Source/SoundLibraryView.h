#pragma once

#include "SoundLibrary.h"

/** The Sound Library window's contents: the folders you choose to scan (Add Folder..., and the
    Folders menu to rescan, remove or show one), a search box, and the sounds found, a page at a
    time from the index, in a table to sort by clicking its headers.

    Nothing is scanned until a folder is added; adding one scans it, and Rescan brings every
    folder up to date. Scans run on a thread of their own with their own connection to the index,
    and the table and the line below it follow them as they go.

    Dragging selected rows carries their files out as a file drag, as dragging them from the
    Finder does: into the main window's multitrack view or editor, the Finder or another app.
    Double-clicking a row asks for the sound to be opened. */
class SoundLibraryView final : public juce::Component,
                               private juce::TableListBoxModel,
                               private juce::Timer
{
public:
    explicit SoundLibraryView (const juce::File& database);
    ~SoundLibraryView() override;

    enum Column { name = 1, length, rate, channels, bits, format, location, description };

    /** Adds a folder and scans it: false, with the reason in the summary, when the index refuses it. */
    bool addFolder (const juce::File&);
    bool removeFolder (const juce::File&);
    juce::Array<juce::File> getFolders()                { return index.getFolders(); }

    /** Brings every folder up to date, on the scanning thread (after any scan running now). */
    void rescan();
    bool isScanning() const;

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

    void resized() override;

private:
    class Scanner;

    void paintRowBackground (juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell (juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    void cellDoubleClicked (int row, int column, const juce::MouseEvent&) override;
    void sortOrderChanged (int column, bool forwards) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void timerCallback() override;

    void refresh();
    void updateSummary();
    void chooseFolder();
    void showFoldersMenu();

    SoundLibrary::Index index;
    juce::File database;
    std::unique_ptr<Scanner> scanner;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::String text;
    SoundLibrary::Column sortBy = SoundLibrary::Column::name;
    bool forwards = true;
    juce::int64 rows = 0, total = 0;
    std::map<int, std::vector<SoundLibrary::Sound>> pages;   // the pages read, by number
    juce::String scanNote;                                    // what the last scan did, or why a folder was refused
    bool dragStarted = false, dragFromSelection = false;
    int ticks = 0;

    juce::TextEditor search;
    juce::TextButton addButton { "Add Folder..." }, foldersButton { "Folders" };
    juce::TableListBox table { {}, this };
    juce::Label summary;

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
