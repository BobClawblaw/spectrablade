#pragma once

#include "AudioEngine.h"

/** Where the editor was in a file: the selection, cursor, zoom and spectral band, put back when
    the file is shown again. A zero framesPerPixel means none was kept (the file is fitted). */
struct FilePlace
{
    juce::Range<juce::int64> selection;
    juce::int64 cursor = 0;
    double viewStart = 0.0, framesPerPixel = 0.0;
    juce::Range<double> band;
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

    /** The open files with unsaved changes, by index. */
    juce::Array<int> unsaved() const;

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
    unsaved ones dotted. Click one to switch to it; its × (or right-click) closes it. */
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

    void resized() override;
    void paint (juce::Graphics&) override;

    static constexpr int rowHeight = 40;

private:
    int getNumRows() override                       { return files.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    juce::Rectangle<int> closeBounds (int width) const   { return { width - 26, (rowHeight - 18) / 2, 18, 18 }; }

    OpenFiles& files;
    juce::TextButton openButton { "Open..." };
    juce::ListBox list { {}, this };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilesPanel)
};
