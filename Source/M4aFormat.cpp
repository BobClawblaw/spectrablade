#include "M4aFormat.h"

#if JUCE_WINDOWS

#include "PacketReader.h"

namespace
{
    const char* const m4aFormatName = "M4A file";
}

M4aFormat::M4aFormat() : AudioFormat (m4aFormatName, ".m4a") {}

juce::AudioFormatReader* M4aFormat::createReaderFor (juce::InputStream* in, bool deleteStreamIfOpeningFails)
{
    std::unique_ptr<juce::InputStream> owned (in);
    auto track = in != nullptr ? Mp4File::readAudioTrack (*in) : std::optional<Mp4File::AudioTrack>();
    juce::String error;
    auto decoder = track.has_value() ? MediaFoundation::Decoder::create (track->codec, track->config, track->sampleRate,
                                                                         track->channels, track->bitsPerSample, error)
                                     : nullptr;

    if (decoder == nullptr || decoder->getChannels() <= 0 || decoder->getSampleRate() <= 0.0)
    {
        if (! deleteStreamIfOpeningFails)
            owned.release();

        return nullptr;
    }

    // AAC's packets overlap: one decoded on its own isn't right until the one before it has been.
    PacketReader::Stream stream { std::move (track->packets), track->skip, track->length, track->codec == "aac" ? 2 : 0 };
    return new PacketReader (owned.release(), m4aFormatName, std::move (stream), track->sampleRate, std::move (decoder));
}

#endif
