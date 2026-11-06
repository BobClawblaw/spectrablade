#include "AacWriter.h"

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
