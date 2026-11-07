#include "SoundLibrary.h"
#include <sqlite3.h>
#include <thread>

namespace SoundLibrary
{
namespace
{
    /** A prepared statement, finalized when it goes. */
    class Statement
    {
    public:
        Statement (sqlite3* db, const char* sql)
        {
            if (db != nullptr)
                sqlite3_prepare_v2 (db, sql, -1, &stmt, nullptr);
        }

        ~Statement()                                    { sqlite3_finalize (stmt); }

        bool ok() const noexcept                        { return stmt != nullptr; }
        void bind (int i, juce::int64 v)                { sqlite3_bind_int64 (stmt, i, v); }
        void bind (int i, double v)                     { sqlite3_bind_double (stmt, i, v); }
        void bind (int i, const juce::String& v)        { sqlite3_bind_text (stmt, i, v.toRawUTF8(), -1, SQLITE_TRANSIENT); }
        bool step()                                     { return sqlite3_step (stmt) == SQLITE_ROW; }
        bool run()                                      { const auto r = sqlite3_step (stmt); sqlite3_reset (stmt); return r == SQLITE_DONE; }
        void reset()                                    { sqlite3_reset (stmt); sqlite3_clear_bindings (stmt); }

        juce::int64 integer (int i)                     { return sqlite3_column_int64 (stmt, i); }
        double real (int i)                             { return sqlite3_column_double (stmt, i); }
        juce::String text (int i)
        {
            const auto* t = reinterpret_cast<const char*> (sqlite3_column_text (stmt, i));
            return t != nullptr ? juce::String::fromUTF8 (t) : juce::String();
        }

    private:
        sqlite3_stmt* stmt = nullptr;
        JUCE_DECLARE_NON_COPYABLE (Statement)
    };

    constexpr int schemaVersion = 1;

    /** What FTS5 is asked: each word searched for, as the start of a word, all of them. */
    juce::String matchFor (const juce::String& text)
    {
        juce::StringArray terms;

        for (auto& word : wordsOf (text))
            terms.add ("\"" + word.toLowerCase() + "\"*");

        return terms.joinIntoString (" ");
    }

    juce::String searchable (const juce::String& name, const juce::String& description)
    {
        return wordsOf (name).joinIntoString (" ") + " " + description;
    }

    const char* orderOf (Column c)
    {
        switch (c)
        {
            case Column::folder:     return "s.path";
            case Column::length:     return "(s.frames / s.rate)";
            case Column::sampleRate: return "s.rate";
            case Column::channels:   return "s.channels";
            case Column::format:     return "s.format";
            case Column::modified:   return "s.modified";
            case Column::name:       break;
        }

        return "s.name COLLATE NOCASE";
    }

    Sound soundFrom (Statement& st)
    {
        Sound s;
        s.id = st.integer (0);
        s.file = juce::File (st.text (1));
        s.folder = juce::File (st.text (2));
        s.name = st.text (3);
        s.size = st.integer (4);
        s.modified = st.integer (5);
        s.frames = st.integer (6);
        s.sampleRate = st.real (7);
        s.channels = (int) st.integer (8);
        s.bitDepth = (int) st.integer (9);
        s.format = st.text (10);
        s.description = st.text (11);
        return s;
    }

