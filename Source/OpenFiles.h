#pragma once

#include "SpectralEdit.h"
#include "AudioEngine.h"

/** Where the editor was in a file: the selection, cursor, zoom and spectral band, put back when
    the file is shown again. A zero framesPerPixel means none was kept (the file is fitted). */
struct FilePlace
{
    juce::Range<juce::int64> selection;
    juce::int64 cursor = 0;
    double viewStart = 0.0, framesPerPixel = 0.0;
    juce::Range<double> band;
    SpectralEdit::Shape shape;   // a lasso or brush drawing, when there is one
};

/** The files open in the editor. One is in the engine's document, the others are kept aside, each
    with its own undo history and place, until switched to. */
class OpenFiles final : public juce::ChangeBroadcaster,
                        private juce::ChangeListener
{
public:
    explicit OpenFiles (AudioEngine&);
    ~OpenFiles() override;

    int size() const                                   { return (int) entries.size(); }
    int getActiveIndex() const                         { return active; }

    /** An open file's document: the engine's for the one in the editor. */
    const AudioDocument& documentAt (int index) const;
    AudioDocument& documentAt (int index);

    /** Where the editor was in a file kept aside (for the one shown, ask the editor). */
    const FilePlace& placeAt (int index) const          { return entries[(size_t) index].place; }

    /** The name shown for a file: its file name, or "Untitled" for one never saved. */
    juce::String nameAt (int index) const;

    /** The open file read from this file on disk, or -1. */
    int indexOf (const juce::File&) const;

    /** Makes room for a file about to be loaded into the engine's document: the one there is kept
        aside behind a new entry, which becomes the active one, unless it is empty, untitled and
        unchanged (then it is simply replaced). `place` is where the editor was in it. */
    void prepareForAnother (const FilePlace& place);

    /** Switches to another open file, keeping `place` for the one leaving. Returns the place kept
        for the one arriving, or nothing when the index is the active one or out of range. */
    std::optional<FilePlace> activate (int index, const FilePlace& place);

    /** Closes a file, changes and all (ask about them first). Closing the active one brings in its
        neighbour below, or above at the end, and returns its place; closing the last one leaves an
        empty document. */
    std::optional<FilePlace> close (int index);

    /** Closes every file, changes and all (ask about them first), leaving an empty document. */
    void closeAll();

    /** The open files with unsaved changes, by index. */
    juce::Array<int> unsaved() const;

    /** Adds an open file's audio, as it is in the editor (unsaved changes and all), as a clip at
        the position on a track of a session state (a new track when the track is past the last),
        named after the file: an empty session takes the file's sample rate, otherwise the audio
        is converted to the session's. The clip's file is the one on disk, so a saved session
        reloads the file as saved there (none for a file never saved). Returns the clip's id, or
        0 for an empty file or an index out of range. */
    juce::uint32 addToSession (SessionState&, int index, int track, juce::int64 position) const;

private:
    struct Entry
    {
        std::unique_ptr<AudioDocument> aside;   // the file's document while another is active; an empty one while it is
        FilePlace place;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override   { sendChangeMessage(); }
    bool isBlank (const AudioDocument&) const;

    AudioEngine& engine;
    std::vector<Entry> entries;
    int active = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OpenFiles)
};

/** The Files panel: every open file with its length and format, the one in the editor marked and
    unsaved ones dotted. Click one to switch to it; its × (or right-click) closes it. Drag one
    into the multitrack view to add it there as a clip, on the track it lands on or a new one. */
class FilesPanel final : public juce::Component,
                         private juce::ListBoxModel,
                         private juce::ChangeListener
{
public:
    explicit FilesPanel (OpenFiles&);
    ~FilesPanel() override;

    std::function<void (int)> onChoose, onClose;
    std::function<void()> onOpen;

    /** The text a row shows: its name with a dot when unsaved, then its length and format. */
    juce::String describeRow (int row) const;

    /** Clicks a row (on its × when close is true), as the mouse does. */
    void clickRow (int row, bool close);

    /** What a drag of a row carries (the multitrack view takes it as the file to add), and the
        row a drag's description names, or -1 when the drag is not a row's. */
    static juce::var dragDescription (int row);
    static int rowOfDrag (const juce::var& description);

    void resized() override;
    void paint (juce::Graphics&) override;

    static constexpr int rowHeight = 40;

private:
    int getNumRows() override                       { return files.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    juce::Rectangle<int> closeBounds (int width) const   { return { width - 26, (rowHeight - 18) / 2, 18, 18 }; }

    OpenFiles& files;
    juce::TextButton openButton { "Open..." };
    juce::ListBox list { {}, this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilesPanel)
};

/** The open files as tabs over the editor: each with its name (dotted while unsaved) and a × to
    close it, the one showing marked with the accent. Click a tab to show its file. The tabs
    share out the width, shrinking when there are many. */
class FileTabs final : public juce::Component,
                       public juce::SettableTooltipClient,
                       private juce::ChangeListener
{
public:
    explicit FileTabs (OpenFiles&);
    ~FileTabs() override;

    std::function<void (int)> onChoose, onClose;

    /** Whether there is a file to show a tab for (none while the editor is empty). */
    bool hasTabs() const;

    juce::Rectangle<int> getTabBounds (int index) const;
    juce::Rectangle<int> getCloseBounds (int index) const;
    int tabAt (juce::Point<int>) const;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    static constexpr int height = 24;
    static constexpr int widest = 220, narrowest = 70;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    int tabWidth (int index) const;

    OpenFiles& files;
    int hovered = -1, pressed = -1;
    bool pressedClose = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FileTabs)
};
