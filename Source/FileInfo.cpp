#include "FileInfo.h"

bool FileInfo::hasText() const
{
    for (const auto* s : { &title, &artist, &album, &genre, &year, &track, &comment })
        if (s->isNotEmpty())
            return true;

    return false;
}

juce::String FileInfo::coverMime() const
{
    if (! hasCover())
        return {};

    const auto* p = (const juce::uint8*) cover->getData();
    return cover->getSize() >= 4 && p[0] == 0x89 && p[1] == 'P' ? "image/png" : "image/jpeg";
}

bool FileInfo::operator== (const FileInfo& o) const
{
    const auto sameCover = hasCover() == o.hasCover() && (! hasCover() || *cover == *o.cover);
    return title == o.title && artist == o.artist && album == o.album && genre == o.genre && year == o.year && track == o.track
        && comment == o.comment && description == o.description && originator == o.originator && originatorReference == o.originatorReference
        && originationDate == o.originationDate && originationTime == o.originationTime && timeReference == o.timeReference && sameCover;
}

bool FileInfo::hasBroadcast() const
{
    return description.isNotEmpty() || originator.isNotEmpty() || originatorReference.isNotEmpty() || originationDate.isNotEmpty()
        || originationTime.isNotEmpty() || timeReference != 0;
}

namespace FileInfoIO
{
bool isCoverImage (const void* data, size_t size)
{
    const auto* p = (const juce::uint8*) data;
    const auto jpeg = size >= 3 && p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff;
    const auto png = size >= 8 && std::memcmp (p, "\x89PNG\r\n\x1a\n", 8) == 0;
    return (jpeg || png) && size <= FileInfo::maxCoverBytes;
}

namespace
{
    using Bytes = std::vector<juce::uint8>;

    /** A cover from bytes, when they are an image. */
    std::shared_ptr<const juce::MemoryBlock> coverFrom (const juce::uint8* data, size_t size)
    {
        return isCoverImage (data, size) ? std::make_shared<const juce::MemoryBlock> (data, size) : nullptr;
    }

    void parseId3v2 (const juce::uint8* head, Bytes body, FileInfo& info);
    void parseVorbisComment (const juce::uint8* data, size_t size, FileInfo& info);
    Bytes pictureBlock (const FileInfo& info);

    juce::String textFrom (const juce::uint8* data, size_t size)
    {
        while (size > 0 && data[size - 1] == 0)
            --size;

        const auto* chars = reinterpret_cast<const char*> (data);

        if (juce::CharPointer_UTF8::isValidString (chars, (int) size))
            return juce::String::fromUTF8 (chars, (int) size).trim();

        juce::String latin;

        for (size_t i = 0; i < size; ++i)
            latin += (juce::juce_wchar) data[i];

        return latin.trim();
    }

    void put32le (Bytes& b, juce::uint32 v)   { for (int i = 0; i < 4; ++i) b.push_back ((juce::uint8) (v >> (8 * i))); }
    void put32be (Bytes& b, juce::uint32 v)   { for (int i = 3; i >= 0; --i) b.push_back ((juce::uint8) (v >> (8 * i))); }
    void put4cc (Bytes& b, const char* id)    { for (int i = 0; i < 4; ++i) b.push_back ((juce::uint8) id[i]); }
    void putBytes (Bytes& b, const Bytes& more) { b.insert (b.end(), more.begin(), more.end()); }

    juce::uint32 get32be (const juce::uint8* p) { return ((juce::uint32) p[0] << 24) | ((juce::uint32) p[1] << 16) | ((juce::uint32) p[2] << 8) | p[3]; }
    juce::uint32 get32le (const juce::uint8* p) { return ((juce::uint32) p[3] << 24) | ((juce::uint32) p[2] << 16) | ((juce::uint32) p[1] << 8) | p[0]; }
    void set32be (juce::uint8* p, juce::uint32 v) { for (int i = 0; i < 4; ++i) p[i] = (juce::uint8) (v >> (8 * (3 - i))); }

    Bytes utf8 (const juce::String& s)
    {
        const auto* c = s.toRawUTF8();
        return Bytes (c, c + s.getNumBytesAsUTF8());
    }

    //==============================================================================
    struct RiffKey { const char* id; juce::String FileInfo::* field; };
    const RiffKey riffKeys[] { { "INAM", &FileInfo::title }, { "IART", &FileInfo::artist }, { "IPRD", &FileInfo::album },
                               { "IGNR", &FileInfo::genre }, { "ICRD", &FileInfo::year }, { "ITRK", &FileInfo::track },
                               { "ICMT", &FileInfo::comment } };

    //==============================================================================
    // ID3v2
    juce::uint32 syncsafe (const juce::uint8* p) { return ((juce::uint32) (p[0] & 0x7f) << 21) | ((juce::uint32) (p[1] & 0x7f) << 14) | ((juce::uint32) (p[2] & 0x7f) << 7) | (p[3] & 0x7f); }

    bool isLatin1 (const juce::String& s)
    {
        for (auto p = s.getCharPointer(); ! p.isEmpty();)
            if (p.getAndAdvance() > 0xff)
                return false;

        return true;
    }

    /** The text in ID3's encoding 0 (Latin-1) or 1 (UTF-16 with a byte-order mark), with no terminator. */
    Bytes id3Text (const juce::String& s, bool wide, bool terminate)
    {
        Bytes b;

        if (! wide)
        {
            for (auto p = s.getCharPointer(); ! p.isEmpty();)
                b.push_back ((juce::uint8) p.getAndAdvance());

            if (terminate)
                b.push_back (0);

            return b;
        }

        const auto unit = [&b] (juce::uint32 u) { b.push_back ((juce::uint8) (u & 0xff)); b.push_back ((juce::uint8) (u >> 8)); };
        b.push_back (0xff);
        b.push_back (0xfe);

        for (auto p = s.getCharPointer(); ! p.isEmpty();)
        {
            const auto c = (juce::uint32) p.getAndAdvance();

            if (c >= 0x10000)
            {
                unit (0xd800 + ((c - 0x10000) >> 10));
                unit (0xdc00 + ((c - 0x10000) & 0x3ff));
            }
            else
            {
                unit (c);
            }
        }

        if (terminate)
            unit (0);

        return b;
    }

