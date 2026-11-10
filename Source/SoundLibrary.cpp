#include "SoundLibrary.h"
#include "SoundCategories.h"
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
        void bindNull (int i)                           { sqlite3_bind_null (stmt, i); }
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

    constexpr int schemaVersion = 4;   // 2 added the categories, 3 ratings, favourites and tags, 4 names matching CatIDs only in their case

    /** What FTS5 is asked: each word searched for, as the start of a word, all of them. */
    juce::String matchFor (const juce::String& text)
    {
        juce::StringArray terms;

        for (auto& word : wordsOf (text))
            terms.add ("\"" + word.toLowerCase() + "\"*");

        return terms.joinIntoString (" ");
    }

    juce::String searchable (const juce::String& name, const juce::String& description, const juce::String& catId)
    {
        auto text = wordsOf (name).joinIntoString (" ") + " " + description;

        if (const auto* e = Ucs::byCatId (catId))
            text << " " << e->category << " " << e->subCategory;

        return text;
    }

    /** The paths of the sounds inside a folder: from its path and a separator, up to (not including)
        its path and the character after the separator. */
    std::pair<juce::String, juce::String> pathRange (const juce::File& folder)
    {
        const auto base = folder.getFullPathName();
        const auto separator = juce::File::getSeparatorChar();
        return { base + juce::String::charToString (separator), base + juce::String::charToString ((juce::juce_wchar) (separator + 1)) };
    }

    /** The WHERE clause for a query's filters (category, rating, favourites, tag, folder), with what it binds. */
    std::pair<juce::String, juce::StringArray> categoryFilter (const Query& q)
    {
        juce::StringArray conditions, values;

        if (q.withoutCategory)
            conditions.add ("s.catId IS NULL");
        else if (q.catId.isNotEmpty())
        {
            conditions.add ("s.catId = ?");
            values.add (q.catId);
        }
        else if (q.category.isNotEmpty())
        {
            juce::StringArray marks;

            for (const auto* e : Ucs::subCategoriesOf (q.category))
            {
                values.add (e->catId);
                marks.add ("?");
            }

            conditions.add (marks.isEmpty() ? juce::String ("0") : "s.catId IN (" + marks.joinIntoString (", ") + ")");
        }

        if (q.minRating > 0)
            conditions.add ("s.rating >= " + juce::String (juce::jlimit (1, 5, q.minRating)));

        if (q.favouritesOnly)
            conditions.add ("s.favourite = 1");

        if (q.tag.isNotEmpty())
        {
            conditions.add ("s.id IN (SELECT sound FROM tags WHERE tag = ?)");   // tag compares without case, as declared
            values.add (q.tag.trim());
        }

        if (q.within != juce::File())
        {
            // Every path that starts with the folder and a separator: the next character after the
            // separator ends the range, which the paths' own index finds without reading the rest.
            const auto [from, to] = pathRange (q.within);
            conditions.add ("s.path >= ? AND s.path < ?");
            values.add (from);
            values.add (to);
        }

        if (! q.hiddenFormats.isEmpty())
        {
            juce::StringArray marks;

            for (const auto& format : q.hiddenFormats)
            {
                values.add (format);
                marks.add ("?");
            }

            conditions.add ("(s.format IS NULL OR s.format NOT IN (" + marks.joinIntoString (", ") + "))");
        }

        return { conditions.joinIntoString (" AND "), values };
    }

    /** A tag as kept: one line, trimmed, at most 64 characters; empty when nothing is left. */
    juce::String cleanTag (const juce::String& tag)
    {
        return tag.replaceCharacters ("\t\r\n", "   ").trim().substring (0, 64).trim();
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
            case Column::category:   return "(s.catId IS NULL), s.catId COLLATE NOCASE";
            case Column::rating:     return "s.rating";
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
        s.catId = st.text (12);
        s.categoryFrom = (CategoryFrom) juce::jlimit (0, 3, (int) st.integer (13));
        s.rating = juce::jlimit (0, 5, (int) st.integer (14));
        s.favourite = st.integer (15) != 0;
        s.tags = juce::StringArray::fromTokens (st.text (16), "\t", {});
        return s;
    }

    constexpr const char* soundColumns = "s.id, s.path, s.folder, s.name, s.size, s.modified, s.frames, s.rate, s.channels, s.bits, s.format, "
                                         "s.description, s.catId, s.catFrom, s.rating, s.favourite,"
                                         " (SELECT group_concat (tag, char (9)) FROM (SELECT tag FROM tags t WHERE t.sound = s.id ORDER BY tag COLLATE NOCASE))";
}

