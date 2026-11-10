#if defined (_WIN32)

// Before JUCE's headers.
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include "MediaFoundation.h"

using Microsoft::WRL::ComPtr;

namespace MediaFoundation
{
namespace
{
    /** COM on this thread for as long as it runs (one already started, of either kind, is used as it is),
        and Media Foundation while any of these objects lives. */
    void startComHere()
    {
        struct Com
        {
            Com() : started (SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED))) {}
            ~Com() { if (started) CoUninitialize(); }
            const bool started;
        };

        thread_local Com com;
    }

    /** Media Foundation is left out of Windows' N editions until the Media Feature Pack is added. Its
        library is loaded only when first used (delay-loaded), and only if it is there, so the program
        starts without it and just goes without what needs it. */
    bool present()
    {
        static const bool found = LoadLibraryExW (L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr;
        return found;
    }

    const char* const missing = "Windows' media features (Media Foundation) aren't installed: on an N edition of Windows, "
                                "add the Media Feature Pack in Settings > Apps > Optional features.";

    struct Platform
    {
        Platform()  { startComHere(); ok = SUCCEEDED (MFStartup (MF_VERSION, MFSTARTUP_LITE)); }
        ~Platform() { if (ok) MFShutdown(); }
        bool ok = false;
    };

    constexpr LONGLONG unitsPerSecond = 10'000'000;   // Media Foundation's 100 ns

    ComPtr<IMFTransform> findTransform (const GUID& category, const GUID& input, const GUID& output)
    {
        MFT_REGISTER_TYPE_INFO in { MFMediaType_Audio, input }, out { MFMediaType_Audio, output };
        IMFActivate** found = nullptr;
        UINT32 count = 0;

        if (FAILED (MFTEnumEx (category, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                               input == GUID_NULL ? nullptr : &in, output == GUID_NULL ? nullptr : &out, &found, &count)))
            return {};

        ComPtr<IMFTransform> transform;

        for (UINT32 i = 0; i < count; ++i)
        {
            if (transform == nullptr)
                found[i]->ActivateObject (IID_PPV_ARGS (&transform));

            found[i]->Release();
        }

        CoTaskMemFree (found);
        return transform;
    }

    ComPtr<IMFMediaType> audioType (const GUID& subtype, double rate, int channels, int bits)
    {
        ComPtr<IMFMediaType> t;

        if (FAILED (MFCreateMediaType (&t)))
            return {};

        const auto r = (UINT32) juce::roundToInt (rate);
        t->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID (MF_MT_SUBTYPE, subtype);
        t->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, r);
        t->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, (UINT32) channels);
        t->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, (UINT32) bits);

        if (subtype == MFAudioFormat_PCM || subtype == MFAudioFormat_Float)
        {
            const auto block = (UINT32) (channels * bits / 8);
            t->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, block);
            t->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, block * r);
            t->SetUINT32 (MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        }

        return t;
    }
}

//==============================================================================
/** A synchronous transform: buffers pushed in, buffers pulled out. */
class Transform
{
public:
    explicit Transform (ComPtr<IMFTransform> t) : mft (std::move (t)) {}