    /** Decodes ID3 text in an encoding (0 Latin-1, 1 UTF-16 with BOM, 2 UTF-16BE, 3 UTF-8) up to its
        first terminator; returns the bytes used, terminator included. */
    size_t id3Decode (int encoding, const juce::uint8* p, size_t size, juce::String& out)
    {
        out.clear();

        if (encoding == 0 || encoding == 3)
        {
            size_t n = 0;

            while (n < size && p[n] != 0)
                ++n;

            if (encoding == 3)
                out = juce::String::fromUTF8 ((const char*) p, (int) n);
            else
                for (size_t i = 0; i < n; ++i)
                    out += (juce::juce_wchar) p[i];

            return juce::jmin (size, n + 1);
        }

        auto bigEndian = encoding == 2;
        size_t i = 0;

        if (encoding == 1 && size >= 2 && ((p[0] == 0xff && p[1] == 0xfe) || (p[0] == 0xfe && p[1] == 0xff)))
        {
            bigEndian = p[0] == 0xfe;
            i = 2;
        }

        for (; i + 1 < size; i += 2)
        {
            auto u = bigEndian ? (juce::uint32) ((p[i] << 8) | p[i + 1]) : (juce::uint32) ((p[i + 1] << 8) | p[i]);

            if (u == 0)
                return i + 2;

            if (u >= 0xd800 && u < 0xdc00 && i + 3 < size)
            {
                const auto low = bigEndian ? (juce::uint32) ((p[i + 2] << 8) | p[i + 3]) : (juce::uint32) ((p[i + 3] << 8) | p[i + 2]);
                u = 0x10000 + ((u - 0xd800) << 10) + (low - 0xdc00);
                i += 2;
            }

            out += (juce::juce_wchar) u;
        }

        return size;
    }

    //==============================================================================
    // MPEG-4 boxes
    struct Mp4Key { const char* id; juce::String FileInfo::* field; };
    const Mp4Key mp4Keys[] { { "\xa9nam", &FileInfo::title }, { "\xa9" "ART", &FileInfo::artist }, { "\xa9" "alb", &FileInfo::album },
                             { "\xa9gen", &FileInfo::genre }, { "\xa9" "day", &FileInfo::year }, { "\xa9" "cmt", &FileInfo::comment } };

    Bytes box (const char* type, const Bytes& content)
    {
        Bytes b;
        put32be (b, (juce::uint32) (content.size() + 8));
        put4cc (b, type);
        putBytes (b, content);
        return b;
    }

    /** The box of a type among the boxes in [begin, end) of data, as {offset, size}; size 0 if none. */
    std::pair<size_t, size_t> findBox (const Bytes& data, size_t begin, size_t end, const char* type)
    {
        for (auto at = begin; at + 8 <= end;)
        {
            const auto size = (size_t) get32be (data.data() + at);

            if (size < 8 || at + size > end)
                break;

            if (std::memcmp (data.data() + at + 4, type, 4) == 0)
                return { at, size };

            at += size;
        }

        return { 0, 0 };
    }

    /** Where a meta box's children start: after its version and flags, unless (QuickTime style) it has none. */
    size_t metaChildren (const Bytes& data, size_t meta)
    {
        return std::memcmp (data.data() + meta + 12, "hdlr", 4) == 0 ? meta + 8 : meta + 12;
    }

    /** The top-level boxes of a file, as {type, offset, size}. */
    struct TopBox { juce::String type; juce::int64 offset, size; };