std::vector<FormatChoice> formatChoices()
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::vector<FormatChoice> choices;
    const auto sample = juce::File::getSpecialLocation (juce::File::tempDirectory);

    for (int i = 0; i < formats.getNumKnownFormats(); ++i)
    {
        auto* format = formats.getKnownFormat (i);
        auto reachable = false;

        for (const auto& extension : format->getFileExtensions())
        {
            auto taken = false;

            for (int j = 0; j < i && ! taken; ++j)
                taken = formats.getKnownFormat (j)->canHandleFile (sample.getChildFile ("sound" + extension));

            reachable = reachable || ! taken;
        }

        if (! reachable)
            continue;

        FormatChoice choice;
        choice.name = format->getFormatName();
        choice.label = choice.name.upToFirstOccurrenceOf (" file", false, true).replace ("Ogg-Vorbis", "Ogg Vorbis");

        if (choice.name.startsWith ("CoreAudio"))
        {
            choice.label = "Other";
            choice.detail = "MP3, M4A, CAF and whatever else CoreAudio reads";
        }

        choices.push_back (std::move (choice));
    }

    return choices;
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
                            " format TEXT, description TEXT, seen INTEGER DEFAULT 0,"
                            " catId TEXT, catFrom INTEGER DEFAULT 0, fileCatId TEXT, fileCatFrom INTEGER DEFAULT 0,"
                            " rating INTEGER DEFAULT 0, favourite INTEGER DEFAULT 0);"
                            "CREATE TABLE IF NOT EXISTS tags (sound INTEGER NOT NULL, tag TEXT NOT NULL COLLATE NOCASE, PRIMARY KEY (sound, tag));"
                            "CREATE INDEX IF NOT EXISTS tagsByTag ON tags (tag);"
                            "CREATE INDEX IF NOT EXISTS soundsByFolder ON sounds (folder);"
                            "CREATE INDEX IF NOT EXISTS soundsByName ON sounds (name COLLATE NOCASE);"
                            "CREATE VIRTUAL TABLE IF NOT EXISTS words USING fts5 (text, tokenize='unicode61');");

    if (! made)
        return;

    Statement version (db, "SELECT value FROM meta WHERE key = 'schema'");

    const auto stored = version.step() ? version.text (0) : juce::String();
    version.reset();

    // An index from before the categories gets their columns, and its files are read again on
    // the next scan (their size forgotten), so their names and iXML give their categories.
    // Version 3 added ratings and favourites (the tags table is made above when it isn't there).
    const auto upgrade = [this] (const char* sql)
    {
        if (exec (sql))
            return true;

        error = "The Sound Library's index couldn't be brought up to date.";
        sqlite3_close (db);
        db = nullptr;
        return false;
    };

    if (stored == "1" && ! upgrade ("BEGIN;"
                                    "ALTER TABLE sounds ADD COLUMN catId TEXT;"
                                    "ALTER TABLE sounds ADD COLUMN catFrom INTEGER DEFAULT 0;"
                                    "ALTER TABLE sounds ADD COLUMN fileCatId TEXT;"
                                    "ALTER TABLE sounds ADD COLUMN fileCatFrom INTEGER DEFAULT 0;"
                                    "UPDATE sounds SET size = -1;"
                                    "UPDATE meta SET value = '2' WHERE key = 'schema';"
                                    "COMMIT;"))
        return;

    if ((stored == "1" || stored == "2") && ! upgrade ("BEGIN;"
                                                       "ALTER TABLE sounds ADD COLUMN rating INTEGER DEFAULT 0;"
                                                       "ALTER TABLE sounds ADD COLUMN favourite INTEGER DEFAULT 0;"
                                                       "UPDATE meta SET value = '3' WHERE key = 'schema';"
                                                       "COMMIT;"))
        return;

    // Version 4: a name gives a category only when it starts with the CatID in its own case
    // ("Wind_Chimes" and "beep_glitch" are not UCS names). Sounds whose names gave one that way
    // lose it (unless it was set by hand) and are read again on the next scan, for their iXML.
    if (stored == "1" || stored == "2" || stored == "3")
    {
        if (! exec ("BEGIN"))
            return;

        std::vector<juce::int64> loose;
        {
            Statement named (db, "SELECT id, name FROM sounds WHERE fileCatFrom = 1");

            while (named.step())
                if (Ucs::fromName (named.text (1)) == nullptr)
                    loose.push_back (named.integer (0));

            named.reset();
        }

        Statement forget (db, "UPDATE sounds SET fileCatId = NULL, fileCatFrom = 0, size = -1,"
                              " catId = CASE WHEN catFrom = 3 THEN catId ELSE NULL END,"
                              " catFrom = CASE WHEN catFrom = 3 THEN 3 ELSE 0 END WHERE id = ?");

        for (const auto id : loose)
        {
            forget.bind (1, id);
            forget.run();
            forget.reset();
            indexWords (id);
        }

        if (! upgrade ("UPDATE meta SET value = '4' WHERE key = 'schema';"
                       "COMMIT;"))
            return;
    }

    if (stored.isNotEmpty() && stored != "1" && stored != "2" && stored != "3" && stored.getIntValue() != schemaVersion)
    {
        error = "The Sound Library's index is from another version of SpectraBlade.";
        sqlite3_close (db);
        db = nullptr;
        return;
    }

    Statement set (db, "INSERT OR IGNORE INTO meta (key, value) VALUES ('schema', ?)");
    set.bind (1, juce::String (schemaVersion));
    set.run();
    exec ("CREATE INDEX IF NOT EXISTS soundsByCategory ON sounds (catId);"
          "CREATE INDEX IF NOT EXISTS soundsRated ON sounds (rating) WHERE rating > 0;"
          "CREATE INDEX IF NOT EXISTS soundsFavourite ON sounds (favourite) WHERE favourite = 1;");
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
    Statement tags (db, "DELETE FROM tags WHERE sound = ?");
    Statement sound (db, "DELETE FROM sounds WHERE id = ?");
    words.bind (1, id);
    tags.bind (1, id);
    sound.bind (1, id);
    return words.run() && tags.run() && sound.run();
}

