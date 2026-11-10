#include "Mp4File.h"

namespace Mp4File
{
namespace
{
    using Bytes = std::vector<juce::uint8>;

    //==============================================================================
    // Reading.

    struct View
    {
        const juce::uint8* data = nullptr;
        size_t size = 0;

        juce::uint32 be16 (size_t at) const   { return at + 2 <= size ? (juce::uint32) ((data[at] << 8) | data[at + 1]) : 0; }
        juce::uint32 be32 (size_t at) const   { return at + 4 <= size ? (juce::uint32) juce::ByteOrder::bigEndianInt (data + at) : 0; }
        juce::uint64 be64 (size_t at) const   { return at + 8 <= size ? ((juce::uint64) be32 (at) << 32) | be32 (at + 4) : 0; }
        bool is (size_t at, const char* type) const { return at + 4 <= size && std::memcmp (data + at, type, 4) == 0; }
    };

    /** A box's content: from just after its header to its end. */
    struct Box
    {
        size_t begin = 0, end = 0;
        bool found() const noexcept   { return end > begin || begin != 0; }
    };

    /** Each child box of [begin, end) in turn, with its type and content. */
    template <typename Fn>
    void forEachBox (const View& v, size_t begin, size_t end, Fn&& fn)
    {
        for (auto at = begin; at + 8 <= end;)
        {
            juce::uint64 size = v.be32 (at);
            size_t header = 8;

            if (size == 1)
            {
                size = v.be64 (at + 8);
                header = 16;
            }
            else if (size == 0)
            {
                size = end - at;
            }

            if (size < header || at + size > end)
                return;

            const char type[5] { (char) v.data[at + 4], (char) v.data[at + 5], (char) v.data[at + 6], (char) v.data[at + 7], 0 };

            if (! fn (juce::String (type), Box { at + header, (size_t) (at + size) }))
                return;

            at += (size_t) size;
        }
    }

    Box child (const View& v, Box parent, const char* type, size_t skip = 0)
    {
        Box result;
        forEachBox (v, parent.begin + skip, parent.end, [&] (const juce::String& t, Box b)
        {
            if (t != type)
                return true;

            result = b;
            return false;
        });
        return result;
    }

    Box path (const View& v, Box from, std::initializer_list<const char*> types)
    {
        for (auto* t : types)
        {
            from = child (v, from, t);

            if (! from.found())
                return {};
        }

        return from;
    }

    /** An MPEG-4 descriptor's length: up to four bytes of seven bits. */
    size_t descriptorLength (const View& v, size_t& at, size_t end)
    {
        size_t length = 0;

        for (int i = 0; i < 4 && at < end; ++i)
        {
            const auto b = v.data[at++];
            length = (length << 7) | (b & 0x7f);

            if ((b & 0x80) == 0)
                break;
        }

        return length;
    }

    /** The AudioSpecificConfig in an esds box. */
    juce::MemoryBlock audioSpecificConfig (const View& v, Box esds)
    {
        auto at = esds.begin + 4;   // version and flags

        if (at >= esds.end || v.data[at++] != 0x03)
            return {};

        descriptorLength (v, at, esds.end);
        const auto flags = at + 2 < esds.end ? v.data[at + 2] : 0;
        at += 3;

        if ((flags & 0x80) != 0)  at += 2;
        if ((flags & 0x40) != 0)  at += 1 + (at < esds.end ? v.data[at] : 0);
        if ((flags & 0x20) != 0)  at += 2;

        if (at >= esds.end || v.data[at++] != 0x04)
            return {};

        descriptorLength (v, at, esds.end);
        at += 13;

        if (at >= esds.end || v.data[at++] != 0x05)
            return {};

        const auto length = descriptorLength (v, at, esds.end);

        if (at + length > esds.end)
            return {};

        return juce::MemoryBlock (v.data + at, length);
    }

    /** iTunSMPB, which iTunes and some encoders write instead of an edit list: the delay, the padding
        and the length, in hexadecimal. */
    std::optional<std::array<juce::int64, 3>> iTunSmpb (const View& v)
    {
        static const char key[] = "iTunSMPB";

        for (size_t i = 0; i + 8 < v.size; ++i)
        {
            if (std::memcmp (v.data + i, key, 8) != 0)
                continue;

            for (auto j = i + 8; j + 16 < v.size && j < i + 64; ++j)
            {
                if (! v.is (j, "data"))
                    continue;

                const auto boxEnd = juce::jmin (v.size, j - 4 + (size_t) v.be32 (j - 4));
                const auto text = juce::String::fromUTF8 ((const char*) v.data + j + 12, (int) (boxEnd - (j + 12)));
                const auto fields = juce::StringArray::fromTokens (text.trim(), " ", {});

                if (fields.size() < 4)
                    return {};

                return std::array<juce::int64, 3> { fields[1].getHexValue64(), fields[2].getHexValue64(), fields[3].getHexValue64() };
            }

            return {};
        }

        return {};
    }