    constexpr const char* soundColumns = "s.id, s.path, s.folder, s.name, s.size, s.modified, s.frames, s.rate, s.channels, s.bits, s.format, s.description";
}

juce::StringArray wordsOf (const juce::String& name)
{
    juce::StringArray words;
    juce::String word;
    juce::juce_wchar previous = 0;

    const auto flush = [&]
    {
        if (word.isNotEmpty())
            words.add (word);

        word.clear();
    };

    // Letters past ASCII count as letters whatever the C library's locale makes of them.
    const auto isWordCharacter = [] (juce::juce_wchar c)
    {
        return c > 127 ? ! juce::CharacterFunctions::isWhitespace (c) : juce::CharacterFunctions::isLetterOrDigit (c);
    };

    for (auto p = name.getCharPointer(); ! p.isEmpty(); ++p)
    {
        const auto c = *p;
        const auto next = *(p + 1);

        if (! isWordCharacter (c))
        {
            flush();
            previous = 0;
            continue;
        }

        // "doorWood" splits before the W, and so does "DOORWood": a capital after a capital starts a
        // word when a small letter follows it.
        const auto caseChange = juce::CharacterFunctions::isUpperCase (c)
                                && (juce::CharacterFunctions::isLowerCase (previous)
                                    || (juce::CharacterFunctions::isUpperCase (previous) && juce::CharacterFunctions::isLowerCase (next)));
        const auto kindChange = previous != 0 && juce::CharacterFunctions::isDigit (previous) != juce::CharacterFunctions::isDigit (c);

        if (caseChange || kindChange)
            flush();

        word += juce::String::charToString (c);
        previous = c;
    }

    flush();
    return words;
}

Index::Index (const juce::File& database)
{
    database.getParentDirectory().createDirectory();

    if (sqlite3_open_v2 (database.getFullPathName().toRawUTF8(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
    {
        error = "The Sound Library's index can't be opened: " + juce::String::fromUTF8 (sqlite3_errmsg (db));
        sqlite3_close (db);
        db = nullptr;
        return;
    }

    sqlite3_busy_timeout (db, 5000);

    const auto made = exec ("PRAGMA journal_mode=WAL;"
                            "PRAGMA synchronous=NORMAL;"
                            "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);"
                            "CREATE TABLE IF NOT EXISTS folders (path TEXT PRIMARY KEY);"
                            "CREATE TABLE IF NOT EXISTS sounds (id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, folder TEXT NOT NULL,"
                            " name TEXT NOT NULL, size INTEGER, modified INTEGER, frames INTEGER, rate REAL, channels INTEGER, bits INTEGER,"
                            " format TEXT, description TEXT, seen INTEGER DEFAULT 0);"
                            "CREATE INDEX IF NOT EXISTS soundsByFolder ON sounds (folder);"
                            "CREATE INDEX IF NOT EXISTS soundsByName ON sounds (name COLLATE NOCASE);"
                            "CREATE VIRTUAL TABLE IF NOT EXISTS words USING fts5 (text, tokenize='unicode61');");

    if (! made)
        return;

    Statement version (db, "SELECT value FROM meta WHERE key = 'schema'");

    const auto stored = version.step() ? version.text (0) : juce::String();
    version.reset();

    if (stored.isNotEmpty() && stored.getIntValue() != schemaVersion)
    {
        error = "The Sound Library's index is from another version of SpectraBlade.";
        sqlite3_close (db);
        db = nullptr;
        return;
    }

    Statement set (db, "INSERT OR IGNORE INTO meta (key, value) VALUES ('schema', ?)");
    set.bind (1, juce::String (schemaVersion));
    set.run();
}

Index::~Index()
{
    sqlite3_close (db);
}

bool Index::exec (const char* sql)
{
    char* message = nullptr;

    if (db == nullptr || sqlite3_exec (db, sql, nullptr, nullptr, &message) == SQLITE_OK)
        return db != nullptr;

    error = juce::String::fromUTF8 (message != nullptr ? message : "unknown error");
    sqlite3_free (message);
    return false;
}

bool Index::addFolder (const juce::File& folder)
{
    if (! folder.isDirectory())
    {
        error = folder.getFullPathName() + " isn't a folder.";
        return false;
    }

    for (auto& f : getFolders())
    {
        if (f == folder || folder.isAChildOf (f) || f.isAChildOf (folder))
        {
            error = folder == f ? folder.getFullPathName() + " is already in the Sound Library."
                                : folder.getFullPathName() + (folder.isAChildOf (f) ? " is inside " : " holds ") + f.getFullPathName()
                                      + ", which is already in the Sound Library.";
            return false;
        }
    }

    Statement st (db, "INSERT INTO folders (path) VALUES (?)");
    st.bind (1, folder.getFullPathName());
    return st.run();
}

bool Index::removeFolder (const juce::File& folder)
{
    Statement st (db, "DELETE FROM folders WHERE path = ?");
    st.bind (1, folder.getFullPathName());

    if (! st.run() || sqlite3_changes (db) == 0)
        return false;

    exec ("BEGIN");
    Statement ids (db, "SELECT id FROM sounds WHERE folder = ?");
    ids.bind (1, folder.getFullPathName());
    std::vector<juce::int64> gone;

    while (ids.step())
        gone.push_back (ids.integer (0));

    for (auto id : gone)
        remove (id);

    return exec ("COMMIT");
}

juce::Array<juce::File> Index::getFolders()
{
    juce::Array<juce::File> folders;
    Statement st (db, "SELECT path FROM folders ORDER BY path");

    while (st.step())
        folders.add (juce::File (st.text (0)));

    return folders;
}

bool Index::remove (juce::int64 id)
{
    Statement words (db, "DELETE FROM words WHERE rowid = ?");
    Statement sound (db, "DELETE FROM sounds WHERE id = ?");
    words.bind (1, id);
    sound.bind (1, id);
    return words.run() && sound.run();
}

bool Index::put (Sound& s)
{
    Statement st (db, "INSERT INTO sounds (path, folder, name, size, modified, frames, rate, channels, bits, format, description)"
                      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
                      " ON CONFLICT (path) DO UPDATE SET folder = excluded.folder, name = excluded.name, size = excluded.size,"
                      " modified = excluded.modified, frames = excluded.frames, rate = excluded.rate, channels = excluded.channels,"
                      " bits = excluded.bits, format = excluded.format, description = excluded.description"
                      " RETURNING id");
    st.bind (1, s.file.getFullPathName());
    st.bind (2, s.folder.getFullPathName());
    st.bind (3, s.name);
    st.bind (4, s.size);
    st.bind (5, s.modified);
    st.bind (6, s.frames);
    st.bind (7, s.sampleRate);
    st.bind (8, (juce::int64) s.channels);
    st.bind (9, (juce::int64) s.bitDepth);
    st.bind (10, s.format);
    st.bind (11, s.description);

    if (! st.step())
        return false;

    s.id = st.integer (0);
    Statement clear (db, "DELETE FROM words WHERE rowid = ?");
    clear.bind (1, s.id);
    Statement words (db, "INSERT INTO words (rowid, text) VALUES (?, ?)");
    words.bind (1, s.id);
    words.bind (2, searchable (s.name, s.description));
    return clear.run() && words.run();
}

bool Index::putAll (std::vector<Sound>& sounds)
{
    if (! exec ("BEGIN"))
        return false;

    auto all = true;

    for (auto& s : sounds)
        all = put (s) && all;

    return exec ("COMMIT") && all;
}

namespace
{
    /** A sound's header, read through a buffer: the readers ask for a few bytes at a time, and
        unbuffered each ask is a trip to the disk. */
    std::unique_ptr<juce::AudioFormatReader> readHeader (juce::AudioFormatManager& formats, const juce::File& file)
    {
        for (int i = 0; i < formats.getNumKnownFormats(); ++i)
        {
            auto* format = formats.getKnownFormat (i);

            if (! format->canHandleFile (file))
                continue;

            auto in = file.createInputStream();

            if (in == nullptr)
                return {};

            if (auto* reader = format->createReaderFor (new juce::BufferedInputStream (in.release(), 16384, true), true))
                return std::unique_ptr<juce::AudioFormatReader> (reader);
        }

        return {};
    }

    /** What a scan learns of a file it has to read. */
    struct ToRead
    {
        juce::File file, folder;
        juce::int64 size = 0, modified = 0;
        bool known = false;
        std::optional<Sound> sound;
    };

    /** Reads the files' headers on several threads at once, so the disk has more than one read
        to get on with (the index itself is written on the scanning thread). */
    void readAll (juce::AudioFormatManager& formats, std::vector<ToRead>& files)
    {
        static constexpr size_t readers = 8;
        std::atomic<size_t> next { 0 };

        const auto work = [&]
        {
            for (auto i = next++; i < files.size(); i = next++)
            {
                auto& f = files[i];
                const auto reader = readHeader (formats, f.file);

                if (reader == nullptr)
                    continue;

                Sound s;
                s.file = f.file;
                s.folder = f.folder;
                s.name = f.file.getFileNameWithoutExtension();
                s.size = f.size;
                s.modified = f.modified;
                s.frames = reader->lengthInSamples;
                s.sampleRate = reader->sampleRate;
                s.channels = (int) reader->numChannels;
                s.bitDepth = (int) reader->bitsPerSample;
                s.format = reader->getFormatName();
                s.description = reader->metadataValues[juce::WavAudioFormat::bwavDescription];
                f.sound = std::move (s);
            }
        };

        std::vector<std::thread> threads;

        for (size_t t = 1; t < juce::jmin (readers, files.size()); ++t)
            threads.emplace_back (work);

        work();

        for (auto& t : threads)
            t.join();
    }
}

ScanResult Index::scan (juce::AudioFormatManager& formats, const std::function<bool (int)>& progress)
{
    ScanResult result;

    if (db == nullptr)
        return result;

    // Each scan marks what it finds; what is left unmarked in a scanned folder is gone. Every
    // statement is reset as soon as it is read: one left open holds a read of the index open,
    // and then SQLite can't move what is written back from its log, which grows without end.
    Statement last (db, "SELECT COALESCE (MAX (seen), 0) + 1 FROM sounds");
    const auto mark = last.step() ? last.integer (0) : 1;
    last.reset();
    Statement known (db, "SELECT id, size, modified FROM sounds WHERE path = ?");
    Statement seen (db, "UPDATE sounds SET seen = ? WHERE id = ?");
    const auto wildcard = formats.getWildcardForAllFormats();
    int looked = 0;
    std::vector<ToRead> toRead;

    const auto markSeen = [&] (juce::int64 id)
    {
        seen.reset();
        seen.bind (1, mark);
        seen.bind (2, id);
        seen.run();
    };

    // The files read since the last batch, put into the index.
    const auto readBatch = [&]
    {
        readAll (formats, toRead);

        for (auto& f : toRead)
        {
            if (! f.sound.has_value())
                ++result.unreadable;
            else if (put (*f.sound))
            {
                markSeen (f.sound->id);
                ++(f.known ? result.updated : result.added);
            }
        }

        toRead.clear();
    };

    exec ("BEGIN");

    for (auto& folder : getFolders())
    {
        if (result.stopped)
            break;

        for (const auto& entry : juce::RangedDirectoryIterator (folder, true, wildcard, juce::File::findFiles))
        {
            if ((looked++ % 200) == 0)
                readBatch();

            if (progress != nullptr && (looked - 1) % 200 == 0)
            {
                // Short transactions, so a search on another connection sees the work as it goes.
                exec ("COMMIT");
                exec ("BEGIN");

                if (! progress (looked))
                {
                    result.stopped = true;
                    break;
                }
            }

            const auto file = entry.getFile();
            const auto size = entry.getFileSize();
            const auto modified = entry.getModificationTime().toMilliseconds();

            known.reset();
            known.bind (1, file.getFullPathName());
            const auto there = known.step();
            const auto id = there ? known.integer (0) : 0;
            const auto same = there && known.integer (1) == size && known.integer (2) == modified;
            known.reset();

            if (same)
            {
                markSeen (id);
                ++result.unchanged;
                continue;
            }

            ToRead f;
            f.file = file;
            f.folder = folder;
            f.size = size;
            f.modified = modified;
            f.known = there;
            toRead.push_back (std::move (f));
        }
    }

    // What was gathered when the scan ended or was stopped is read and kept.
    readBatch();

    // Unless stopped (when what wasn't reached isn't known to be gone), the files not found go.
    if (! result.stopped)
    {
        Statement gone (db, "SELECT id FROM sounds WHERE seen <> ?");
        gone.bind (1, mark);
        std::vector<juce::int64> ids;

        while (gone.step())
            ids.push_back (gone.integer (0));

        for (auto id : ids)
            remove (id);

        result.removed = (int) ids.size();
    }

    exec ("COMMIT");

    // What the scan wrote, moved from the log into the index, and the log emptied.
    exec ("PRAGMA wal_checkpoint(TRUNCATE)");
    return result;
}

std::vector<Sound> Index::find (const Query& q)
{
    std::vector<Sound> sounds;
    const auto match = matchFor (q.text);
    const auto order = juce::String (orderOf (q.sortBy)) + (q.ascending ? " ASC" : " DESC") + ", s.id";
    const auto sql = match.isEmpty()
                         ? "SELECT " + juce::String (soundColumns) + " FROM sounds s ORDER BY " + order + " LIMIT ? OFFSET ?"
                         : "SELECT " + juce::String (soundColumns) + " FROM words w JOIN sounds s ON s.id = w.rowid WHERE words MATCH ? ORDER BY "
                               + order + " LIMIT ? OFFSET ?";
    Statement st (db, sql.toRawUTF8());
    int i = 1;

    if (match.isNotEmpty())
        st.bind (i++, match);

    st.bind (i++, (juce::int64) juce::jmax (0, q.limit));
    st.bind (i, (juce::int64) juce::jmax (0, q.offset));

    while (st.step())
        sounds.push_back (soundFrom (st));

    return sounds;
}

juce::int64 Index::count (const juce::String& text)
{
    const auto match = matchFor (text);
    Statement st (db, match.isEmpty() ? "SELECT COUNT(*) FROM sounds" : "SELECT COUNT(*) FROM words WHERE words MATCH ?");

    if (match.isNotEmpty())
        st.bind (1, match);

    return st.step() ? st.integer (0) : 0;
}
}