bool Index::put (Sound& s)
{
    // The category given is the file's own; one set by hand (catFrom 3) stays.
    Statement st (db, "INSERT INTO sounds (path, folder, name, size, modified, frames, rate, channels, bits, format, description,"
                      " catId, catFrom, fileCatId, fileCatFrom)"
                      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?12, ?13, ?12, ?13)"
                      " ON CONFLICT (path) DO UPDATE SET folder = excluded.folder, name = excluded.name, size = excluded.size,"
                      " modified = excluded.modified, frames = excluded.frames, rate = excluded.rate, channels = excluded.channels,"
                      " bits = excluded.bits, format = excluded.format, description = excluded.description,"
                      " fileCatId = excluded.fileCatId, fileCatFrom = excluded.fileCatFrom,"
                      " catId = CASE WHEN sounds.catFrom = 3 THEN sounds.catId ELSE excluded.fileCatId END,"
                      " catFrom = CASE WHEN sounds.catFrom = 3 THEN 3 ELSE excluded.fileCatFrom END"
                      " RETURNING id, catId");
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
    const auto* entry = s.categoryFrom == CategoryFrom::hand ? nullptr : Ucs::byCatId (s.catId);

    if (entry != nullptr)
    {
        st.bind (12, entry->catId);
        st.bind (13, (juce::int64) s.categoryFrom);
    }
    else
    {
        st.bindNull (12);
        st.bind (13, (juce::int64) 0);
    }

    if (! st.step())
        return false;

    s.id = st.integer (0);
    const auto catId = st.text (1);
    st.reset();
    return indexWords (s.id, s.name, s.description, catId);
}

bool Index::indexWords (juce::int64 id, const juce::String& name, const juce::String& description, const juce::String& catId)
{
    Statement tags (db, "SELECT tag FROM tags WHERE sound = ?");
    tags.bind (1, id);
    auto text = searchable (name, description, catId);

    while (tags.step())
        text << " " << tags.text (0);

    tags.reset();
    Statement clear (db, "DELETE FROM words WHERE rowid = ?");
    clear.bind (1, id);
    Statement words (db, "INSERT INTO words (rowid, text) VALUES (?, ?)");
    words.bind (1, id);
    words.bind (2, text);
    return clear.run() && words.run();
}

bool Index::indexWords (juce::int64 id)
{
    Statement read (db, "SELECT name, description, catId FROM sounds WHERE id = ?");
    read.bind (1, id);

    if (! read.step())
        return false;

    const auto name = read.text (0), description = read.text (1), catId = read.text (2);
    read.reset();
    return indexWords (id, name, description, catId);
}

bool Index::updateEach (const std::vector<juce::int64>& ids, const char* sql, const juce::var& value, bool rewriteWords)
{
    if (! exec ("BEGIN"))
        return false;

    Statement st (db, sql);
    auto all = true;

    for (auto id : ids)
    {
        st.reset();
        st.bind (1, id);

        if (value.isString())
            st.bind (2, value.toString());
        else
            st.bind (2, (juce::int64) value);

        all = st.run() && all;

        if (rewriteWords)
            all = indexWords (id) && all;
    }

    return exec ("COMMIT") && all;
}

