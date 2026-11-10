#include "GaplessMp3.h"

#if JUCE_WINDOWS
 #include "PacketReader.h"

namespace
{
    /** The samples Windows' MP3 decoder adds in front: one, where JUCE's adds 529 (it makes up the
        rest of the 528 a Layer III decoder delays by itself). Measured against ffmpeg's gapless decode. */
    constexpr int windowsDecoderDelay = 1;

    /** An MPEG audio frame header, Layer III only (what Windows' MP3 decoder takes). */
    struct FrameHeader
    {
        int version = 0;        // 1 for MPEG-1, 2 for MPEG-2, 25 for MPEG-2.5
        int sampleRate = 0, channels = 0, size = 0, samples = 0;

        static std::optional<FrameHeader> parse (const juce::uint8* h)
        {
            if (h[0] != 0xff || (h[1] & 0xe0) != 0xe0 || ((h[1] >> 1) & 3) != 1)   // sync, Layer III
                return {};

            static const int rates[3][3] { { 44100, 48000, 32000 }, { 22050, 24000, 16000 }, { 11025, 12000, 8000 } };
            static const int kbps1[16] { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
            static const int kbps2[16] { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
            const auto versionBits = (h[1] >> 3) & 3, bitrateIndex = h[2] >> 4, rateIndex = (h[2] >> 2) & 3;

            if (versionBits == 1 || bitrateIndex == 0 || bitrateIndex == 15 || rateIndex == 3)   // reserved, free format, bad
                return {};

            FrameHeader f;
            f.version = versionBits == 3 ? 1 : versionBits == 2 ? 2 : 25;
            f.sampleRate = rates[f.version == 1 ? 0 : f.version == 2 ? 1 : 2][rateIndex];
            const auto kbps = (f.version == 1 ? kbps1 : kbps2)[bitrateIndex];
            const auto padding = (h[2] >> 1) & 1;
            f.size = (f.version == 1 ? 144000 : 72000) * kbps / f.sampleRate + padding;
            f.samples = f.version == 1 ? 1152 : 576;
            f.channels = (h[3] >> 6) == 3 ? 1 : 2;
            return f;
        }

        bool sameStream (const FrameHeader& o) const noexcept
        {
            return version == o.version && sampleRate == o.sampleRate && channels == o.channels;
        }
    };

    struct Scanned
    {
        PacketReader::Stream stream;
        FrameHeader first;
    };

    /** Every audio frame of the MP3 at the stream's position (after its ID3v2 tag), leaving out the
        Xing or Info frame; the stream is left where it was. Nothing when it isn't Layer III. */
    std::optional<Scanned> scanFrames (juce::InputStream& source, const std::optional<GaplessMp3Format::Gap>& gap)
    {
        const auto start = source.getPosition();
        const juce::ScopeGuard rewind { [&] { source.setPosition (start); } };
        juce::BufferedInputStream in (&source, 1 << 16, false);
        const auto total = source.getTotalLength();
        juce::uint8 h[16] {};

        if (in.read (h, 10) != 10)
            return {};

        auto at = start;

        if (h[0] == 'I' && h[1] == 'D' && h[2] == '3')
            at += 10 + (((juce::int64) (h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) | ((h[8] & 0x7f) << 7) | (h[9] & 0x7f))
                     + ((h[5] & 0x10) != 0 ? 10 : 0);

        const auto headerAt = [&] (juce::int64 pos) -> std::optional<FrameHeader>
        {
            if (pos + 4 > total || ! in.setPosition (pos) || in.read (h, 4) != 4)
                return {};

            return FrameHeader::parse (h);
        };

        // The first frame: one whose header is followed by another's.
        std::optional<FrameHeader> first;

        for (const auto limit = at + 65536; at < limit && at + 4 <= total; ++at)
        {
            if (const auto f = headerAt (at); f.has_value() && headerAt (at + f->size).has_value())
            {
                first = f;
                break;
            }
        }

        if (! first.has_value())
            return {};

        Scanned s;
        s.first = *first;

        // A Xing or Info frame first holds no audio: decoders leave it out.
        {
            const auto sideInfo = first->version == 1 ? (first->channels == 1 ? 17 : 32) : (first->channels == 1 ? 9 : 17);
            juce::uint8 tag[4] {};

            if (in.setPosition (at + 4 + sideInfo) && in.read (tag, 4) == 4
                && (std::memcmp (tag, "Xing", 4) == 0 || std::memcmp (tag, "Info", 4) == 0))
                at += first->size;
        }

        juce::int64 time = 0;

        while (at + 4 <= total)
        {
            auto f = headerAt (at);

            if (! f.has_value() || ! f->sameStream (*first))
            {
                // Tags at the end, or damage: the next frame within a few kilobytes that another follows.
                if (std::memcmp (h, "TAG", 3) == 0 || std::memcmp (h, "APET", 4) == 0 || std::memcmp (h, "LYRI", 4) == 0)
                    break;

                f.reset();

                for (auto pos = at + 1, limit = at + 4096; pos < limit && pos + 4 <= total; ++pos)
                {
                    if (const auto g = headerAt (pos); g.has_value() && g->sameStream (*first) && headerAt (pos + g->size).has_value())
                    {
                        at = pos;
                        f = g;
                        break;
                    }
                }

                if (! f.has_value())
                    break;
            }

            if (at + f->size > total)
                break;

            s.stream.packets.push_back ({ at, (juce::uint32) f->size, time, (juce::uint32) f->samples });
            time += f->samples;
            at += f->size;
        }

        if (s.stream.packets.empty())
            return {};

        // The decoder's delay shifts everything; the encoder's delay and padding are left out when the
        // tag gives them (the padding being at least the decoder's delay).
        const auto delay = gap.has_value() ? (juce::int64) gap->delay : 0;
        const auto padding = gap.has_value() ? (juce::int64) gap->padding : (juce::int64) windowsDecoderDelay;
        s.stream.skip = delay + windowsDecoderDelay;
        s.stream.length = juce::jmax ((juce::int64) 0, time - delay - padding);
        // Frames decoded before the one a read starts in: enough that the bit reservoir (which reaches
        // back 511 bytes of audio data in MPEG-1, 255 in MPEG-2 and 2.5) has all it needs for the frame
        // before it, whose output overlaps the first one's, counting what this stream's smallest frames
        // hold besides their header and side information, and a few over. A 22.05 kHz V9 file's 26-byte
        // frames hold 13 bytes: 23 frames, where 10 do at 44.1 kHz.
        auto smallest = s.stream.packets.front().size;

        for (const auto& p : s.stream.packets)
            smallest = juce::jmin (smallest, p.size);

        const auto reservoir = first->version == 1 ? 511 : 255;
        const auto sideInfo = first->version == 1 ? (first->channels == 1 ? 17 : 32) : (first->channels == 1 ? 9 : 17);
        const auto audioBytes = juce::jmax (1, (int) smallest - 4 - sideInfo);
        s.stream.preroll = juce::jmax (10, (reservoir + audioBytes - 1) / audioBytes + 3);
        return s;
    }
}
#endif

std::optional<GaplessMp3Format::Gap> GaplessMp3Format::readGap (juce::InputStream& in)
{
    const auto start = in.getPosition();
    const juce::ScopeGuard rewind { [&] { in.setPosition (start); } };

    // An ID3v2 tag first, skipped by its size (synchsafe: 7 bits a byte), and its footer if it has one.
    juce::uint8 id3[10] {};

    if (in.read (id3, 10) != 10)
        return {};

    auto frameStart = start;

    if (id3[0] == 'I' && id3[1] == 'D' && id3[2] == '3')
    {
        const auto size = ((juce::int64) (id3[6] & 0x7f) << 21) | ((id3[7] & 0x7f) << 14) | ((id3[8] & 0x7f) << 7) | (id3[9] & 0x7f);
        frameStart += 10 + size + ((id3[5] & 0x10) != 0 ? 10 : 0);
    }

    // The first frame: its header, the side information, then "Xing" or "Info".
    juce::uint8 frame[256] {};

    if (! in.setPosition (frameStart) || in.read (frame, (int) sizeof (frame)) != (int) sizeof (frame))
        return {};

    if (frame[0] != 0xff || (frame[1] & 0xe0) != 0xe0)
        return {};

    const auto mpeg1 = (frame[1] & 0x18) == 0x18;
    const auto mono = (frame[3] & 0xc0) == 0xc0;
    const auto sideInfo = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    const auto* xing = frame + 4 + sideInfo;

    if (std::memcmp (xing, "Xing", 4) != 0 && std::memcmp (xing, "Info", 4) != 0)
        return {};

    const auto flags = xing[7];
    auto lame = 8 + ((flags & 1) != 0 ? 4 : 0) + ((flags & 2) != 0 ? 4 : 0) + ((flags & 4) != 0 ? 100 : 0) + ((flags & 8) != 0 ? 4 : 0);

    if (4 + sideInfo + lame + 24 > (int) sizeof (frame))
        return {};

    // The LAME extension: a 9-character encoder name ("LAME3.100", "Lavc61.19"), then at byte 21 the delay
    // and padding, 12 bits each.
    const auto* ext = xing + lame;

    for (int i = 0; i < 4; ++i)
        if (! juce::CharacterFunctions::isLetterOrDigit ((juce::juce_wchar) ext[i]))
            return {};

    Gap gap;
    gap.delay = (ext[21] << 4) | (ext[22] >> 4);
    gap.padding = ((ext[22] & 0x0f) << 8) | ext[23];

    // Larger than a few frames isn't a delay any encoder writes: not to be trusted.
    if (gap.delay > 4 * 1152 || gap.padding > 4 * 1152)
        return {};

    return gap;
}

juce::AudioFormatReader* GaplessMp3Format::createReaderFor (juce::InputStream* in, bool deleteStreamIfOpeningFails)
{
    const auto gap = in != nullptr ? readGap (*in) : std::optional<Gap>();

   #if JUCE_WINDOWS
    // Windows' own decoder, through a reader that finds any position to the frame (JUCE's MP3 reader
    // lands a frame or more away when it seeks).
    if (in != nullptr)
    {
        if (auto scanned = scanFrames (*in, gap))
        {
            // MPEGLAYER3WAVEFORMAT's own fields: id, flags, block size, frames a block, codec delay.
            juce::MemoryBlock layer3;
            const auto put16 = [&layer3] (int x) { const juce::uint8 b[] { (juce::uint8) x, (juce::uint8) (x >> 8) }; layer3.append (b, 2); };
            put16 (1);
            put16 (0);
            put16 (0);
            put16 (scanned->first.size);
            put16 (1);
            put16 (1393);
            juce::String error;

            if (auto decoder = MediaFoundation::Decoder::create ("mp3", layer3, scanned->first.sampleRate, scanned->first.channels, 16, error))
                return new PacketReader (in, getFormatName(), std::move (scanned->stream), scanned->first.sampleRate, std::move (decoder));
        }
    }
   #endif
    auto* reader = MP3AudioFormat::createReaderFor (in, deleteStreamIfOpeningFails);

    if (reader == nullptr || ! gap.has_value())
        return reader;

    // The decoder's own delay shifts everything: past the encoder's delay at the start, and taken off
    // the padding at the end (which LAME makes at least that long).
    const auto skip = (juce::int64) gap->delay + decoderDelay;
    const auto length = reader->lengthInSamples - gap->delay - gap->padding;

    if (length <= 0)
        return reader;

    auto metadata = reader->metadataValues;
    auto* trimmed = new juce::AudioSubsectionReader (reader, skip, length, true);
    trimmed->metadataValues = std::move (metadata);
    return trimmed;
}