    std::optional<AudioTrack> trackFrom (const View& v, Box trak, juce::uint32 movieScale)
    {
        const auto mdia = child (v, trak, "mdia");
        const auto hdlr = child (v, mdia, "hdlr");

        if (! hdlr.found() || ! v.is (hdlr.begin + 8, "soun"))
            return {};

        const auto mdhd = child (v, mdia, "mdhd");
        const auto version = mdhd.found() ? v.data[mdhd.begin] : 0;
        const auto mediaScale = v.be32 (mdhd.begin + (version == 1 ? 20 : 12));
        const auto stbl = path (v, mdia, { "minf", "stbl" });
        const auto stsd = child (v, stbl, "stsd");

        if (mediaScale == 0 || ! stsd.found())
            return {};

        AudioTrack t;
        t.sampleRate = mediaScale;

        // The first sample entry: an AudioSampleEntry, its version saying how long its fields run.
        Box entry;
        juce::String format;
        forEachBox (v, stsd.begin + 8, stsd.end, [&] (const juce::String& type, Box b) { format = type; entry = b; return false; });

        if (format != "mp4a" && format != "alac")
            return {};

        const auto entryVersion = v.be16 (entry.begin + 8);
        t.channels = (int) v.be16 (entry.begin + 16);
        t.bitsPerSample = (int) v.be16 (entry.begin + 18);
        const auto children = entry.begin + 28 + (entryVersion == 1 ? 16 : entryVersion == 2 ? 36 : 0);
        const Box inner { children, entry.end };

        if (format == "mp4a")
        {
            auto esds = child (v, inner, "esds");

            if (! esds.found())
                esds = path (v, inner, { "wave", "esds" });   // QuickTime puts it in a wave box

            t.codec = "aac";
            t.config = esds.found() ? audioSpecificConfig (v, esds) : juce::MemoryBlock();
        }
        else
        {
            const auto config = child (v, inner, "alac");

            if (! config.found() || config.end - config.begin < 28)
                return {};

            t.codec = "alac";
            t.config = juce::MemoryBlock (v.data + config.begin + 4, 24);
            t.bitsPerSample = v.data[config.begin + 4 + 5];
            t.channels = v.data[config.begin + 4 + 9];
        }

        if (t.config.isEmpty() || t.channels <= 0)
            return {};

        // Sizes, durations and where each chunk of packets starts.
        const auto stsz = child (v, stbl, "stsz");
        const auto stts = child (v, stbl, "stts");
        const auto stsc = child (v, stbl, "stsc");
        auto stco = child (v, stbl, "stco");
        const auto co64 = ! stco.found() ? child (v, stbl, "co64") : Box();

        if (! stsz.found() || ! stts.found() || ! stsc.found() || (! stco.found() && ! co64.found()))
            return {};

        const auto count = v.be32 (stsz.begin + 8);
        const auto fixedSize = v.be32 (stsz.begin + 4);

        if (count == 0 || (fixedSize == 0 && stsz.begin + 12 + (size_t) count * 4 > stsz.end))
            return {};

        t.packets.resize (count);

        for (juce::uint32 i = 0; i < count; ++i)
            t.packets[i].size = fixedSize != 0 ? fixedSize : v.be32 (stsz.begin + 12 + (size_t) i * 4);

        juce::uint32 next = 0;
        juce::int64 time = 0;

        for (juce::uint32 e = 0, entries = v.be32 (stts.begin + 4); e < entries && next < count; ++e)
        {
            const auto n = v.be32 (stts.begin + 8 + (size_t) e * 8), delta = v.be32 (stts.begin + 12 + (size_t) e * 8);

            for (juce::uint32 k = 0; k < n && next < count; ++k, ++next)
            {
                t.packets[next].start = time;
                t.packets[next].frames = delta;
                time += delta;
            }
        }

        t.decodedFrames = time;
        const auto chunks = co64.found() ? v.be32 (co64.begin + 4) : v.be32 (stco.begin + 4);
        const auto chunkOffset = [&] (juce::uint32 c) -> juce::int64
        {
            return co64.found() ? (juce::int64) v.be64 (co64.begin + 8 + (size_t) c * 8) : (juce::int64) v.be32 (stco.begin + 8 + (size_t) c * 4);
        };

        const auto stscEntries = v.be32 (stsc.begin + 4);
        juce::uint32 packet = 0, e = 0;

        for (juce::uint32 c = 0; c < chunks && packet < count; ++c)
        {
            while (e + 1 < stscEntries && v.be32 (stsc.begin + 8 + (size_t) (e + 1) * 12) <= c + 1)
                ++e;

            const auto perChunk = v.be32 (stsc.begin + 12 + (size_t) e * 12);
            auto offset = chunkOffset (c);

            for (juce::uint32 k = 0; k < perChunk && packet < count; ++k, ++packet)
            {
                t.packets[packet].offset = offset;
                offset += t.packets[packet].size;
            }
        }

        if (packet < count)
            return {};

        // What is meant to be heard: the edit list's first edit into the media, else iTunSMPB, else all.
        t.length = t.decodedFrames;

        if (const auto elst = path (v, trak, { "edts", "elst" }); elst.found())
        {
            const auto v1 = v.data[elst.begin] == 1;
            const auto entrySize = v1 ? 20u : 12u;

            for (juce::uint32 i = 0, n = v.be32 (elst.begin + 4); i < n; ++i)
            {
                const auto at = elst.begin + 8 + (size_t) i * entrySize;
                const auto segment = v1 ? (juce::int64) v.be64 (at) : (juce::int64) v.be32 (at);
                const auto mediaTime = v1 ? (juce::int64) v.be64 (at + 8) : (juce::int64) (juce::int32) v.be32 (at + 4);

                if (mediaTime < 0)
                    continue;   // an empty edit: a gap before the media, not part of the file's audio

                t.skip = mediaTime;
                t.length = segment > 0 && movieScale > 0 ? (juce::int64) std::llround ((double) segment * mediaScale / movieScale)
                                                         : t.decodedFrames - t.skip;
                break;
            }
        }
        else if (const auto smpb = iTunSmpb (v))
        {
            t.skip = (*smpb)[0];
            t.length = (*smpb)[2] > 0 ? (*smpb)[2] : t.decodedFrames - (*smpb)[0] - (*smpb)[1];
        }

        t.skip = juce::jlimit ((juce::int64) 0, t.decodedFrames, t.skip);
        t.length = juce::jlimit ((juce::int64) 0, t.decodedFrames - t.skip, t.length);
        return t;
    }

