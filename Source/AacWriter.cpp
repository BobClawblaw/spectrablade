#include "AacWriter.h"

#if defined (__APPLE__)

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace AacWriter
{
std::string write (const char* path, double inputRate, double outputRate, unsigned channels, int kbps, double seconds, const Pull& pull)
{
    constexpr int framesPerWrite = 1 << 14;
    const auto url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) path, (CFIndex) std::strlen (path), false);

    if (url == nullptr)
        return std::string ("Cannot write to ") + path;

    AudioStreamBasicDescription stored {};
    stored.mFormatID = kAudioFormatMPEG4AAC;
    stored.mSampleRate = outputRate;
    stored.mChannelsPerFrame = channels;
    stored.mFramesPerPacket = 1024;

    ExtAudioFileRef ext = nullptr;
    const auto created = ExtAudioFileCreateWithURL (url, kAudioFileM4AType, &stored, nullptr, kAudioFileFlags_EraseFile, &ext);
    CFRelease (url);

    if (created != noErr || ext == nullptr)
        return "The AAC encoder could not start (error " + std::to_string ((int) created) + ").";

    const std::unique_ptr<OpaqueExtAudioFile, decltype (&ExtAudioFileDispose)> owner (ext, &ExtAudioFileDispose);

    // Room for the packet index before the audio, sized to the audio's length: by default the file keeps
    // about 55 KB free there.
    AudioFileID audioFile = nullptr;
    UInt32 idSize = sizeof (audioFile);

    if (ExtAudioFileGetProperty (ext, kExtAudioFileProperty_AudioFile, &idSize, &audioFile) == noErr && audioFile != nullptr)
    {
        Float64 reserve = seconds;
        AudioFileSetProperty (audioFile, kAudioFilePropertyReserveDuration, sizeof (reserve), &reserve);
    }

    AudioStreamBasicDescription given {};
    given.mFormatID = kAudioFormatLinearPCM;
    given.mFormatFlags = (AudioFormatFlags) kAudioFormatFlagsNativeFloatPacked | (AudioFormatFlags) kAudioFormatFlagIsNonInterleaved;
    given.mSampleRate = inputRate;
    given.mChannelsPerFrame = channels;
    given.mBitsPerChannel = 32;
    given.mBytesPerFrame = 4;
    given.mBytesPerPacket = 4;
    given.mFramesPerPacket = 1;

    if (ExtAudioFileSetProperty (ext, kExtAudioFileProperty_ClientDataFormat, sizeof (given), &given) != noErr)
        return "The AAC encoder can't take this audio.";

    AudioConverterRef converter = nullptr;
    UInt32 size = sizeof (converter);

    if (ExtAudioFileGetProperty (ext, kExtAudioFileProperty_AudioConverter, &size, &converter) != noErr || converter == nullptr)
        return "The AAC encoder could not start.";

    // The encoder takes only some bit rates for each sample rate and channel count: use the nearest one
    // at or below the rate asked for, or its lowest.
    UInt32 rangesSize = 0;
    Boolean writable = false;

    if (AudioConverterGetPropertyInfo (converter, kAudioConverterApplicableEncodeBitRates, &rangesSize, &writable) != noErr
        || rangesSize < sizeof (AudioValueRange))
        return "The AAC encoder gave no bit rates.";

    std::vector<AudioValueRange> ranges (rangesSize / sizeof (AudioValueRange));

    if (AudioConverterGetProperty (converter, kAudioConverterApplicableEncodeBitRates, &rangesSize, ranges.data()) != noErr)
        return "The AAC encoder gave no bit rates.";

    const auto wanted = kbps * 1000.0;
    auto chosen = 0.0;
    auto lowest = std::numeric_limits<double>::max();

    for (const auto& r : ranges)
    {
        lowest = std::min (lowest, r.mMinimum);

        if (r.mMinimum <= wanted)
            chosen = std::max (chosen, std::min (r.mMaximum, wanted));
    }

    UInt32 bitRate = (UInt32) (chosen > 0.0 ? chosen : lowest);
    UInt32 quality = kAudioConverterQuality_Max;
    AudioConverterSetProperty (converter, kAudioConverterCodecQuality, sizeof (quality), &quality);

    if (AudioConverterSetProperty (converter, kAudioConverterEncodeBitRate, sizeof (bitRate), &bitRate) != noErr)
        return "The AAC encoder refused " + std::to_string (bitRate / 1000) + " kbps.";

    // Tells the file to take up the converter's new settings.
    CFArrayRef config = nullptr;
    ExtAudioFileSetProperty (ext, kExtAudioFileProperty_ConverterConfig, sizeof (config), &config);

    std::vector<std::vector<float>> blocks (channels, std::vector<float> (framesPerWrite));
    std::vector<float*> pointers;

    for (auto& b : blocks)
        pointers.push_back (b.data());

    std::vector<std::byte> listStorage (offsetof (AudioBufferList, mBuffers) + channels * sizeof (AudioBuffer));
    auto* list = reinterpret_cast<AudioBufferList*> (listStorage.data());
    list->mNumberBuffers = channels;

    for (;;)
    {
        const auto count = pull (pointers.data(), framesPerWrite);

        if (count < 0)
            return "Stopped.";

        if (count == 0)
            break;

        for (unsigned ch = 0; ch < channels; ++ch)
            list->mBuffers[ch] = { 1, (UInt32) count * 4, pointers[ch] };

        if (ExtAudioFileWrite (ext, (UInt32) count, list) != noErr)
            return "Writing failed (disk full?).";
    }

    return {};
}
}