    std::vector<TopBox> topBoxes (juce::FileInputStream& in)
    {
        std::vector<TopBox> boxes;
        const auto total = in.getTotalLength();

        for (juce::int64 at = 0; at + 8 <= total;)
        {
            juce::uint8 head[16];
            in.setPosition (at);

            if (in.read (head, 8) != 8)
                break;

            juce::int64 size = get32be (head);

            if (size == 1)
            {
                if (in.read (head + 8, 8) != 8)
                    break;

                size = ((juce::int64) get32be (head + 8) << 32) | get32be (head + 12);
            }
            else if (size == 0)
            {
                size = total - at;
            }

            if (size < 8)
                break;

            // The type's four bytes as characters, whatever they are (the tag boxes start with 0xA9).
            juce::String type;

            for (int i = 4; i < 8; ++i)
                type += juce::String::charToString ((juce::juce_wchar) head[i]);

            boxes.push_back ({ type, at, size });
            at += size;
        }

        return boxes;
    }
}

//==============================================================================
void readBroadcast (const juce::StringPairArray& m, FileInfo& info)
{
    info.description = m[juce::WavAudioFormat::bwavDescription];
    info.originator = m[juce::WavAudioFormat::bwavOriginator];
    info.originatorReference = m[juce::WavAudioFormat::bwavOriginatorRef];
    info.originationDate = m[juce::WavAudioFormat::bwavOriginationDate];
    info.originationTime = m[juce::WavAudioFormat::bwavOriginationTime];
    info.timeReference = m[juce::WavAudioFormat::bwavTimeReference].getLargeIntValue();
}

juce::StringPairArray broadcastMetadata (const FileInfo& info)
{
    juce::StringPairArray m;

    if (info.hasBroadcast())
    {
        m.set (juce::WavAudioFormat::bwavDescription, info.description);
        m.set (juce::WavAudioFormat::bwavOriginator, info.originator);
        m.set (juce::WavAudioFormat::bwavOriginatorRef, info.originatorReference);
        m.set (juce::WavAudioFormat::bwavOriginationDate, info.originationDate);
        m.set (juce::WavAudioFormat::bwavOriginationTime, info.originationTime);
        m.set (juce::WavAudioFormat::bwavTimeReference, juce::String (info.timeReference));
    }

    return m;
}

void readRiffInfo (const juce::File& file, FileInfo& info)
{
    juce::FileInputStream in (file);
    juce::uint8 head[12];

    if (! in.openedOk() || in.read (head, 12) != 12 || std::memcmp (head, "RIFF", 4) != 0 || std::memcmp (head + 8, "WAVE", 4) != 0)
        return;

    FileInfo fromId3;

    while (in.getPosition() + 8 <= in.getTotalLength())
    {
        juce::uint8 chunk[8];

        if (in.read (chunk, 8) != 8)
            return;

        const auto size = (juce::int64) get32le (chunk + 4);
        const auto start = in.getPosition();

        if (std::memcmp (chunk, "LIST", 4) == 0 && size >= 4 && size < (1 << 20))
        {
            juce::MemoryBlock list;
            in.readIntoMemoryBlock (list, (ssize_t) size);
            const auto* p = (const juce::uint8*) list.getData();

            if (list.getSize() >= 4 && std::memcmp (p, "INFO", 4) == 0)
            {
                for (size_t at = 4; at + 8 <= list.getSize();)
                {
                    const auto itemSize = (size_t) get32le (p + at + 4);

                    if (at + 8 + itemSize > list.getSize())
                        break;

                    for (const auto& key : riffKeys)
                        if (std::memcmp (p + at, key.id, 4) == 0)
                            info.*key.field = textFrom (p + at + 8, itemSize);

                    // ffmpeg writes the track as IPRT when it has no ITRK.
                    if (std::memcmp (p + at, "IPRT", 4) == 0 && info.track.isEmpty())
                        info.track = textFrom (p + at + 8, itemSize);

                    at += 8 + itemSize + (itemSize & 1);
                }
            }
        }
        else if ((std::memcmp (chunk, "id3 ", 4) == 0 || std::memcmp (chunk, "ID3 ", 4) == 0) && size >= 10 && size < (32 << 20))
        {
            juce::MemoryBlock tag;
            in.readIntoMemoryBlock (tag, (ssize_t) size);
            const auto* p = (const juce::uint8*) tag.getData();
            parseId3v2 (p, Bytes (p + 10, p + tag.getSize()), fromId3);
        }

        if (! in.setPosition (start + size + (size & 1)))
            break;
    }

    // An ID3 chunk's cover, and its tags where INFO has none.
    info.cover = fromId3.cover;

    for (auto field : { &FileInfo::title, &FileInfo::artist, &FileInfo::album, &FileInfo::genre, &FileInfo::year, &FileInfo::track, &FileInfo::comment })
        if ((info.*field).isEmpty())
            info.*field = fromId3.*field;
}

juce::String appendRiffInfo (const juce::File& file, const FileInfo& info)
{
    if (! info.hasTags())
        return {};

    Bytes list;
    put4cc (list, "INFO");

    for (const auto& key : riffKeys)
    {
        const auto& value = info.*key.field;

        if (value.isEmpty())
            continue;

        auto text = utf8 (value);
        text.push_back (0);
        put4cc (list, key.id);
        put32le (list, (juce::uint32) text.size());
        putBytes (list, text);

        if (text.size() & 1)
            list.push_back (0);
    }

    Bytes chunk;

    if (info.hasText())
    {
        put4cc (chunk, "LIST");
        put32le (chunk, (juce::uint32) list.size());
        putBytes (chunk, list);
    }

    // A cover goes in an ID3 chunk, with the tags too, as the players that show covers read them there.
    if (info.hasCover())
    {
        const auto tag = id3v2 (info);
        put4cc (chunk, "id3 ");
        put32le (chunk, (juce::uint32) tag.getSize());
        chunk.insert (chunk.end(), (const juce::uint8*) tag.getData(), (const juce::uint8*) tag.getData() + tag.getSize());

        if ((tag.getSize() & 1) != 0)
            chunk.push_back (0);
    }

    {
        juce::FileInputStream in (file);
        juce::uint8 head[4];

        if (! in.openedOk() || in.read (head, 4) != 4)
            return "Couldn't read " + file.getFileName() + ".";

        if (std::memcmp (head, "RIFF", 4) != 0)
            return "The tags can't be added to a WAV file over 4 GB.";
    }

    const auto length = file.getSize();
    const auto pad = (length & 1) != 0;

    if (length + (pad ? 1 : 0) + (juce::int64) chunk.size() > (juce::int64) 0xffffffff)
        return "The tags can't be added to a WAV file over 4 GB.";

    juce::FileOutputStream out (file);

    if (out.failedToOpen())
        return "Couldn't add the tags to " + file.getFileName() + ".";

    if (pad)
        out.writeByte (0);

    out.write (chunk.data(), chunk.size());
    const auto riffSize = (juce::uint32) (out.getPosition() - 8);
    out.setPosition (4);
    out.writeInt ((int) riffSize);   // little-endian, as RIFF wants
    out.flush();
    return out.getStatus().wasOk() ? juce::String() : "Couldn't add the tags to " + file.getFileName() + ".";
}

//==============================================================================
juce::MemoryBlock id3v2 (const FileInfo& info)
{
    if (! info.hasTags())
        return {};

    Bytes frames;

    const auto frame = [&frames] (const char* id, const Bytes& content)
    {
        put4cc (frames, id);
        put32be (frames, (juce::uint32) content.size());
        frames.push_back (0);
        frames.push_back (0);
        putBytes (frames, content);
    };

    const auto text = [&frame] (const char* id, const juce::String& value)
    {
        if (value.isEmpty())
            return;

        const auto wide = ! isLatin1 (value);
        Bytes content { (juce::uint8) (wide ? 1 : 0) };
        putBytes (content, id3Text (value, wide, false));
        frame (id, content);
    };

    text ("TIT2", info.title);
    text ("TPE1", info.artist);
    text ("TALB", info.album);
    text ("TCON", info.genre);
    text ("TYER", info.year);
    text ("TRCK", info.track);

    if (info.comment.isNotEmpty())
    {
        const auto wide = ! isLatin1 (info.comment);
        Bytes content { (juce::uint8) (wide ? 1 : 0), 'e', 'n', 'g' };
        putBytes (content, id3Text ({}, wide, true));
        putBytes (content, id3Text (info.comment, wide, false));
        frame ("COMM", content);
    }

    // The cover: Latin-1, its MIME type, picture type 3 (front cover), no description, the image.
    if (info.hasCover())
    {
        Bytes content { 0 };
        putBytes (content, utf8 (info.coverMime()));
        content.push_back (0);
        content.push_back (3);
        content.push_back (0);
        content.insert (content.end(), (const juce::uint8*) info.cover->getData(), (const juce::uint8*) info.cover->getData() + info.cover->getSize());
        frame ("APIC", content);
    }

    Bytes tag { 'I', 'D', '3', 3, 0, 0 };
    const auto size = (juce::uint32) frames.size();

    for (int shift : { 21, 14, 7, 0 })
        tag.push_back ((juce::uint8) ((size >> shift) & 0x7f));

    putBytes (tag, frames);
    return { tag.data(), tag.size() };
}

size_t id3v2Size (const void* data, size_t size)
{
    const auto* p = (const juce::uint8*) data;

    if (size < 10 || std::memcmp (p, "ID3", 3) != 0)
        return 0;

    return 10 + syncsafe (p + 6) + ((p[5] & 0x10) != 0 ? 10 : 0);
}

namespace
{
    /** The tags in an ID3v2.3 or 2.4 tag: its 10-byte header, then body (as much of it as was read). */
    void parseId3v2 (const juce::uint8* head, Bytes body, FileInfo& info)
    {
        if (std::memcmp (head, "ID3", 3) != 0)
            return;

        const auto version = head[3];

        if (version != 3 && version != 4)
            return;

        // Version 2.3 unsynchronises the whole tag: every FF 00 stands for FF.
        if (version == 3 && (head[5] & 0x80) != 0)
        {
            Bytes plain;

            for (size_t i = 0; i < body.size(); ++i)
            {
                plain.push_back (body[i]);

                if (body[i] == 0xff && i + 1 < body.size() && body[i + 1] == 0)
                    ++i;
            }

            body = std::move (plain);
        }

        size_t at = 0;

        if ((head[5] & 0x40) != 0 && body.size() >= 4)
            at = version == 3 ? 4 + get32be (body.data()) : syncsafe (body.data());

        auto commentFound = false;

        while (at + 10 <= body.size() && body[at] != 0)
        {
            const auto* f = body.data() + at;
            const auto size = (size_t) (version == 4 ? syncsafe (f + 4) : get32be (f + 4));
            const juce::String id ((const char*) f, 4);

            if (at + 10 + size > body.size() || size < 1)
                break;

            const auto* data = f + 10;
            const int encoding = data[0];
            juce::String value;

            if (id == "APIC" && size > 4 && ! info.hasCover())
            {
                // Encoding, MIME type (Latin-1, ended by 0), picture type, description (ended by 0 or 00 00), image.
                const auto* end = data + size;
                const auto* p = std::find (data + 1, end, (juce::uint8) 0);

                if (p + 2 < end)
                {
                    p += 2;   // the 0, then the picture type
                    juce::String description;
                    p += id3Decode (encoding, p, (size_t) (end - p), description);

                    if (p < end)
                        info.cover = coverFrom (p, (size_t) (end - p));
                }
            }
            else if (id == "COMM" && ! commentFound && size > 4)
            {
                juce::String description;
                const auto used = id3Decode (encoding, data + 4, size - 4, description);
                id3Decode (encoding, data + 4 + used, size - 4 - used, value);
                info.comment = value.trim();
                commentFound = true;
            }
            else if (id.startsWithChar ('T'))
            {
                id3Decode (encoding, data + 1, size - 1, value);
                value = value.trim();

                if (id == "TIT2")                       info.title = value;
                else if (id == "TPE1")                  info.artist = value;
                else if (id == "TALB")                  info.album = value;
                else if (id == "TYER" || id == "TDRC")  info.year = value;
                else if (id == "TRCK")                  info.track = value;
                else if (id == "TCON")
                    info.genre = value.startsWithChar ('(') && value.containsChar (')') && ! value.endsWithChar (')')
                                     ? value.fromFirstOccurrenceOf (")", false, false) : value;
            }

            at += 10 + size;
        }
    }
}

void readId3v2 (const juce::File& file, FileInfo& info)
{
    juce::FileInputStream in (file);
    juce::uint8 head[10];

    if (! in.openedOk() || in.read (head, 10) != 10 || std::memcmp (head, "ID3", 3) != 0)
        return;

    juce::MemoryBlock block;
    in.readIntoMemoryBlock (block, (ssize_t) syncsafe (head + 6));
    parseId3v2 (head, Bytes ((const juce::uint8*) block.getData(), (const juce::uint8*) block.getData() + block.getSize()), info);
}

//==============================================================================
namespace
{
    /** An AIFF or AIFF-C file's chunks after its FORM header: id and the offset and size of its data. */
    struct IffChunk { juce::String id; juce::int64 offset = 0; juce::uint32 size = 0; };

