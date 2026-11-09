#include "PacketReader.h"

#if JUCE_WINDOWS

PacketReader::PacketReader (juce::InputStream* in, const juce::String& formatName, Stream s, double fileRate,
                            std::unique_ptr<MediaFoundation::Decoder> d)
    : AudioFormatReader (in, formatName), stream (std::move (s)), decoder (std::move (d))
{
    // HE-AAC decodes at twice the rate its file states: positions in the file scale with it.
    scale = juce::jmax (1, juce::roundToInt (decoder->getSampleRate() / fileRate));
    sampleRate = decoder->getSampleRate();
    numChannels = (unsigned int) decoder->getChannels();
    bitsPerSample = 32;
    usesFloatingPointData = true;
    lengthInSamples = stream.length * scale;
}

bool PacketReader::readSamples (int* const* destChannels, int numDestChannels, int startOffsetInDestBuffer,
                                juce::int64 startSampleInFile, int numSamples)
{
    clearSamplesBeyondAvailableLength (destChannels, numDestChannels, startOffsetInDestBuffer, startSampleInFile,
                                       numSamples, lengthInSamples);

    const auto channels = (int) numChannels;
    auto position = stream.skip * scale + startSampleInFile;   // in the decoded stream
    auto done = 0;

    while (done < numSamples)
    {
        if (! decodeUpTo (position))
            break;

        const auto available = (int) juce::jmin ((juce::int64) (numSamples - done), cacheStart + cachedFrames() - position);

        if (available <= 0)
            break;

        const auto* from = cache.data() + (size_t) (position - cacheStart) * (size_t) channels;

        for (int ch = 0; ch < numDestChannels; ++ch)
        {
            if (destChannels[ch] == nullptr)
                continue;

            auto* dest = reinterpret_cast<float*> (destChannels[ch]) + startOffsetInDestBuffer + done;

            if (ch < channels)
                for (int i = 0; i < available; ++i)
                    dest[i] = from[(size_t) i * (size_t) channels + (size_t) ch];
            else
                std::fill (dest, dest + available, 0.0f);
        }

        done += available;
        position += available;
    }

    // Past what the packets give (a damaged file): silence.
    if (done < numSamples)
        for (int ch = 0; ch < numDestChannels; ++ch)
            if (destChannels[ch] != nullptr)
                std::fill (reinterpret_cast<float*> (destChannels[ch]) + startOffsetInDestBuffer + done,
                           reinterpret_cast<float*> (destChannels[ch]) + startOffsetInDestBuffer + numSamples, 0.0f);

    return true;
}

bool PacketReader::decodeUpTo (juce::int64 position)
{
    if (stream.packets.empty())
        return false;

    const auto packetFrames = (juce::int64) stream.packets[0].frames * scale;

    if (cacheStart < 0 || position < cacheStart || position > cacheStart + cachedFrames() + 4 * packetFrames)
    {
        // The packet holding the position, and the ones before it the decoder needs to settle.
        auto k = (size_t) (std::upper_bound (stream.packets.begin(), stream.packets.end(), position,
                                             [this] (juce::int64 p, const Mp4File::Packet& q) { return p < q.start * scale; })
                           - stream.packets.begin());
        k = k > 0 ? k - 1 : 0;
        const auto first = k > (size_t) stream.preroll ? k - (size_t) stream.preroll : 0;
        decoder->reset();
        cache.clear();
        nextPacket = first;
        cacheStart = stream.packets[first].start * scale;
    }

    while (position >= cacheStart + cachedFrames())
    {
        if (nextPacket >= stream.packets.size())
            return false;

        // What is behind the position isn't needed again soon: dropped, keeping the cache small (the
        // position can be past what is cached, by a few packets).
        if (const auto behind = position - cacheStart; behind > 4 * packetFrames)
        {
            const auto frames = juce::jmin (behind - packetFrames, cachedFrames());
            cache.erase (cache.begin(), cache.begin() + (std::ptrdiff_t) ((size_t) frames * numChannels));
            cacheStart += frames;
        }

        const auto& p = stream.packets[nextPacket++];
        packet.setSize (p.size, false);

        if (! input->setPosition (p.offset) || input->read (packet.getData(), (int) p.size) != (int) p.size
            || ! decoder->decode (packet.getData(), p.size, cache))
            return false;
    }

    return true;
}

#endif