#elif defined (_WIN32)

#include "MediaFoundation.h"
#include "Mp4File.h"

#include <map>
#include <mutex>
#include <tuple>

namespace AacWriter
{
namespace
{
    /** The frames Windows' AAC encoder puts in front of the audio, which the file's edit list leaves
        out. It isn't documented and changes with the rate (1,200 at 48 kHz, 1,303 at 44.1 kHz on
        Windows 11), so it is measured, once for each format: a burst of noise encoded, decoded with
        Windows' own decoder, and found again where it correlates best. */
    juce::int64 encoderPriming (double rate, int channels, int kbps)
    {
        static std::mutex mutex;
        static std::map<std::tuple<int, int, int>, juce::int64> known;
        const auto key = std::make_tuple (juce::roundToInt (rate), channels, kbps);

        {
            const std::lock_guard<std::mutex> lock (mutex);

            if (const auto found = known.find (key); found != known.end())
                return found->second;
        }

        constexpr int noiseFrames = 16384, tailFrames = 8192, maxDelay = 4096, span = 8192;
        juce::String error;
        auto encoder = MediaFoundation::AacEncoder::create (rate, channels, kbps, error);

        if (encoder == nullptr)
            return 0;

        juce::Random random (0x5b1ade);
        std::vector<float> noise ((size_t) noiseFrames);
        std::vector<juce::int16> pcm ((size_t) (noiseFrames + tailFrames) * (size_t) channels, 0);

        for (int i = 0; i < noiseFrames; ++i)
        {
            noise[(size_t) i] = (random.nextFloat() - 0.5f) * 0.5f;

            for (int ch = 0; ch < channels; ++ch)
                pcm[(size_t) i * (size_t) channels + (size_t) ch] = (juce::int16) juce::roundToInt (noise[(size_t) i] * 32767.0f);
        }

        std::vector<juce::MemoryBlock> packets;
        const auto keep = [&packets] (const void* d, size_t n) { packets.emplace_back (d, n); return true; };
        auto decoder = encoder->encode (pcm.data(), noiseFrames + tailFrames, keep) && encoder->finish (keep)
                         ? MediaFoundation::Decoder::create ("aac", encoder->getAudioSpecificConfig(), rate, channels, 16, error)
                         : nullptr;
        std::vector<float> decoded;

        if (decoder == nullptr)
            return 0;

        for (const auto& p : packets)
            if (! decoder->decode (p.getData(), p.getSize(), decoded))
                return 0;

        const auto width = (size_t) decoder->getChannels();
        auto best = (juce::int64) 0;
        auto bestSum = -1.0;

        for (int lag = 0; lag <= maxDelay; ++lag)
        {
            if ((size_t) (span + lag + span) * width > decoded.size())
                break;

            double sum = 0.0;

            for (int i = span / 2; i < span / 2 + span; ++i)
                sum += (double) noise[(size_t) i] * decoded[(size_t) (i + lag) * width];

            if (sum > bestSum)
            {
                bestSum = sum;
                best = lag;
            }
        }

        const std::lock_guard<std::mutex> lock (mutex);
        known[key] = best;
        return best;
    }