    std::vector<IffChunk> aiffChunks (juce::FileInputStream& in)
    {
        std::vector<IffChunk> chunks;
        juce::uint8 head[12];

        if (! in.openedOk() || ! in.setPosition (0) || in.read (head, 12) != 12 || std::memcmp (head, "FORM", 4) != 0
            || (std::memcmp (head + 8, "AIFF", 4) != 0 && std::memcmp (head + 8, "AIFC", 4) != 0))
            return chunks;

        while (in.getPosition() + 8 <= in.getTotalLength())
        {
            juce::uint8 h[8];

            if (in.read (h, 8) != 8)
                break;

            const IffChunk chunk { juce::String ((const char*) h, 4), in.getPosition(), get32be (h + 4) };
            chunks.push_back (chunk);

            if (! in.setPosition (chunk.offset + chunk.size + (chunk.size & 1)))
                break;
        }

        return chunks;
    }
}

void readAiff (const juce::File& file, FileInfo& info)
{
    juce::FileInputStream in (file);
    const auto read = [&in] (const IffChunk& chunk)
    {
        juce::MemoryBlock block;
        in.setPosition (chunk.offset);
        in.readIntoMemoryBlock (block, (ssize_t) juce::jmin (chunk.size, (juce::uint32) (16 << 20)));
        return Bytes ((const juce::uint8*) block.getData(), (const juce::uint8*) block.getData() + block.getSize());
    };

    const auto chunks = aiffChunks (in);
    Bytes id3;

    for (const auto& chunk : chunks)
    {
        if (chunk.id == "NAME" || chunk.id == "AUTH" || (chunk.id == "ANNO" && info.comment.isEmpty()))
        {
            const auto text = read (chunk);
            auto& field = chunk.id == "NAME" ? info.title : chunk.id == "AUTH" ? info.artist : info.comment;
            field = textFrom (text.data(), text.size());
        }
        else if ((chunk.id == "ID3 " || chunk.id == "id3 ") && id3.empty())
        {
            id3 = read (chunk);
        }
    }

    // The ID3 tag, when there is one, has the last word.
    if (id3.size() >= 10)
        parseId3v2 (id3.data(), Bytes (id3.begin() + 10, id3.end()), info);
}

juce::String appendAiffTags (const juce::File& file, const FileInfo& info)
{
    if (! info.hasTags())
        return {};

    {
        juce::FileInputStream in (file);

        if (aiffChunks (in).empty())
            return file.getFileName() + " is not an AIFF file.";
    }

    const auto tag = id3v2 (info);
    const auto length = file.getSize();
    const auto pad = (length & 1) != 0;
    Bytes chunk;
    put4cc (chunk, "ID3 ");
    put32be (chunk, (juce::uint32) tag.getSize());
    chunk.insert (chunk.end(), (const juce::uint8*) tag.getData(), (const juce::uint8*) tag.getData() + tag.getSize());

    if ((chunk.size() & 1) != 0)
        chunk.push_back (0);

    if (length + (pad ? 1 : 0) + (juce::int64) chunk.size() > (juce::int64) 0xffffffff)
        return "The tags would take " + file.getFileName() + " past 4 GB.";

    juce::FileOutputStream out (file);

    if (out.failedToOpen())
        return "Couldn't add the tags to " + file.getFileName() + ".";

    if (pad)
        out.writeByte (0);

    out.write (chunk.data(), chunk.size());
    const auto total = out.getPosition();
    Bytes size;
    put32be (size, (juce::uint32) (total - 8));

    if (! out.setPosition (4) || ! out.write (size.data(), 4))
        return "Couldn't add the tags to " + file.getFileName() + ".";

    out.flush();
    return out.getStatus().wasOk() ? juce::String() : "Couldn't add the tags to " + file.getFileName() + ".";
}

//==============================================================================
juce::String addFlacComments (const juce::File& file, const FileInfo& info)
{
    if (! info.hasTags())
        return {};

    const auto failed = "Couldn't add the tags to " + file.getFileName() + ".";
    juce::FileInputStream in (file);
    char magic[4];

    if (! in.openedOk() || in.read (magic, 4) != 4 || std::memcmp (magic, "fLaC", 4) != 0)
        return file.getFileName() + " is not a FLAC file.";

    // Its metadata blocks: STREAMINFO first, then the rest; any Vorbis comment is replaced.
    std::vector<std::pair<int, Bytes>> blocks;
    Bytes padding;
    bool hasPadding = false;

    for (bool last = false; ! last;)
    {
        juce::uint8 h[4];

        if (in.read (h, 4) != 4)
            return failed;

        last = (h[0] & 0x80) != 0;
        const auto type = h[0] & 0x7f;
        const auto length = (size_t) ((h[1] << 16) | (h[2] << 8) | h[3]);
        Bytes data (length);

        if (length > 0 && in.read (data.data(), (int) length) != (int) length)
            return failed;

        if (type == 1)
        {
            padding = std::move (data);
            hasPadding = true;
        }
        else if (type != 4 && type != 6)
        {
            blocks.emplace_back (type, std::move (data));
        }
    }

    if (blocks.empty() || blocks.front().first != 0)
        return failed;

    // The Vorbis comment: vendor, then FIELD=value entries in UTF-8.
    Bytes comment;
    const auto vendor = utf8 ("SpectraBlade");
    put32le (comment, (juce::uint32) vendor.size());
    putBytes (comment, vendor);
    std::vector<Bytes> entries;
    const auto entry = [&entries] (const char* key, const juce::String& value)
    {
        if (value.isNotEmpty())
            entries.push_back (utf8 (juce::String (key) + "=" + value));
    };

    entry ("TITLE", info.title);
    entry ("ARTIST", info.artist);
    entry ("ALBUM", info.album);
    entry ("GENRE", info.genre);
    entry ("DATE", info.year);
    entry ("TRACKNUMBER", info.track.upToFirstOccurrenceOf ("/", false, false).trim());
    entry ("TRACKTOTAL", info.track.containsChar ('/') ? info.track.fromFirstOccurrenceOf ("/", false, false).trim() : juce::String());
    entry ("COMMENT", info.comment);
    put32le (comment, (juce::uint32) entries.size());

    for (const auto& e : entries)
    {
        put32le (comment, (juce::uint32) e.size());
        putBytes (comment, e);
    }

    if (comment.size() >= (1u << 24))
        return "The tags are too long for a FLAC file.";

    blocks.emplace_back (4, std::move (comment));

    if (info.hasCover())
    {
        auto picture = pictureBlock (info);

        if (picture.size() >= (1u << 24))
            return "The cover is too large for a FLAC file.";

        blocks.emplace_back (6, std::move (picture));
    }

    if (hasPadding)
        blocks.emplace_back (1, std::move (padding));

    juce::TemporaryFile temp (file);

    {
        juce::FileOutputStream out (temp.getFile());

        if (out.failedToOpen())
            return failed;

        out.write ("fLaC", 4);

        for (size_t i = 0; i < blocks.size(); ++i)
        {
            const auto& [type, data] = blocks[i];
            const juce::uint8 h[4] { (juce::uint8) (type | (i + 1 == blocks.size() ? 0x80 : 0)), (juce::uint8) (data.size() >> 16),
                                     (juce::uint8) (data.size() >> 8), (juce::uint8) data.size() };
            out.write (h, 4);
            out.write (data.data(), data.size());
        }

        // The audio frames, as they were.
        const auto rest = in.getTotalLength() - in.getPosition();

        if (out.writeFromInputStream (in, rest) != rest)
            return failed;

        out.flush();

        if (! out.getStatus().wasOk())
            return failed;
    }

    return temp.overwriteTargetFileWithTemporary() ? juce::String() : failed;
}

//==============================================================================
void readMp4 (const juce::File& file, FileInfo& info)
{
    juce::FileInputStream in (file);

    if (! in.openedOk())
        return;

    for (const auto& top : topBoxes (in))
    {
        if (top.type != "moov" || top.size > (64 << 20))
            continue;

        juce::MemoryBlock block;
        in.setPosition (top.offset);
        in.readIntoMemoryBlock (block, (ssize_t) top.size);
        const Bytes moov ((const juce::uint8*) block.getData(), (const juce::uint8*) block.getData() + block.getSize());

        const auto udta = findBox (moov, 8, moov.size(), "udta");
        const auto meta = udta.second > 0 ? findBox (moov, udta.first + 8, udta.first + udta.second, "meta") : std::pair<size_t, size_t> {};

        if (meta.second < 12)
            return;

        const auto ilst = findBox (moov, metaChildren (moov, meta.first), meta.first + meta.second, "ilst");

        for (auto at = ilst.first + 8; ilst.second > 0 && at + 8 <= ilst.first + ilst.second;)
        {
            const auto size = (size_t) get32be (moov.data() + at);

            if (size < 8 || at + size > ilst.first + ilst.second)
                break;

            const auto data = findBox (moov, at + 8, at + size, "data");

            if (data.second >= 16)
            {
                const auto* payload = moov.data() + data.first + 16;
                const auto length = data.second - 16;

                for (const auto& key : mp4Keys)
                    if (std::memcmp (moov.data() + at + 4, key.id, 4) == 0)
                        info.*key.field = juce::String::fromUTF8 ((const char*) payload, (int) length).trim();

                if (std::memcmp (moov.data() + at + 4, "covr", 4) == 0 && ! info.hasCover())
                    info.cover = coverFrom (payload, length);

                if (std::memcmp (moov.data() + at + 4, "trkn", 4) == 0 && length >= 6)
                {
                    const auto number = (payload[2] << 8) | payload[3], total = (payload[4] << 8) | payload[5];
                    info.track = number == 0 ? juce::String() : juce::String (number) + (total > 0 ? "/" + juce::String (total) : juce::String());
                }
            }

            at += size;
        }

        return;
    }
}

juce::String addMp4Tags (const juce::File& file, const FileInfo& info)
{
    if (! info.hasTags())
        return {};

    // The items.
    Bytes items;

    for (const auto& key : mp4Keys)
    {
        const auto& value = info.*key.field;

        if (value.isEmpty())
            continue;

        Bytes data;
        put32be (data, 1);   // UTF-8 text
        put32be (data, 0);   // no locale
        putBytes (data, utf8 (value));
        putBytes (items, box (key.id, box ("data", data)));
    }

    if (const auto number = info.track.upToFirstOccurrenceOf ("/", false, false).trim().getIntValue(); number > 0)
    {
        const auto total = info.track.fromFirstOccurrenceOf ("/", false, false).trim().getIntValue();
        Bytes data;
        put32be (data, 0);   // implicit type
        put32be (data, 0);
        Bytes value { 0, 0, (juce::uint8) (number >> 8), (juce::uint8) number, (juce::uint8) (total >> 8), (juce::uint8) total, 0, 0 };
        putBytes (data, value);
        putBytes (items, box ("trkn", box ("data", data)));
    }

    if (info.hasCover())
    {
        Bytes data;
        put32be (data, info.coverMime() == "image/png" ? 14u : 13u);   // PNG or JPEG
        put32be (data, 0);
        data.insert (data.end(), (const juce::uint8*) info.cover->getData(), (const juce::uint8*) info.cover->getData() + info.cover->getSize());
        putBytes (items, box ("covr", box ("data", data)));
    }

    // The moov box and the free box after it.
    juce::int64 moovOffset = -1, moovSize = 0, freeSize = 0;

    {
        juce::FileInputStream in (file);

        if (! in.openedOk())
            return "Couldn't read " + file.getFileName() + ".";

        const auto boxes = topBoxes (in);

        for (size_t i = 0; i + 1 < boxes.size(); ++i)
            if (boxes[i].type == "moov" && boxes[i + 1].type == "free")
                moovOffset = boxes[i].offset, moovSize = boxes[i].size, freeSize = boxes[i + 1].size;
    }

    if (moovOffset < 0 || moovSize > (64 << 20))
        return "There is no room for the tags in " + file.getFileName() + ".";

    Bytes region ((size_t) (moovSize + freeSize));

    {
        juce::FileInputStream in (file);
        in.setPosition (moovOffset);

        if (in.read (region.data(), (int) region.size()) != (int) region.size())
            return "Couldn't read " + file.getFileName() + ".";
    }

    // Where the items go, and which boxes around them grow.
    std::vector<size_t> growing { 0 };
    size_t insertAt = 0;
    Bytes inserted;
    const auto moovEnd = (size_t) moovSize;
    const auto udta = findBox (region, 8, moovEnd, "udta");

    Bytes hdlr;
    put32be (hdlr, 0);
    put32be (hdlr, 0);
    put4cc (hdlr, "mdir");
    put4cc (hdlr, "appl");
    put32be (hdlr, 0);
    put32be (hdlr, 0);
    hdlr.push_back (0);

    const auto newMeta = [&]
    {
        Bytes content;
        put32be (content, 0);   // version and flags
        putBytes (content, box ("hdlr", hdlr));
        putBytes (content, box ("ilst", items));
        return box ("meta", content);
    };

    if (udta.second == 0)
    {
        insertAt = moovEnd;
        inserted = box ("udta", newMeta());
    }
    else
    {
        growing.push_back (udta.first);
        const auto meta = findBox (region, udta.first + 8, udta.first + udta.second, "meta");

        if (meta.second < 12)
        {
            insertAt = udta.first + udta.second;
            inserted = newMeta();
        }
        else
        {
            growing.push_back (meta.first);
            const auto ilst = findBox (region, metaChildren (region, meta.first), meta.first + meta.second, "ilst");

            if (ilst.second == 0)
            {
                insertAt = meta.first + meta.second;
                inserted = box ("ilst", items);
            }
            else
            {
                growing.push_back (ilst.first);
                insertAt = ilst.first + ilst.second;
                inserted = items;
            }
        }
    }

    const auto delta = (juce::int64) inserted.size();
    const auto freeLeft = freeSize - delta;

    if (freeLeft != 0 && freeLeft < 8)
        return "There is no room for the tags in " + file.getFileName() + ".";

    for (auto at : growing)
        set32be (region.data() + at, get32be (region.data() + at) + (juce::uint32) delta);

    // The free box shrinks by as much as moov grows, so the audio stays where it is.
    Bytes rebuilt (region.begin(), region.begin() + (std::ptrdiff_t) insertAt);
    putBytes (rebuilt, inserted);
    rebuilt.insert (rebuilt.end(), region.begin() + (std::ptrdiff_t) insertAt, region.begin() + (std::ptrdiff_t) moovEnd);

    if (freeLeft > 0)
    {
        put32be (rebuilt, (juce::uint32) freeLeft);
        put4cc (rebuilt, "free");
        rebuilt.resize (region.size(), 0);
    }

    jassert (rebuilt.size() == region.size());
    juce::FileOutputStream out (file);

    if (out.failedToOpen() || ! out.setPosition (moovOffset) || ! out.write (rebuilt.data(), rebuilt.size()))
        return "Couldn't add the tags to " + file.getFileName() + ".";

    out.flush();
    return out.getStatus().wasOk() ? juce::String() : "Couldn't add the tags to " + file.getFileName() + ".";
}

//==============================================================================
namespace
{
    /** The tags in a Vorbis comment: vendor, then FIELD=value entries. */
    void parseVorbisComment (const juce::uint8* data, size_t end, FileInfo& info)
    {
        size_t at = 0;

        if (end < 8)
            return;

        const auto vendor = (size_t) get32le (data);
        at += 4 + vendor;

        if (at + 4 > end)
            return;

        const auto count = get32le (data + at);
        at += 4;
        juce::String trackNumber, trackTotal;

        for (juce::uint32 i = 0; i < count && at + 4 <= end; ++i)
        {
            const auto length = (size_t) get32le (data + at);
            at += 4;

            if (at + length > end)
                break;

            const auto entry = juce::String::fromUTF8 ((const char*) data + at, (int) length);
            at += length;
            const auto key = entry.upToFirstOccurrenceOf ("=", false, false).toUpperCase();
            const auto value = entry.fromFirstOccurrenceOf ("=", false, false).trim();

            if (key == "TITLE")                                 info.title = value;
            else if (key == "ARTIST")                           info.artist = value;
            else if (key == "ALBUM")                            info.album = value;
            else if (key == "GENRE")                            info.genre = value;
            else if (key == "DATE" || key == "YEAR")            info.year = value;
            else if (key == "TRACKNUMBER")                      trackNumber = value;
            else if (key == "TRACKTOTAL" || key == "TOTALTRACKS") trackTotal = value;
            else if (key == "COMMENT" || (key == "DESCRIPTION" && info.comment.isEmpty())) info.comment = value;
        }

        if (trackNumber.isNotEmpty())
            info.track = trackNumber.containsChar ('/') || trackTotal.isEmpty() ? trackNumber : trackNumber + "/" + trackTotal;
    }

