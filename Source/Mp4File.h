#pragma once

#include <JuceHeader.h>

/** The audio track of an MPEG-4 file (.m4a): read into the packets a decoder takes, and written for
    AAC in the layout SpectraBlade's own files have. Only the boxes audio needs; no video, no
    fragments. Used where the system has no MPEG-4 audio file support of its own (Windows). */
namespace Mp4File
{
    struct Packet
    {
        juce::int64 offset = 0;     // in the file
        juce::uint32 size = 0;
        juce::int64 start = 0;      // in frames, from the first packet's
        juce::uint32 frames = 0;
    };

    struct AudioTrack
    {
        juce::String codec;         // "aac" or "alac"
        juce::MemoryBlock config;   // AAC's AudioSpecificConfig, or ALAC's 24-byte specific config
        double sampleRate = 0.0;
        int channels = 0;
        int bitsPerSample = 16;     // ALAC's; 16 for AAC, which decodes to float anyway
        std::vector<Packet> packets;
        juce::int64 decodedFrames = 0;   // all the packets give
        juce::int64 skip = 0;            // the encoder's delay, from the edit list
        juce::int64 length = 0;          // the frames meant to be heard, after skip
    };

    /** The first sound track in the stream: its codec, configuration and packets, and the part of it
        meant to be heard. Nothing if there is none, or it isn't AAC or ALAC. */
    std::optional<AudioTrack> readAudioTrack (juce::InputStream&);

    struct AacTrack
    {
        double sampleRate = 0.0;
        int channels = 0;
        juce::MemoryBlock audioSpecificConfig;
        juce::uint32 averageBitRate = 0, maxBitRate = 0;
        std::vector<juce::uint32> packetSizes;   // 1,024 frames each
        juce::int64 priming = 0;                 // frames at the start that aren't the audio
        juce::int64 frames = 0;                  // the audio's frames
    };

    /** Writes an .m4a: ftyp, moov (with an edit list leaving out the priming and the padding at the
        end), a free box of freeBytes for tags added later, and mdat with the packets, read in order
        from payload. Returns an error message, or empty on success. */
    juce::String writeAac (juce::OutputStream&, const AacTrack&, juce::InputStream& payload, juce::int64 freeBytes);
}
