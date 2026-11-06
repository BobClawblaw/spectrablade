#include "AudioFileIO.h"
#include "AudioEdits.h"
#include "Ffmpeg.h"

namespace AudioFileIO
{
namespace
{
    constexpr int framesPerBlock = 1 << 20;
    constexpr int framesPerWrite = 1 << 16;

    juce::AudioFormat* formatFor (const juce::File& file, juce::AudioFormatManager& formats)
    {
        return formats.findFormatForFileExtension (file.getFileExtension());
    }
}

Loaded load (const juce::File& file, juce::AudioFormatManager& formats)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    // Formats read through ffmpeg: decoded to a temporary WAV, which is read and then deleted.
    if (reader == nullptr && file.existsAsFile())
    {
        const auto ffmpeg = Ffmpeg::find();

        if (ffmpeg == juce::File())
            return { nullptr, 0.0, 0, Ffmpeg::importExtensions().contains (file.getFileExtension().substring (1).toLowerCase())
                                          ? "Opening " + file.getFileExtension() + " files needs ffmpeg, which isn't installed."
                                          : "Unsupported or unreadable audio file.", {}, {} };

        juce::TemporaryFile wav (".wav");

        if (const auto error = Ffmpeg::decode (ffmpeg, file, wav.getFile()); error.isNotEmpty())
            return { nullptr, 0.0, 0, error, {}, {} };

        auto loaded = load (wav.getFile(), formats);
        loaded.markers = {};
        return loaded;
    }

    if (reader == nullptr)
        return { nullptr, 0.0, 0, "Unsupported or unreadable audio file.", {}, {} };

    const auto channels = (int) reader->numChannels;
    const auto total = reader->lengthInSamples;

    if (channels <= 0 || total < 0)
        return { nullptr, 0.0, 0, "The file has no audio channels.", {}, {} };

    std::vector<AudioSnapshot::Segment> segments;

    for (juce::int64 pos = 0; pos < total; pos += framesPerBlock)
    {
        const auto count = (int) juce::jmin ((juce::int64) framesPerBlock, total - pos);
        juce::AudioBuffer<float> buffer (channels, count);

        if (! reader->read (&buffer, 0, count, pos, true, true))
            return { nullptr, 0.0, 0, "Reading failed at frame " + juce::String (pos) + ".", {}, {} };

        segments.push_back ({ std::make_shared<const AudioBlock> (std::move (buffer)), 0, count });
    }

    const auto bitDepth = reader->usesFloatingPointData ? 32 : (int) reader->bitsPerSample;
    return { std::make_shared<const AudioSnapshot> (channels, std::move (segments)), reader->sampleRate, bitDepth, {},
             MarkerEdits::fromMetadata (reader->metadataValues, total), FileInfoIO::read (file, reader->metadataValues) };
}

juce::Array<int> bitDepthsFor (const juce::File& file, juce::AudioFormatManager& formats)
{
    juce::Array<int> result;

    if (auto* format = formatFor (file, formats))
    {
        // Prefer float, then the deepest integer format.
        for (auto depth : { 32, 24, 16, 8 })
            if (format->getPossibleBitDepths().contains (depth))
                result.add (depth);
    }

    return result;
}

bool canStoreInfo (const juce::File& file)
{
    return file.hasFileExtension ("wav;wave;bwf;aif;aiff;aifc;flac");
}

bool canStoreBroadcast (const juce::File& file)
{
    return file.hasFileExtension ("wav;wave;bwf");
}

bool canStoreMarkers (const juce::File& file)
{
    return file.hasFileExtension ("wav;wave;bwf;aif;aiff;aifc");
}