    /** The image in a FLAC PICTURE block: picture type, MIME type, description, size and colours, data. */
    std::shared_ptr<const juce::MemoryBlock> parsePicture (const juce::uint8* data, size_t size)
    {
        size_t at = 4;

        for (int field = 0; field < 2; ++field)   // the MIME type and the description, each with its length
        {
            if (at + 4 > size)
                return nullptr;

            at += 4 + (size_t) get32be (data + at);
        }

        at += 16;   // width, height, colour depth, colours used

        if (at + 4 > size)
            return nullptr;

        const auto length = (size_t) get32be (data + at);
        at += 4;
        return at + length <= size ? coverFrom (data + at, length) : nullptr;
    }

    /** A FLAC PICTURE block's contents for the cover, as the front cover (type 3). */
    Bytes pictureBlock (const FileInfo& info)
    {
        const auto image = juce::ImageFileFormat::loadFrom (info.cover->getData(), info.cover->getSize());
        Bytes block;
        put32be (block, 3);
        const auto mime = utf8 (info.coverMime());
        put32be (block, (juce::uint32) mime.size());
        putBytes (block, mime);
        put32be (block, 0);   // no description
        put32be (block, (juce::uint32) image.getWidth());
        put32be (block, (juce::uint32) image.getHeight());
        put32be (block, image.hasAlphaChannel() ? 32u : 24u);
        put32be (block, 0);   // not an indexed image
        put32be (block, (juce::uint32) info.cover->getSize());
        block.insert (block.end(), (const juce::uint8*) info.cover->getData(), (const juce::uint8*) info.cover->getData() + info.cover->getSize());
        return block;
    }
}

void readFlac (const juce::File& file, FileInfo& info)
{
    juce::FileInputStream in (file);
    char magic[4];

    if (! in.openedOk() || in.read (magic, 4) != 4 || std::memcmp (magic, "fLaC", 4) != 0)
        return;

    auto frontCover = false;   // a picture marked as the front cover (type 3) wins over others

    for (bool last = false; ! last;)
    {
        juce::uint8 h[4];

        if (in.read (h, 4) != 4)
            return;

        last = (h[0] & 0x80) != 0;
        const auto type = h[0] & 0x7f;
        const auto length = (juce::int64) ((h[1] << 16) | (h[2] << 8) | h[3]);
        const auto next = in.getPosition() + length;

        if (type == 4 || (type == 6 && ! frontCover))
        {
            juce::MemoryBlock block;

            if (in.readIntoMemoryBlock (block, (ssize_t) length) != (size_t) length)
                return;

            const auto* p = (const juce::uint8*) block.getData();

            if (type == 4)
            {
                parseVorbisComment (p, block.getSize(), info);
            }
            else if (block.getSize() >= 4)
            {
                const auto isFront = get32be (p) == 3;

                if (auto picture = parsePicture (p, block.getSize()); picture != nullptr && (isFront || ! info.hasCover()))
                {
                    info.cover = std::move (picture);
                    frontCover = isFront;
                }
            }
        }

        if (! in.setPosition (next))
            return;
    }
}

void readVorbisComments (const juce::File& file, FileInfo& info)
{
    juce::FileInputStream in (file);

    if (! in.openedOk())
        return;

    juce::MemoryBlock head;
    in.readIntoMemoryBlock (head, 1 << 20);
    const Bytes data ((const juce::uint8*) head.getData(), (const juce::uint8*) head.getData() + head.getSize());

    if (data.size() < 4 || std::memcmp (data.data(), "OggS", 4) != 0)
        return;

    const auto findText = [&data] (const char* text, size_t length) -> size_t
    {
        const auto it = std::search (data.begin(), data.end(), text, text + length);
        return it == data.end() ? data.size() : (size_t) (it - data.begin());
    };

    size_t at = 0;

    if (const auto opus = findText ("OpusTags", 8); opus < data.size())
        at = opus + 8;
    else if (const auto vorbis = findText ("\x03vorbis", 7); vorbis < data.size())
        at = vorbis + 7;

    if (at > 0)
        parseVorbisComment (data.data() + at, data.size() - at, info);
}

//==============================================================================
juce::StringArray ffmpegArguments (const FileInfo& info)
{
    juce::StringArray args;
    const std::pair<const char*, const juce::String*> keys[] { { "title", &info.title }, { "artist", &info.artist }, { "album", &info.album },
                                                               { "genre", &info.genre }, { "date", &info.year }, { "track", &info.track },
                                                               { "comment", &info.comment } };

    for (const auto& [key, value] : keys)
        if (value->isNotEmpty())
            args.addArray ({ "-metadata", juce::String (key) + "=" + *value });

    return args;
}

FileInfo read (const juce::File& file, const juce::StringPairArray& metadata)
{
    FileInfo info;

    if (file.hasFileExtension ("wav;wave;bwf"))
    {
        readBroadcast (metadata, info);
        readRiffInfo (file, info);
    }
    else if (file.hasFileExtension ("aif;aiff;aifc"))
    {
        readAiff (file, info);
    }
    else if (file.hasFileExtension ("mp3"))
    {
        readId3v2 (file, info);
    }
    else if (file.hasFileExtension ("m4a;m4b;mp4;mov"))
    {
        readMp4 (file, info);
    }
    else if (file.hasFileExtension ("flac"))
    {
        readFlac (file, info);
    }
    else if (file.hasFileExtension ("ogg;oga;opus;spx"))
    {
        readVorbisComments (file, info);
    }

    return info;
}
}
