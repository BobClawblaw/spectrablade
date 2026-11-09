#include "GaplessMp3.h"

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