    //==============================================================================
    // Writing.

    void put8 (Bytes& b, juce::uint32 x)    { b.push_back ((juce::uint8) x); }
    void put16 (Bytes& b, juce::uint32 x)   { put8 (b, x >> 8); put8 (b, x); }
    void put24 (Bytes& b, juce::uint32 x)   { put8 (b, x >> 16); put16 (b, x); }
    void put32 (Bytes& b, juce::uint32 x)   { put16 (b, x >> 16); put16 (b, x); }
    void put4cc (Bytes& b, const char* t)   { b.insert (b.end(), t, t + 4); }
    void putBytes (Bytes& b, const Bytes& more) { b.insert (b.end(), more.begin(), more.end()); }
    void putZeros (Bytes& b, size_t n)      { b.insert (b.end(), n, 0); }

    Bytes box (const char* type, const Bytes& content)
    {
        Bytes b;
        put32 (b, (juce::uint32) (content.size() + 8));
        put4cc (b, type);
        putBytes (b, content);
        return b;
    }

    Bytes fullBox (const char* type, juce::uint32 versionAndFlags, const Bytes& content)
    {
        Bytes c;
        put32 (c, versionAndFlags);
        putBytes (c, content);
        return box (type, c);
    }

    void putMatrix (Bytes& b)
    {
        for (auto x : { 0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u })
            put32 (b, x);
    }

    Bytes esds (const AacTrack& t, juce::uint32 bufferSize)
    {
        const auto asc = (juce::uint32) t.audioSpecificConfig.getSize();
        Bytes d;
        put8 (d, 0x03);                  // ES_Descriptor
        put8 (d, 3 + (2 + 13 + 2 + asc) + (2 + 1));
        put16 (d, 0);                    // ES_ID
        put8 (d, 0);                     // no dependency, URL or OCR
        put8 (d, 0x04);                  // DecoderConfigDescriptor
        put8 (d, 13 + 2 + asc);
        put8 (d, 0x40);                  // MPEG-4 audio
        put8 (d, 0x15);                  // an audio stream
        put24 (d, bufferSize);
        put32 (d, t.maxBitRate);
        put32 (d, t.averageBitRate);
        put8 (d, 0x05);                  // DecoderSpecificInfo: the AudioSpecificConfig
        put8 (d, asc);
        d.insert (d.end(), (const juce::uint8*) t.audioSpecificConfig.getData(), (const juce::uint8*) t.audioSpecificConfig.getData() + asc);
        put8 (d, 0x06);                  // SLConfigDescriptor
        put8 (d, 1);
        put8 (d, 0x02);
        return fullBox ("esds", 0, d);
    }