bool Index::setRating (const std::vector<juce::int64>& ids, int stars)
{
    return updateEach (ids, "UPDATE sounds SET rating = ?2 WHERE id = ?1", juce::jlimit (0, 5, stars), false);
}

bool Index::setFavourite (const std::vector<juce::int64>& ids, bool favourite)
{
    return updateEach (ids, "UPDATE sounds SET favourite = ?2 WHERE id = ?1", favourite ? 1 : 0, false);
}

bool Index::addTag (const std::vector<juce::int64>& ids, const juce::String& tag)
{
    const auto clean = cleanTag (tag);
    return clean.isNotEmpty()
        && updateEach (ids, "INSERT OR IGNORE INTO tags (sound, tag) SELECT id, ?2 FROM sounds WHERE id = ?1", clean, true);
}

bool Index::removeTag (const std::vector<juce::int64>& ids, const juce::String& tag)
{
    const auto clean = cleanTag (tag);
    return clean.isNotEmpty() && updateEach (ids, "DELETE FROM tags WHERE sound = ?1 AND tag = ?2", clean, true);
}

std::vector<std::pair<juce::String, juce::int64>> Index::tagCounts()
{
    std::vector<std::pair<juce::String, juce::int64>> counts;
    Statement st (db, "SELECT tag, COUNT(*) FROM tags GROUP BY tag ORDER BY tag COLLATE NOCASE");

    while (st.step())
        counts.emplace_back (st.text (0), st.integer (1));

    return counts;
}