juce::String save (const AudioSnapshot& snapshot, double sampleRate, const juce::File& target, int bitDepth,
                   juce::AudioFormatManager& formats, const Markers& markers, const FileInfo& info)
{
    auto* format = formatFor (target, formats);

    if (format == nullptr)
        return "Unknown file type: " + target.getFileExtension();

    if (! format->getPossibleBitDepths().contains (bitDepth))
        return format->getFormatName() + " cannot store " + juce::String (bitDepth) + "-bit audio.";

    if (! format->getPossibleSampleRates().isEmpty() && ! format->getPossibleSampleRates().contains ((int) sampleRate))
        return format->getFormatName() + " cannot store " + juce::String (sampleRate) + " Hz audio.";

    juce::TemporaryFile temp (target);

    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (temp.getFile());

        if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen())
            return "Cannot write to " + target.getParentDirectory().getFullPathName();

        const auto isFloat = bitDepth == 32;
        auto options = juce::AudioFormatWriterOptions{}
                           .withSampleRate (sampleRate)
                           .withNumChannels (snapshot.getNumChannels())
                           .withBitsPerSample (bitDepth)
                           .withSampleFormat (isFloat ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                      : juce::AudioFormatWriterOptions::SampleFormat::integral);

        if (canStoreMarkers (target))
        {
            auto metadata = MarkerEdits::toMetadata (markers);

            if (canStoreBroadcast (target))
            {
                const auto broadcast = FileInfoIO::broadcastMetadata (info);

                for (int i = 0; i < broadcast.size(); ++i)
                    metadata[broadcast.getAllKeys()[i]] = broadcast.getAllValues()[i];
            }

            options = options.withMetadataValues (metadata);
        }

        auto writer = format->createWriterFor (stream, options);

        if (writer == nullptr)
            return "Could not create a " + format->getFormatName() + " writer for these settings.";

        // TPDF dither of +-1 LSB for integer output.
        const auto lsb = isFloat ? 0.0f : 1.0f / (float) (1 << (bitDepth - 1));
        juce::Random random;
        juce::AudioBuffer<float> buffer (snapshot.getNumChannels(), framesPerWrite);

        for (juce::int64 pos = 0; pos < snapshot.getNumFrames(); pos += framesPerWrite)
        {
            const auto count = (int) juce::jmin ((juce::int64) framesPerWrite, snapshot.getNumFrames() - pos);
            snapshot.read (buffer, 0, pos, count);

            if (lsb > 0.0f)
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                {
                    auto* d = buffer.getWritePointer (ch);

                    for (int i = 0; i < count; ++i)
                        d[i] += (random.nextFloat() - random.nextFloat()) * lsb;
                }

            if (! writer->writeFromAudioSampleBuffer (buffer, 0, count))
                return "Writing failed (disk full?).";
        }
    }

    // The tags, added to the finished file: a LIST INFO chunk, an ID3 chunk or a Vorbis comment block.
    if (canStoreInfo (target))
    {
        const auto error = canStoreBroadcast (target)                 ? FileInfoIO::appendRiffInfo (temp.getFile(), info)
                         : target.hasFileExtension ("aif;aiff;aifc") ? FileInfoIO::appendAiffTags (temp.getFile(), info)
                                                                     : FileInfoIO::addFlacComments (temp.getFile(), info);

        if (error.isNotEmpty())
            return error;
    }

    if (! temp.overwriteTargetFileWithTemporary())
        return "Could not replace " + target.getFullPathName();

    return {};
}

juce::String saveRange (const AudioSnapshot& snapshot, double sampleRate, juce::int64 start, juce::int64 end, const juce::File& target,
                        int bitDepth, juce::AudioFormatManager& formats, const Markers& markers, const FileInfo& info)
{
    return save (*AudioEdits::crop (snapshot, start, end), sampleRate, target, bitDepth, formats, MarkerEdits::afterCrop (markers, start, end), info);
}

juce::Array<juce::File> filesIn (const juce::String& commandLine)
{
    juce::StringArray tokens;
    tokens.addTokens (commandLine, true);
    juce::Array<juce::File> files;

    for (auto token : tokens)
    {
        token = token.unquoted();

        if (juce::File::isAbsolutePath (token) && juce::File (token).existsAsFile())
            files.addIfNotAlreadyThere (juce::File (token));
    }

    return files;
}
}
