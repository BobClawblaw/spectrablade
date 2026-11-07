#pragma once

#include <JuceHeader.h>

struct sqlite3;

/** The Sound Library's index: the audio files in the folders it is given to watch (only those:
    it has none until one is added, and never scans anywhere else), kept in an SQLite
    database so a library of a million files and more can be searched and listed a page at a time.

    A scan reads only each file's header (its format, length, rate, channels, bit depth and BWF
    description) and only for files that are new or whose size or modification time changed;
    files gone from a watched folder leave the index. Searching matches the start of words in a
    sound's name (split at case changes and digits too, so "wood" finds "DOORWood_Creak") and its
    description; every word searched for must be found.

    One Index is one connection to the database, to be used on one thread at a time: a scan runs
    on an Index of its own on the same file while another lists and searches (the database is in
    WAL mode, so they don't block each other). */
namespace SoundLibrary
{
    struct Sound
    {
        juce::int64 id = 0;
        juce::File file;
        juce::String name;              // the file name without its extension
        juce::File folder;              // the watched folder it is in
        juce::int64 size = 0;           // bytes
        juce::int64 modified = 0;       // ms since 1970
        juce::int64 frames = 0;
        double sampleRate = 0.0;
        int channels = 0;
        int bitDepth = 0;
        juce::String format;            // the reader's format name, e.g. "WAV file"
        juce::String description;       // the BWF description, where there is one

        double seconds() const noexcept  { return sampleRate > 0.0 ? (double) frames / sampleRate : 0.0; }
    };

    enum class Column { name, folder, length, sampleRate, channels, format, modified };

    struct Query
    {
        juce::String text;              // words to find; empty: every sound
        Column sortBy = Column::name;
        bool ascending = true;
        int offset = 0, limit = 100;
    };

    struct ScanResult
    {
        int added = 0, updated = 0, unchanged = 0, removed = 0, unreadable = 0;
        bool stopped = false;
    };

    /** The words a name is searched by: split at anything not a letter or digit, at a lower-case
        letter followed by a capital, and between letters and digits ("DOORWood_Creak 02" gives
        "DOOR Wood Creak 02"; a capital after capitals starts a word when a small letter follows). */
    juce::StringArray wordsOf (const juce::String& name);

    class Index
    {
    public:
        /** Opens the database at this file, making it (and its folder) when it isn't there. */
        explicit Index (const juce::File& database);
        ~Index();

        bool isOpen() const noexcept            { return db != nullptr; }
        juce::String getError() const           { return error; }

        /** Adds a folder to watch: false (with the reason in getError()) when it doesn't exist,
            is already watched, or is inside or around one that is. Its files come in with the next scan. */
        bool addFolder (const juce::File&);

        /** Stops watching a folder, and its sounds leave the index. */
        bool removeFolder (const juce::File&);
        juce::Array<juce::File> getFolders();

        /** Brings the index up to date with the watched folders. progress is called at the first
            file and every 200 after, with the files looked at so far, and returns false to stop
            (what was done so far is kept, and nothing is removed). */
        ScanResult scan (juce::AudioFormatManager&, const std::function<bool (int)>& progress = {});

        /** One page of the sounds matching the query, in its order. */
        std::vector<Sound> find (const Query&);

        /** How many sounds match these words (all of them for none). */
        juce::int64 count (const juce::String& text = {});

        /** Puts a sound in the index, or replaces the one at its file (what a scan does for each file). */
        bool put (Sound&);

        /** Puts many, in one transaction. */
        bool putAll (std::vector<Sound>&);

    private:
        sqlite3* db = nullptr;
        juce::String error;

        bool exec (const char* sql);
        bool remove (juce::int64 id);

        JUCE_DECLARE_NON_COPYABLE (Index)
    };
}