bool Index::setCategory (const std::vector<juce::int64>& ids, const juce::String& catId)
{
    const auto* entry = catId.isEmpty() ? nullptr : Ucs::byCatId (catId);

    if (catId.isNotEmpty() && entry == nullptr)
        return false;

    if (! exec ("BEGIN"))
        return false;

    Statement set (db, entry != nullptr ? "UPDATE sounds SET catId = ?, catFrom = 3 WHERE id = ?2"
                                        : "UPDATE sounds SET catId = fileCatId, catFrom = fileCatFrom WHERE id = ?2");
    auto all = true;

    for (auto id : ids)
    {
        set.reset();

        if (entry != nullptr)
            set.bind (1, entry->catId);

        set.bind (2, id);
        all = set.run() && all;
        all = indexWords (id) && all;
    }

    return exec ("COMMIT") && all;
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
    /** A WAV file's iXML chunk, read from the stream (left anywhere); empty when there is none. */
    juce::String readIXml (juce::InputStream& in)
    {
        if (in.readInt() != (int) juce::ByteOrder::littleEndianInt ("RIFF"))
            return {};

        in.readInt();

        if (in.readInt() != (int) juce::ByteOrder::littleEndianInt ("WAVE"))
            return {};

        while (! in.isExhausted())
        {
            const auto id = in.readInt();
            const auto size = (juce::int64) (juce::uint32) in.readInt();
            const auto start = in.getPosition();

            if (id == (int) juce::ByteOrder::littleEndianInt ("iXML"))
            {
                juce::MemoryBlock block;
                in.readIntoMemoryBlock (block, (ssize_t) juce::jmin (size, (juce::int64) 1 << 20));
                return block.toString();
            }

            if (size == 0xffffffff || ! in.setPosition (start + size + (size & 1)))
                break;   // an RF64 file's sizes are elsewhere: no iXML is looked for past its audio
        }

        return {};
    }

    struct Header
    {
        std::unique_ptr<juce::AudioFormatReader> reader;
        juce::String ixml;
    };

    Header readHeader (juce::AudioFormatManager& formats, const juce::File& file)
    {
        for (int i = 0; i < formats.getNumKnownFormats(); ++i)
        {
            auto* format = formats.getKnownFormat (i);

            if (! format->canHandleFile (file))
                continue;

            auto in = file.createInputStream();

            if (in == nullptr)
                return {};

            auto buffered = std::make_unique<juce::BufferedInputStream> (in.release(), 16384, true);
            Header h;

            if (dynamic_cast<juce::WavAudioFormat*> (format) != nullptr)
            {
                h.ixml = readIXml (*buffered);
                buffered->setPosition (0);
            }

            if (auto* reader = format->createReaderFor (buffered.release(), true))
            {
                h.reader.reset (reader);
                return h;
            }
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
                const auto header = readHeader (formats, f.file);
                const auto& reader = header.reader;

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

                // The category: from a UCS name, else from the iXML.
                if (const auto* e = Ucs::fromName (s.name))
                {
                    s.catId = e->catId;
                    s.categoryFrom = CategoryFrom::name;
                }
                else if (const auto* m = header.ixml.isNotEmpty() ? Ucs::fromIXml (header.ixml) : nullptr)
                {
                    s.catId = m->catId;
                    s.categoryFrom = CategoryFrom::metadata;
                }

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
    const auto direction = q.ascending ? " ASC" : " DESC";
    auto order = juce::String (orderOf (q.sortBy)).replace (", ", juce::String (direction) + ", ") + direction + ", s.id";
    const auto [filter, values] = categoryFilter (q);
    auto conditions = filter;

    if (match.isNotEmpty())
        conditions = "words MATCH ?" + (filter.isEmpty() ? juce::String() : " AND " + filter);

    const auto sql = "SELECT " + juce::String (soundColumns)
                     + (match.isEmpty() ? " FROM sounds s" : " FROM words w JOIN sounds s ON s.id = w.rowid")
                     + (conditions.isEmpty() ? juce::String() : " WHERE " + conditions) + " ORDER BY " + order + " LIMIT ? OFFSET ?";
    Statement st (db, sql.toRawUTF8());
    int i = 1;

    if (match.isNotEmpty())
        st.bind (i++, match);

    for (const auto& v : values)
        st.bind (i++, v);

    st.bind (i++, (juce::int64) juce::jmax (0, q.limit));
    st.bind (i, (juce::int64) juce::jmax (0, q.offset));

    while (st.step())
        sounds.push_back (soundFrom (st));

    return sounds;
}

juce::int64 Index::count (const Query& q)
{
    const auto [filter, values] = categoryFilter (q);

    if (filter.isEmpty())
        return count (q.text);

    const auto match = matchFor (q.text);
    const auto sql = match.isEmpty() ? "SELECT COUNT(*) FROM sounds s WHERE " + filter
                                     : "SELECT COUNT(*) FROM words w JOIN sounds s ON s.id = w.rowid WHERE words MATCH ? AND " + filter;
    Statement st (db, sql.toRawUTF8());
    int i = 1;

    if (match.isNotEmpty())
        st.bind (i++, match);

    for (const auto& v : values)
        st.bind (i++, v);

    return st.step() ? st.integer (0) : 0;
}

juce::Array<juce::File> Index::subfoldersOf (const juce::File& folder)
{
    juce::Array<juce::File> found;
    const auto [prefix, end] = pathRange (folder);
    const auto separator = juce::File::getSeparatorString();
    auto from = prefix;
    auto past = true;   // the next path is past `from` (after a file), or at least it (after a folder)

    // Steps from one subfolder to the next with one look-up each (and one for each file loose in
    // the folder), rather than reading every path below.
    for (;;)
    {
        Statement st (db, past ? "SELECT path FROM sounds WHERE path > ? AND path < ? ORDER BY path LIMIT 1"
                               : "SELECT path FROM sounds WHERE path >= ? AND path < ? ORDER BY path LIMIT 1");
        st.bind (1, from);
        st.bind (2, end);

        if (! st.step())
            break;

        const auto path = st.text (0);
        const auto rest = path.substring (prefix.length());
        const auto cut = rest.indexOf (separator);

        if (cut < 0)
        {
            from = path;
            past = true;
            continue;
        }

        const auto child = folder.getChildFile (rest.substring (0, cut));
        found.add (child);
        from = pathRange (child).second;
        past = false;
    }

    // By name as the Finder lists them, not by the bytes of the path ("Doors" before "Doors-Old").
    std::sort (found.begin(), found.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getFileName().compareNatural (b.getFileName()) < 0;
    });

    return found;
}

juce::int64 Index::countWithin (const juce::File& folder)
{
    const auto [from, to] = pathRange (folder);
    Statement st (db, "SELECT COUNT(*) FROM sounds WHERE path >= ? AND path < ?");
    st.bind (1, from);
    st.bind (2, to);
    return st.step() ? st.integer (0) : 0;
}

std::map<juce::String, juce::int64> Index::countByCategory (const juce::String& text)
{
    std::map<juce::String, juce::int64> counts;
    const auto match = matchFor (text);
    Statement st (db, match.isEmpty() ? "SELECT catId, COUNT(*) FROM sounds GROUP BY catId"
                                      : "SELECT s.catId, COUNT(*) FROM words w JOIN sounds s ON s.id = w.rowid WHERE words MATCH ? GROUP BY s.catId");

    if (match.isNotEmpty())
        st.bind (1, match);

    while (st.step())
        counts[st.text (0)] = st.integer (1);

    return counts;
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
