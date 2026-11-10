#pragma once

#include <JuceHeader.h>

struct sqlite3;

/** The Sound Library's index: the audio files in the folders it is given to watch (only those:
    it has none until one is added, and never scans anywhere else), kept in an SQLite
    database so a library of a million files and more can be searched and listed a page at a time.

    A scan reads only each file's header (its format, length, rate, channels, bit depth, BWF
    description and, in a WAV file, its iXML) and only for files that are new or whose size or modification time changed;
    files gone from a watched folder leave the index. Searching matches the start of words in a
    sound's name (split at case changes and digits too, so "wood" finds "DOORWood_Creak") and its
    description, and its UCS category's names; every word searched for must be found.

    Your own tags, a rating (none, or one to five stars) and a favourite mark are kept in the index
    with each sound: they stay through rescans (a changed file keeps them; a file gone from its
    folder leaves with them), tags are searched like words of the name, and each can filter.

    Each sound has a UCS category (see SoundCategories.h) when its name or iXML gives one, or one
    is set by hand; one set by hand stays through rescans until it is cleared.

    One Index is one connection to the database, to be used on one thread at a time: a scan runs
    on an Index of its own on the same file while another lists and searches (the database is in
    WAL mode, so they don't block each other). */
namespace SoundLibrary
{
    /** Where a sound's category came from. */
    enum class CategoryFrom { none = 0, name = 1, metadata = 2, hand = 3 };

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
        juce::String catId;             // its UCS CatID (e.g. DOORWood); empty for none
        CategoryFrom categoryFrom = CategoryFrom::none;
        int rating = 0;                 // 0 (none) to 5 stars
        bool favourite = false;
        juce::StringArray tags;         // in the order of their names

        double seconds() const noexcept  { return sampleRate > 0.0 ? (double) frames / sampleRate : 0.0; }
    };

    enum class Column { name, folder, length, sampleRate, channels, format, modified, category, rating };

    struct Query
    {
        juce::String text;              // words to find; empty: every sound
        Column sortBy = Column::name;
        bool ascending = true;
        int offset = 0, limit = 100;

        /** Only the sounds of one subcategory (a CatID), or of a whole category (its name), or
            only those with no category. */
        juce::String catId {}, category {};
        bool withoutCategory = false;

        /** Only sounds rated at least this many stars, only favourites, only those with this tag. */
        int minRating = 0;
        bool favouritesOnly = false;
        juce::String tag {};

        /** Only the sounds in this folder or any folder inside it (none: anywhere). */
        juce::File within {};

        /** Only sounds of these formats (their names as a scan keeps them, e.g. "WAV file"; see
            formatChoices()); empty: any format. */
        juce::StringArray formats {};

        /** None of the sounds in these folders or any folder inside them. */
        juce::Array<juce::File> foldersLeftOut {};
    };

    /** A format the scan can read, as its name is kept with each sound (the reader's) and as it
        is shown: a short label, and for the catch-all a word on what it covers. */
    struct FormatChoice
    {
        juce::String name, label, detail;
    };

    /** The formats a scan can give a sound, in the order it tries them: WAV, AIFF, FLAC, Ogg
        Vorbis and what CoreAudio reads (MP3, M4A, CAF and more). A format whose every file an
        earlier one takes first is left out, since no sound can carry its name. */
    std::vector<FormatChoice> formatChoices();

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

        /** How many sounds match a query's words and category (its order and page don't matter). */
        juce::int64 count (const Query&);

        /** The folders directly inside this one that hold sounds (in them or deeper), by name as
            the Finder orders them. Read from the index, not the disk, so a volume that is away doesn't hold it up. */
        juce::Array<juce::File> subfoldersOf (const juce::File&);

        /** How many sounds are in this folder or deeper. */
        juce::int64 countWithin (const juce::File&);

        /** How many sounds matching these words have each CatID ("" for those with none). */
        std::map<juce::String, juce::int64> countByCategory (const juce::String& text = {});

        /** Sets these sounds' category by hand (a CatID of the UCS list), or with an empty one
            clears what was set by hand, giving them back the category of their name or iXML.
            False for a CatID not in the list. */
        bool setCategory (const std::vector<juce::int64>& ids, const juce::String& catId);

        /** Rates these sounds: 0 (no rating) to 5 stars. */
        bool setRating (const std::vector<juce::int64>& ids, int stars);
        bool setFavourite (const std::vector<juce::int64>& ids, bool);

        /** Adds a tag to these sounds, or takes it off them. A tag is up to 64 characters of one
            line, with spaces round it trimmed; tags differing only in case are the same tag.
            False for an empty one. */
        bool addTag (const std::vector<juce::int64>& ids, const juce::String& tag);
        bool removeTag (const std::vector<juce::int64>& ids, const juce::String& tag);

        /** Every tag in use, in the order of their names, with how many sounds have it. */
        std::vector<std::pair<juce::String, juce::int64>> tagCounts();

        /** Puts a sound in the index, or replaces the one at its file (what a scan does for each
            file). Its category is taken as the file's own: one set by hand on that file stays. */
        bool put (Sound&);

        /** Puts many, in one transaction. */
        bool putAll (std::vector<Sound>&);

    private:
        sqlite3* db = nullptr;
        juce::String error;

        bool exec (const char* sql);
        bool remove (juce::int64 id);

        /** Rewrites the words a sound is searched by (its name, description, category and tags). */
        bool indexWords (juce::int64 id, const juce::String& name, const juce::String& description, const juce::String& catId);
        bool indexWords (juce::int64 id);

        /** Runs an update (binding the sound's id as ?1 and the value as ?2) on each sound in one
            transaction, rewriting their words when asked. */
        bool updateEach (const std::vector<juce::int64>& ids, const char* sql, const juce::var& value, bool rewriteWords);

        JUCE_DECLARE_NON_COPYABLE (Index)
    };
}
