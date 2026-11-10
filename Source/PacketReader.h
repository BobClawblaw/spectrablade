#pragma once

#include "Mp4File.h"

#if JUCE_WINDOWS

#include "MediaFoundation.h"

/** Reads a compressed file through one of Windows' decoders, packet by packet, from any position to
    the frame: a read that doesn't follow on from the last starts the decoder again a few packets
    before it (what the codec needs to settle) and skips to it. The packets' times give positions;
    skip and length leave out the encoder's delay and padding. Shared by the .m4a and MP3 readers. */
class PacketReader final : public juce::AudioFormatReader
{
public:
    struct Stream
    {
        std::vector<Mp4File::Packet> packets;   // in order; start and frames in the file's own rate
        juce::int64 skip = 0, length = 0;        // in the file's own rate
        int preroll = 0;                         // packets decoded before the one a read starts in
    };

    /** Takes the stream (owned, and deleted with this) and the decoder; the stream's packets are read
        from it. */
    PacketReader (juce::InputStream*, const juce::String& formatName, Stream, double fileRate,
                  std::unique_ptr<MediaFoundation::Decoder>);

    bool readSamples (int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                      juce::int64 startSampleInFile, int numSamples) override;

private:
    juce::int64 cachedFrames() const noexcept   { return (juce::int64) (cache.size() / numChannels); }
    bool decodeUpTo (juce::int64 position);

    Stream stream;
    std::unique_ptr<MediaFoundation::Decoder> decoder;
    int scale = 1;
    std::vector<float> cache;          // interleaved, from cacheStart, in the decoded rate
    juce::int64 cacheStart = -1;
    size_t nextPacket = 0;
    juce::MemoryBlock packet;
};

#endif