    /** Room kept for tags in the free box, as the Mac's encoder keeps it: about 197 bytes for each
        second asked for, less what the packets' sizes already take. */
    juce::int64 freeRoom (double seconds, size_t packets)
    {
        return 8 + juce::jmax ((juce::int64) 0, (juce::int64) std::ceil (197.0 * seconds) - (juce::int64) packets * 4);
    }
}

std::string write (const char* path, double inputRate, double outputRate, unsigned channels, int kbps, double seconds, const Pull& pull)
{
    constexpr int framesPerPull = 1 << 14;
    const auto numChannels = (int) channels;
    const juce::File target (juce::String::fromUTF8 (path));
    juce::String error;

    auto encoder = MediaFoundation::AacEncoder::create (outputRate, numChannels, kbps, error);

    if (encoder == nullptr)
        return error.toStdString();

    const auto priming = encoderPriming (outputRate, numChannels, kbps);

    std::unique_ptr<MediaFoundation::Resampler> resampler;

    if (! juce::exactlyEqual (inputRate, outputRate))
        if (resampler = MediaFoundation::Resampler::create (inputRate, outputRate, numChannels, error); resampler == nullptr)
            return error.toStdString();

    // The packets go to a file of their own while encoding, as their sizes go at the front of the file.
    juce::TemporaryFile packetFile (target);
    std::vector<juce::uint32> sizes;
    juce::int64 framesIn = 0, framesOut = 0;

    {
        juce::FileOutputStream packets (packetFile.getFile());

        if (packets.failedToOpen())
            return ("Cannot write to " + target.getParentDirectory().getFullPathName()).toStdString();

        const auto keep = [&] (const void* data, size_t size)
        {
            sizes.push_back ((juce::uint32) size);
            return packets.write (data, size);
        };

        std::vector<juce::int16> pcm;
        const auto encode = [&] (const float* interleaved, size_t count)
        {
            const auto frames = (int) (count / (size_t) numChannels);
            pcm.resize (count);

            for (size_t i = 0; i < count; ++i)
                pcm[i] = (juce::int16) juce::jlimit (-32768, 32767, juce::roundToInt (interleaved[i] * 32768.0f));

            framesOut += frames;
            return frames == 0 || encoder->encode (pcm.data(), frames, keep);
        };

        juce::AudioBuffer<float> block (numChannels, framesPerPull);
        std::vector<float> interleaved, resampled;

        for (;;)
        {
            const auto n = pull (block.getArrayOfWritePointers(), framesPerPull);

            if (n < 0)
                return "Stopped.";

            if (n == 0)
                break;

            interleaved.resize ((size_t) n * channels);

            for (int i = 0; i < n; ++i)
                for (int ch = 0; ch < numChannels; ++ch)
                    interleaved[(size_t) i * channels + (size_t) ch] = block.getSample (ch, i);

            framesIn += n;
            auto ok = true;

            if (resampler != nullptr)
            {
                resampled.clear();
                ok = resampler->process (interleaved.data(), n, resampled) && encode (resampled.data(), resampled.size());
            }
            else
            {
                ok = encode (interleaved.data(), interleaved.size());
            }

            if (! ok)
                return packets.getStatus().wasOk() ? "Encoding failed." : "Writing failed (disk full?).";
        }

        if (resampler != nullptr)
        {
            resampled.clear();

            if (! resampler->finish (resampled) || ! encode (resampled.data(), resampled.size()))
                return "Encoding failed.";
        }

        // Silence after the audio: the encoder keeps back its last frames, and lets them go only when
        // more comes, so the end of the audio reaches the file.
        const auto audioFrames = framesOut;
        const std::vector<float> silence ((size_t) (priming + 3072) * channels, 0.0f);

        if (! encode (silence.data(), silence.size()) || ! encoder->finish (keep))
            return "Encoding failed.";

        framesOut = audioFrames;

        packets.flush();

        if (! packets.getStatus().wasOk())
            return "Writing failed (disk full?).";
    }

    // The audio's length at the rate written, within what the packets hold after the priming.
    Mp4File::AacTrack track;
    track.sampleRate = outputRate;
    track.channels = numChannels;
    track.audioSpecificConfig = encoder->getAudioSpecificConfig();
    track.packetSizes = std::move (sizes);
    track.priming = priming;
    const auto wanted = resampler != nullptr ? (juce::int64) std::llround ((double) framesIn * outputRate / inputRate) : framesIn;
    track.frames = juce::jlimit ((juce::int64) 0, juce::jmax ((juce::int64) 0, (juce::int64) track.packetSizes.size() * 1024 - track.priming),
                                 juce::jmin (wanted, framesOut));

    juce::uint64 bytes = 0;

    for (auto s : track.packetSizes)
        bytes += s;

    const auto audioSeconds = juce::jmax (1.0e-3, (double) track.packetSizes.size() * 1024.0 / outputRate);
    track.averageBitRate = (juce::uint32) ((double) bytes * 8.0 / audioSeconds);
    track.maxBitRate = (juce::uint32) juce::jmax ((juce::uint64) track.averageBitRate, (juce::uint64) encoder->getBitRate() * 1000);

    juce::FileInputStream payload (packetFile.getFile());
    juce::FileOutputStream out (target);

    if (payload.failedToOpen() || out.failedToOpen() || ! out.setPosition (0) || ! out.truncate().wasOk())
        return ("Cannot write to " + target.getFullPathName()).toStdString();

    if (const auto written = Mp4File::writeAac (out, track, payload, freeRoom (seconds, track.packetSizes.size())); written.isNotEmpty())
        return written.toStdString();

    return out.getStatus().wasOk() ? std::string() : std::string ("Writing failed (disk full?).");
}
}

#else

namespace AacWriter
{
std::string write (const char*, double, double, unsigned, int, double, const Pull&)
{
    return "AAC export is not available on this system.";
}
}

#endif
