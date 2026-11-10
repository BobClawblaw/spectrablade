#pragma once

#include <JuceHeader.h>

#if JUCE_WINDOWS

/** Windows' own audio codecs, through Media Foundation: the AAC encoder for .m4a export, the AAC and
    ALAC decoders for reading .m4a files, and the resampler the encoder's rates need. Each object
    starts COM and Media Foundation for itself, and is used from one thread at a time. */
namespace MediaFoundation
{
    class Transform;

    /** AAC-LC, raw packets of 1,024 frames each, from interleaved 16-bit audio. */
    class AacEncoder
    {
    public:
        /** At 44.1 or 48 kHz (or another rate it offers), one or two channels, and the highest bit rate
            it offers at or below kbps (it offers 96 to 320 kbps on Windows 11). Null, with error set,
            when it can't. */
        static std::unique_ptr<AacEncoder> create (double sampleRate, int channels, int kbps, juce::String& error);
        ~AacEncoder();

        using PacketOut = std::function<bool (const void* data, size_t size)>;

        /** Encodes frames of interleaved audio; each packet done goes to out. False on failure. */
        bool encode (const juce::int16* interleaved, int frames, const PacketOut& out);

        /** The packets still held, to the end. */
        bool finish (const PacketOut& out);

        const juce::MemoryBlock& getAudioSpecificConfig() const noexcept   { return audioSpecificConfig; }
        int getBitRate() const noexcept                                     { return bitRate; }

    private:
        AacEncoder();
        std::unique_ptr<Transform> transform;
        juce::MemoryBlock audioSpecificConfig;
        int bitRate = 0, channels = 0;
        double sampleRate = 0.0;
        juce::int64 framesIn = 0;
    };

    /** Interleaved float audio from one rate to another, as it streams. */
    class Resampler
    {
    public:
        static std::unique_ptr<Resampler> create (double inRate, double outRate, int channels, juce::String& error);
        ~Resampler();

        /** Converts frames of interleaved audio, adding what comes out to out. */
        bool process (const float* interleaved, int frames, std::vector<float>& out);

        /** What it still holds, to the end. */
        bool finish (std::vector<float>& out);

    private:
        Resampler();
        std::unique_ptr<Transform> transform;
        int channels = 0;
        double inRate = 0.0;
        juce::int64 framesIn = 0;
    };

    /** AAC or ALAC packets from an MPEG-4 file, or MP3 frames, to interleaved float audio. */
    class Decoder
    {
    public:
        /** codec is "aac" (config: the AudioSpecificConfig), "mp3" (config: an MPEGLAYER3WAVEFORMAT's last
            12 bytes) or "alac" (config: its 24-byte specific config). */
        static std::unique_ptr<Decoder> create (const juce::String& codec, const juce::MemoryBlock& config, double sampleRate,
                                                int channels, int bitsPerSample, juce::String& error);
        ~Decoder();

        /** Decodes one packet, adding the frames it gives to out (interleaved, getChannels() wide). */
        bool decode (const void* data, size_t size, std::vector<float>& out);

        /** Forgets what it holds, before decoding from somewhere else. */
        void reset();

        int getChannels() const noexcept        { return channels; }
        double getSampleRate() const noexcept   { return sampleRate; }   // as decoded (HE-AAC doubles the stored rate)

    private:
        Decoder();
        bool convert (const juce::uint8* data, size_t size, std::vector<float>& out) const;

        std::unique_ptr<Transform> transform;
        int channels = 0, outputBits = 32;
        bool outputFloat = true;
        double sampleRate = 0.0;
        juce::int64 time = 0;
    };
}

#endif