    ~Transform()
    {
        if (mft != nullptr)
            mft->ProcessMessage (MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }

    bool start()
    {
        return SUCCEEDED (mft->ProcessMessage (MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))
            && SUCCEEDED (mft->ProcessMessage (MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    }

    using Out = std::function<bool (const juce::uint8*, size_t)>;

    bool push (const void* data, size_t bytes, LONGLONG time, LONGLONG duration, const Out& out)
    {
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMFSample> sample;
        BYTE* dest = nullptr;

        if (FAILED (MFCreateMemoryBuffer ((DWORD) bytes, &buffer)) || FAILED (buffer->Lock (&dest, nullptr, nullptr)))
            return false;

        std::memcpy (dest, data, bytes);
        buffer->Unlock();
        buffer->SetCurrentLength ((DWORD) bytes);

        if (FAILED (MFCreateSample (&sample)) || FAILED (sample->AddBuffer (buffer.Get())))
            return false;

        sample->SetSampleTime (time);
        sample->SetSampleDuration (duration);

        for (int attempt = 0; attempt < 2; ++attempt)
        {
            const auto hr = mft->ProcessInput (0, sample.Get(), 0);

            if (SUCCEEDED (hr))
                return pull (out);

            if (hr != MF_E_NOTACCEPTING || ! pull (out))
                return false;
        }

        return false;
    }

    /** Everything it has ready, each buffer to out. */
    bool pull (const Out& out)
    {
        for (;;)
        {
            MFT_OUTPUT_STREAM_INFO info {};

            if (FAILED (mft->GetOutputStreamInfo (0, &info)))
                return false;

            MFT_OUTPUT_DATA_BUFFER output {};
            ComPtr<IMFSample> ours;

            if ((info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) == 0)
            {
                ComPtr<IMFMediaBuffer> buffer;

                if (FAILED (MFCreateMemoryBuffer (juce::jmax ((DWORD) (1 << 16), info.cbSize), &buffer))
                    || FAILED (MFCreateSample (&ours)) || FAILED (ours->AddBuffer (buffer.Get())))
                    return false;

                output.pSample = ours.Get();
            }

            DWORD status = 0;
            const auto hr = mft->ProcessOutput (0, 1, &output, &status);

            if (output.pEvents != nullptr)
                output.pEvents->Release();

            // A sample the transform made itself is ours to release.
            ComPtr<IMFSample> got;
            got.Attach (output.pSample != ours.Get() ? output.pSample : nullptr);
            auto* sample = output.pSample;

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
                return true;

            if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
            {
                if (onStreamChange == nullptr || ! onStreamChange())
                    return false;

                continue;
            }

            if (FAILED (hr) || sample == nullptr)
                return false;

            ComPtr<IMFMediaBuffer> contiguous;
            BYTE* data = nullptr;
            DWORD length = 0;

            if (FAILED (sample->ConvertToContiguousBuffer (&contiguous)) || FAILED (contiguous->Lock (&data, nullptr, &length)))
                return false;

            const auto ok = length == 0 || out (data, length);
            contiguous->Unlock();

            if (! ok)
                return false;
        }
    }

    bool drain (const Out& out)
    {
        return SUCCEEDED (mft->ProcessMessage (MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0))
            && SUCCEEDED (mft->ProcessMessage (MFT_MESSAGE_COMMAND_DRAIN, 0))
            && pull (out);
    }

    void flush()
    {
        mft->ProcessMessage (MFT_MESSAGE_COMMAND_FLUSH, 0);
    }

    ComPtr<IMFTransform> mft;
    std::function<bool()> onStreamChange;
    Platform platform;
};

//==============================================================================
AacEncoder::AacEncoder() = default;
AacEncoder::~AacEncoder() = default;

std::unique_ptr<AacEncoder> AacEncoder::create (double sampleRate, int channels, int kbps, juce::String& error)
{
    startComHere();

    if (! present())
    {
        error = missing;
        return {};
    }

    std::unique_ptr<AacEncoder> e (new AacEncoder());
    auto mft = findTransform (MFT_CATEGORY_AUDIO_ENCODER, MFAudioFormat_PCM, MFAudioFormat_AAC);

    if (mft == nullptr)
    {
        error = "Windows' AAC encoder isn't available.";
        return {};
    }

    e->transform = std::make_unique<Transform> (std::move (mft));
    auto& t = *e->transform->mft.Get();

    if (FAILED (t.SetInputType (0, audioType (MFAudioFormat_PCM, sampleRate, channels, 16).Get(), 0)))
    {
        error = "Windows' AAC encoder doesn't take " + juce::String (sampleRate / 1000.0, 1) + " kHz audio in "
                + juce::String (channels) + (channels == 1 ? " channel." : " channels.");
        return {};
    }

    // The highest rate it offers at or below the one asked for, for this rate and channel count.
    ComPtr<IMFMediaType> best;
    UINT32 bestRate = 0;

    for (DWORD i = 0;; ++i)
    {
        ComPtr<IMFMediaType> offered;

        if (FAILED (t.GetOutputAvailableType (0, i, &offered)))
            break;

        GUID subtype {};
        UINT32 rate = 0, chans = 0, bytesPerSecond = 0;
        offered->GetGUID (MF_MT_SUBTYPE, &subtype);
        offered->GetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        offered->GetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, &chans);
        offered->GetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bytesPerSecond);
        const auto payload = MFGetAttributeUINT32 (offered.Get(), MF_MT_AAC_PAYLOAD_TYPE, 0);

        if (subtype == MFAudioFormat_AAC && rate == (UINT32) juce::roundToInt (sampleRate) && chans == (UINT32) channels && payload == 0
            && bytesPerSecond * 8 <= (UINT32) kbps * 1000 && bytesPerSecond > bestRate)
        {
            best = offered;
            bestRate = bytesPerSecond;
        }
    }

    if (best == nullptr || FAILED (t.SetOutputType (0, best.Get(), 0)))
    {
        error = "Windows' AAC encoder offers no bit rate up to " + juce::String (kbps) + " kbps for this audio.";
        return {};
    }

    // MF_MT_USER_DATA: the rest of an HEAACWAVEINFO after its WAVEFORMATEX (12 bytes), then the AudioSpecificConfig.
    UINT8 blob[64] {};
    UINT32 blobSize = 0;

    if (FAILED (best->GetBlob (MF_MT_USER_DATA, blob, sizeof (blob), &blobSize)) || blobSize <= 12)
    {
        error = "Windows' AAC encoder gave no configuration.";
        return {};
    }

    e->audioSpecificConfig = juce::MemoryBlock (blob + 12, blobSize - 12);
    e->bitRate = (int) (bestRate * 8 / 1000);
    e->channels = channels;
    e->sampleRate = sampleRate;

    if (! e->transform->start())
    {
        error = "Windows' AAC encoder didn't start.";
        return {};
    }

    return e;
}

bool AacEncoder::encode (const juce::int16* interleaved, int frames, const PacketOut& out)
{
    startComHere();
    const auto time = (LONGLONG) ((double) framesIn * unitsPerSecond / sampleRate);
    const auto duration = (LONGLONG) ((double) frames * unitsPerSecond / sampleRate);
    framesIn += frames;
    return transform->push (interleaved, (size_t) frames * (size_t) channels * sizeof (juce::int16), time, duration,
                            [&out] (const juce::uint8* d, size_t n) { return out (d, n); });
}

bool AacEncoder::finish (const PacketOut& out)
{
    startComHere();
    return transform->drain ([&out] (const juce::uint8* d, size_t n) { return out (d, n); });
}

//==============================================================================
Resampler::Resampler() = default;
Resampler::~Resampler() = default;

std::unique_ptr<Resampler> Resampler::create (double inRate, double outRate, int channels, juce::String& error)
{
    startComHere();

    if (! present())
    {
        error = missing;
        return {};
    }

    std::unique_ptr<Resampler> r (new Resampler());
    ComPtr<IMFTransform> mft;

    if (FAILED (CoCreateInstance (CLSID_CResamplerMediaObject, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS (&mft))))
    {
        error = "Windows' resampler isn't available.";
        return {};
    }

    ComPtr<IWMResamplerProps> props;

    if (SUCCEEDED (mft.As (&props)))
        props->SetHalfFilterLength (60);   // its best quality

    r->transform = std::make_unique<Transform> (std::move (mft));
    auto& t = *r->transform->mft.Get();

    if (FAILED (t.SetInputType (0, audioType (MFAudioFormat_Float, inRate, channels, 32).Get(), 0))
        || FAILED (t.SetOutputType (0, audioType (MFAudioFormat_Float, outRate, channels, 32).Get(), 0))
        || ! r->transform->start())
    {
        error = "Windows' resampler can't convert " + juce::String (inRate) + " Hz to " + juce::String (outRate) + " Hz.";
        return {};
    }

    r->channels = channels;
    r->inRate = inRate;
    return r;
}

bool Resampler::process (const float* interleaved, int frames, std::vector<float>& out)
{
    startComHere();
    const auto time = (LONGLONG) ((double) framesIn * unitsPerSecond / inRate);
    framesIn += frames;
    const auto append = [&out] (const juce::uint8* d, size_t n)
    {
        out.insert (out.end(), (const float*) d, (const float*) d + n / sizeof (float));
        return true;
    };
    return transform->push (interleaved, (size_t) frames * (size_t) channels * sizeof (float), time,
                            (LONGLONG) ((double) frames * unitsPerSecond / inRate), append);
}

bool Resampler::finish (std::vector<float>& out)
{
    startComHere();
    return transform->drain ([&out] (const juce::uint8* d, size_t n)
    {
        out.insert (out.end(), (const float*) d, (const float*) d + n / sizeof (float));
        return true;
    });
}

//==============================================================================
Decoder::Decoder() = default;
Decoder::~Decoder() = default;

std::unique_ptr<Decoder> Decoder::create (const juce::String& codec, const juce::MemoryBlock& config, double sampleRate,
                                          int channels, int bitsPerSample, juce::String& error)
{
    startComHere();

    if (! present())
    {
        error = missing;
        return {};
    }

    const auto aac = codec == "aac", mp3 = codec == "mp3";
    const auto subtype = aac ? MFAudioFormat_AAC : mp3 ? MFAudioFormat_MP3 : MFAudioFormat_ALAC;
    const juce::String name (aac ? "AAC" : mp3 ? "MP3" : "Apple Lossless");
    auto mft = findTransform (MFT_CATEGORY_AUDIO_DECODER, subtype, GUID_NULL);

    if (mft == nullptr)
    {
        error = "Windows' " + name + " decoder isn't available.";
        return {};
    }

    std::unique_ptr<Decoder> d (new Decoder());
    d->transform = std::make_unique<Transform> (std::move (mft));
    auto& t = *d->transform->mft.Get();

    // The ways each decoder is told its stream: AAC's HEAACWAVEINFO tail and AudioSpecificConfig; MP3's
    // MPEGLAYER3WAVEFORMAT tail, or nothing; ALAC's configuration, bare or in its box, as different
    // writers give it.
    std::vector<juce::MemoryBlock> userData;

    if (aac)
    {
        juce::MemoryBlock b (12, true);
        b.append (config.getData(), config.getSize());
        userData.push_back (b);
    }
    else if (mp3)
    {
        userData.push_back (config);
        userData.push_back ({});
    }
    else
    {
        userData.push_back (config);
        juce::MemoryBlock boxed;
        const juce::uint8 header[] { 0, 0, 0, (juce::uint8) (config.getSize() + 12), 'a', 'l', 'a', 'c', 0, 0, 0, 0 };
        boxed.append (header, sizeof (header));
        boxed.append (config.getData(), config.getSize());
        userData.push_back (boxed);
    }

    auto accepted = false;

    for (const auto& data : userData)
    {
        auto in = audioType (subtype, sampleRate, channels, aac || mp3 ? 16 : bitsPerSample);

        if (aac)
            in->SetUINT32 (MF_MT_AAC_PAYLOAD_TYPE, 0);

        if (! data.isEmpty())
            in->SetBlob (MF_MT_USER_DATA, (const UINT8*) data.getData(), (UINT32) data.getSize());

        if (SUCCEEDED (t.SetInputType (0, in.Get(), 0)))
        {
            accepted = true;
            break;
        }
    }

    if (! accepted)
    {
        error = "Windows' decoder doesn't take this " + name + " audio.";
        return {};
    }

    // Float out if it offers it, else the widest PCM.
    auto* decoder = d.get();
    const auto chooseOutput = [decoder, &t]
    {
        ComPtr<IMFMediaType> chosen;
        auto chosenFloat = false;
        UINT32 chosenBits = 0;

        for (DWORD i = 0;; ++i)
        {
            ComPtr<IMFMediaType> offered;

            if (FAILED (t.GetOutputAvailableType (0, i, &offered)))
                break;

            GUID subtypeOut {};
            UINT32 bits = 0;
            offered->GetGUID (MF_MT_SUBTYPE, &subtypeOut);
            offered->GetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
            const auto isFloat = subtypeOut == MFAudioFormat_Float;

            if ((isFloat || subtypeOut == MFAudioFormat_PCM) && (chosen == nullptr || (isFloat && ! chosenFloat) || (isFloat == chosenFloat && bits > chosenBits)))
            {
                chosen = offered;
                chosenFloat = isFloat;
                chosenBits = bits;
            }
        }

        if (chosen == nullptr || FAILED (t.SetOutputType (0, chosen.Get(), 0)))
            return false;

        UINT32 chans = 0, rate = 0;
        chosen->GetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, &chans);
        chosen->GetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        decoder->channels = (int) chans;
        decoder->outputBits = (int) chosenBits;
        decoder->outputFloat = chosenFloat;
        decoder->sampleRate = rate;
        return chans > 0 && (chosenFloat ? chosenBits == 32 : (chosenBits == 16 || chosenBits == 24 || chosenBits == 32));
    };