    Bytes moov (const AacTrack& t, juce::uint32 chunkOffset)
    {
        const auto scale = (juce::uint32) juce::roundToInt (t.sampleRate);
        const auto packets = (juce::uint32) t.packetSizes.size();
        const auto mediaFrames = (juce::uint32) packets * 1024u;
        const auto frames = (juce::uint32) t.frames;
        const auto biggest = t.packetSizes.empty() ? 0u : *std::max_element (t.packetSizes.begin(), t.packetSizes.end());

        Bytes mvhd;
        put32 (mvhd, 0); put32 (mvhd, 0);          // created, modified
        put32 (mvhd, scale);
        put32 (mvhd, frames);
        put32 (mvhd, 0x00010000);                    // rate 1.0
        put16 (mvhd, 0x0100);                        // volume 1.0
        putZeros (mvhd, 10);
        putMatrix (mvhd);
        putZeros (mvhd, 24);
        put32 (mvhd, 2);                             // next track id

        Bytes tkhd;
        put32 (tkhd, 0); put32 (tkhd, 0);
        put32 (tkhd, 1);                             // track id
        put32 (tkhd, 0);
        put32 (tkhd, frames);
        putZeros (tkhd, 8);
        put16 (tkhd, 0); put16 (tkhd, 1);            // layer, alternate group
        put16 (tkhd, 0x0100); put16 (tkhd, 0);       // volume
        putMatrix (tkhd);
        put32 (tkhd, 0); put32 (tkhd, 0);            // no picture

        Bytes elst;
        put32 (elst, 1);
        put32 (elst, frames);                        // in the movie's time scale, which is the sample rate
        put32 (elst, (juce::uint32) t.priming);      // where it starts in the media
        put32 (elst, 0x00010000);

        Bytes mdhd;
        put32 (mdhd, 0); put32 (mdhd, 0);
        put32 (mdhd, scale);
        put32 (mdhd, mediaFrames);
        put16 (mdhd, 0x55c4);                        // "und"
        put16 (mdhd, 0);

        Bytes hdlr;
        put32 (hdlr, 0);
        put4cc (hdlr, "soun");
        putZeros (hdlr, 12);
        for (auto c : juce::String ("SoundHandler"))
            put8 (hdlr, (juce::uint32) c);
        put8 (hdlr, 0);

        Bytes smhd;
        put32 (smhd, 0);

        Bytes dref;
        put32 (dref, 1);
        putBytes (dref, fullBox ("url ", 1, {}));

        Bytes mp4a;
        putZeros (mp4a, 6);
        put16 (mp4a, 1);                             // data reference
        putZeros (mp4a, 8);
        put16 (mp4a, (juce::uint32) t.channels);
        put16 (mp4a, 16);
        put32 (mp4a, 0);
        put32 (mp4a, scale << 16);
        putBytes (mp4a, esds (t, biggest));

        Bytes stsd;
        put32 (stsd, 1);
        putBytes (stsd, box ("mp4a", mp4a));

        Bytes stts;
        put32 (stts, 1);
        put32 (stts, packets);
        put32 (stts, 1024);

        Bytes stsc;                                  // every packet in one chunk
        put32 (stsc, 1);
        put32 (stsc, 1); put32 (stsc, packets); put32 (stsc, 1);

        Bytes stsz;
        put32 (stsz, 0);
        put32 (stsz, packets);
        for (auto s : t.packetSizes)
            put32 (stsz, s);

        Bytes stco;
        put32 (stco, 1);
        put32 (stco, chunkOffset);

        Bytes stbl;
        putBytes (stbl, fullBox ("stsd", 0, stsd));
        putBytes (stbl, fullBox ("stts", 0, stts));
        putBytes (stbl, fullBox ("stsc", 0, stsc));
        putBytes (stbl, fullBox ("stsz", 0, stsz));
        putBytes (stbl, fullBox ("stco", 0, stco));

        Bytes minf;
        putBytes (minf, fullBox ("smhd", 0, smhd));
        putBytes (minf, box ("dinf", fullBox ("dref", 0, dref)));
        putBytes (minf, box ("stbl", stbl));

        Bytes mdia;
        putBytes (mdia, fullBox ("mdhd", 0, mdhd));
        putBytes (mdia, fullBox ("hdlr", 0, hdlr));
        putBytes (mdia, box ("minf", minf));

        Bytes trak;
        putBytes (trak, fullBox ("tkhd", 7, tkhd));  // enabled, in the movie, in previews
        putBytes (trak, box ("edts", fullBox ("elst", 0, elst)));
        putBytes (trak, box ("mdia", mdia));

        Bytes content;
        putBytes (content, fullBox ("mvhd", 0, mvhd));
        putBytes (content, box ("trak", trak));
        return box ("moov", content);
    }
}

//==============================================================================
std::optional<AudioTrack> readAudioTrack (juce::InputStream& in)
{
    // The moov box, wherever it is among the top-level boxes, read whole.
    juce::MemoryBlock moov;

    for (juce::int64 at = 0, total = in.getTotalLength(); at + 8 <= total;)
    {
        juce::uint8 header[16] {};

        if (! in.setPosition (at) || in.read (header, 8) != 8)
            return {};

        juce::uint64 size = juce::ByteOrder::bigEndianInt (header);
        juce::int64 headerSize = 8;

        if (size == 1)
        {
            if (in.read (header + 8, 8) != 8)
                return {};

            size = ((juce::uint64) juce::ByteOrder::bigEndianInt (header + 8) << 32) | juce::ByteOrder::bigEndianInt (header + 12);
            headerSize = 16;
        }
        else if (size == 0)
        {
            size = (juce::uint64) (total - at);
        }

        if (size < (juce::uint64) headerSize || at + (juce::int64) size > total)
            return {};

        if (std::memcmp (header + 4, "moov", 4) == 0)
        {
            if (size > (256u << 20))
                return {};

            moov.setSize ((size_t) size);
            in.setPosition (at);

            if (in.read (moov.getData(), (int) size) != (int) size)
                return {};

            break;
        }

        at += (juce::int64) size;
    }

    if (moov.isEmpty())
        return {};

    const View v { (const juce::uint8*) moov.getData(), moov.getSize() };
    const Box all { 8, v.size };
    const auto mvhd = child (v, all, "mvhd");
    const auto movieScale = mvhd.found() ? v.be32 (mvhd.begin + (v.data[mvhd.begin] == 1 ? 20 : 12)) : 0;
    std::optional<AudioTrack> track;

    forEachBox (v, all.begin, all.end, [&] (const juce::String& type, Box b)
    {
        if (type == "trak")
            track = trackFrom (v, b, movieScale);

        return ! track.has_value();
    });

    return track;
}

juce::String writeAac (juce::OutputStream& out, const AacTrack& t, juce::InputStream& payload, juce::int64 freeBytes)
{
    juce::uint64 payloadSize = 0;

    for (auto s : t.packetSizes)
        payloadSize += s;

    Bytes ftyp;
    put4cc (ftyp, "M4A ");
    put32 (ftyp, 0);
    for (auto* brand : { "M4A ", "mp42", "isom" })
        put4cc (ftyp, brand);

    const auto ftypBox = box ("ftyp", ftyp);
    const auto bigMdat = payloadSize + 8 > 0xffffffffu;
    const auto mdatHeader = bigMdat ? 16u : 8u;
    const auto freeSize = (juce::uint32) juce::jlimit ((juce::int64) 8, (juce::int64) 0x7fffffff, freeBytes);

    // The moov box's size doesn't depend on the chunk offset it holds, so it is made once to be measured.
    const auto moovSize = moov (t, 0).size();
    const auto chunkOffset = ftypBox.size() + moovSize + freeSize + mdatHeader;

    if (chunkOffset > 0xffffffffu)
        return "The file's header is too large.";

    Bytes head = ftypBox;
    putBytes (head, moov (t, (juce::uint32) chunkOffset));

    Bytes free;
    putZeros (free, freeSize - 8);
    putBytes (head, box ("free", free));

    if (bigMdat)
    {
        put32 (head, 1);
        put4cc (head, "mdat");
        put32 (head, (juce::uint32) ((payloadSize + 16) >> 32));
        put32 (head, (juce::uint32) (payloadSize + 16));
    }
    else
    {
        put32 (head, (juce::uint32) (payloadSize + 8));
        put4cc (head, "mdat");
    }

    if (! out.write (head.data(), head.size()))
        return "Writing failed (disk full?).";

    if (out.writeFromInputStream (payload, (juce::int64) payloadSize) != (juce::int64) payloadSize)
        return "Writing failed (disk full?).";

    out.flush();
    return {};
}
}
