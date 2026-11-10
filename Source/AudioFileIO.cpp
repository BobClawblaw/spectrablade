#include "AudioFileIO.h"
#include "Surround.h"
#include "AudioEdits.h"
#include "Ffmpeg.h"
#include "GaplessMp3.h"
#include "M4aFormat.h"

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

        // Eight channels are 7.1 (Surround): said so, or WAV would call two of them front centre-left and centre-right.
        if (snapshot.getNumChannels() == Surround::sevenOneChannels && format->getFormatName().containsIgnoreCase ("WAV"))
            options = options.withChannelLayout (juce::AudioChannelSet::channelSetWithChannels ({
                juce::AudioChannelSet::left, juce::AudioChannelSet::right, juce::AudioChannelSet::centre, juce::AudioChannelSet::LFE,
                juce::AudioChannelSet::leftSurround, juce::AudioChannelSet::rightSurround,                // back left and right (Lrs Rrs)
                juce::AudioChannelSet::leftSurroundSide, juce::AudioChannelSet::rightSurroundSide }));   // side left and right (Lss Rss)

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

#if JUCE_MAC
namespace
{
    /** Core Audio's reader for an MP3, reading so that every read gives what reading the file from
        its start gives. After a move into a variable-bit-rate MP3, Core Audio decodes the first frames
        wrongly: their bits sit in the frames before the move (the bit reservoir), which it doesn't
        decode first. Measured, the wrong stretch reached 3,456 samples in a V4 file and 9,216 in a
        9 kbps mono one, and none in constant-rate files. So a move goes to primingFrames before the
        sample asked for and what is decoded on the way is dropped; reading on from where the last read
        ended, as playing and loading do, costs nothing extra. */
    class PrimedReader final : public juce::AudioFormatReader
    {
    public:
        static constexpr juce::int64 primingFrames = 32768;

        explicit PrimedReader (std::unique_ptr<juce::AudioFormatReader> r)
            : juce::AudioFormatReader (nullptr, r->getFormatName()), inner (std::move (r))
        {
            sampleRate = inner->sampleRate;
            bitsPerSample = inner->bitsPerSample;
            lengthInSamples = inner->lengthInSamples;
            numChannels = inner->numChannels;
            usesFloatingPointData = inner->usesFloatingPointData;
            metadataValues = inner->metadataValues;
        }

        juce::AudioChannelSet getChannelLayout() override { return inner->getChannelLayout(); }

        bool readSamples (int* const* dest, int numDestChannels, int startOffsetInDestBuffer, juce::int64 start, int count) override
        {
            if (count <= 0)
                return true;

            if (start != next)
            {
                const auto onward = next >= 0 && start >= next && start - next <= primingFrames;
                auto from = onward ? next : juce::jmax<juce::int64> (0, start - primingFrames);
                juce::AudioBuffer<int> dropped ((int) numChannels, 4096);

                while (from < start)
                {
                    const auto n = (int) juce::jmin<juce::int64> (dropped.getNumSamples(), start - from);

                    if (! inner->readSamples (dropped.getArrayOfWritePointers(), (int) numChannels, 0, from, n))
                        return fail();

                    from += n;
                }
            }

            if (! inner->readSamples (dest, numDestChannels, startOffsetInDestBuffer, start, count))
                return fail();

            next = start + count;
            return true;
        }

    private:
        bool fail()
        {
            next = -1;   // where Core Audio is now isn't known: the next read moves afresh
            return false;
        }

        std::unique_ptr<juce::AudioFormatReader> inner;
        juce::int64 next = 0;
    };

    /** Core Audio's formats, with an MP3 read through a PrimedReader. */
    class MacCoreAudioFormat final : public juce::CoreAudioFormat
    {
    public:
        juce::AudioFormatReader* createReaderFor (juce::InputStream* in, bool deleteStreamIfOpeningFails) override
        {
            const auto mp3 = in != nullptr && looksLikeMp3 (*in);
            auto* reader = juce::CoreAudioFormat::createReaderFor (in, deleteStreamIfOpeningFails);
            return mp3 && reader != nullptr ? new PrimedReader (std::unique_ptr<juce::AudioFormatReader> (reader)) : reader;
        }

    private:
        /** Whether the stream starts as an MP3 does: with an ID3v2 tag, or a Layer III frame header. */
        static bool looksLikeMp3 (juce::InputStream& in)
        {
            const auto position = in.getPosition();
            juce::uint8 head[3] {};
            const auto got = in.read (head, 3);
            in.setPosition (position);
            return got == 3 && ((head[0] == 'I' && head[1] == 'D' && head[2] == '3')
                                || (head[0] == 0xff && (head[1] & 0xe0) == 0xe0 && (head[1] & 0x06) == 0x02));
        }
    };
}
#endif

void registerFormats (juce::AudioFormatManager& formats)
{
    // registerBasicFormats' list (a format can only be registered once): on the Mac with Core Audio's
    // MP3 reads primed at each move, elsewhere with MP3 read gaplessly.
    formats.registerFormat (new juce::WavAudioFormat(), true);
    formats.registerFormat (new juce::AiffAudioFormat(), false);
   #if JUCE_USE_FLAC
    formats.registerFormat (new juce::FlacAudioFormat(), false);
   #endif
   #if JUCE_USE_OGGVORBIS
    formats.registerFormat (new juce::OggVorbisAudioFormat(), false);
   #endif
   #if JUCE_USE_OPUS
    formats.registerFormat (new juce::OpusAudioFormat(), false);
   #endif
   #if JUCE_MAC
    formats.registerFormat (new MacCoreAudioFormat(), false);   // MP3, M4A, CAF and the rest; leaves out an MP3's encoder delay and padding itself
   #endif
    formats.registerFormat (new GaplessMp3Format(), false);     // on the Mac, only an MP3 Core Audio won't open
   #if JUCE_WINDOWS
    formats.registerFormat (new M4aFormat(), false);   // Core Audio reads these on the Mac
   #endif
   #if JUCE_USE_WINDOWS_MEDIA_FORMAT
    formats.registerFormat (new juce::WindowsMediaAudioFormat(), false);
   #endif
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