    if (! chooseOutput() || ! d->transform->start())
    {
        error = "Windows' decoder gives no audio this can read.";
        return {};
    }

    d->transform->onStreamChange = chooseOutput;
    return d;
}

bool Decoder::convert (const juce::uint8* data, size_t size, std::vector<float>& out) const
{
    if (outputFloat)
    {
        out.insert (out.end(), (const float*) data, (const float*) data + size / sizeof (float));
        return true;
    }

    const auto bytes = (size_t) outputBits / 8;
    const auto count = size / bytes;
    out.reserve (out.size() + count);

    for (size_t i = 0; i < count; ++i)
    {
        const auto* s = data + i * bytes;

        if (outputBits == 16)
            out.push_back ((float) (juce::int16) juce::ByteOrder::littleEndianShort (s) / 32768.0f);
        else if (outputBits == 24)
            out.push_back ((float) juce::ByteOrder::littleEndian24Bit (s) / 8388608.0f);
        else
            out.push_back ((float) ((double) (juce::int32) juce::ByteOrder::littleEndianInt (s) / 2147483648.0));
    }

    return true;
}

bool Decoder::decode (const void* data, size_t size, std::vector<float>& out)
{
    startComHere();
    const auto stamp = time++;
    return transform->push (data, size, stamp, 0, [this, &out] (const juce::uint8* d, size_t n) { return convert (d, n, out); });
}

void Decoder::reset()
{
    startComHere();
    transform->flush();
}
}

#endif
