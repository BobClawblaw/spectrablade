#include "LossyFiles.h"

#include "AacWriter.h"

#include <lame.h>

namespace LossyFiles
{
namespace
{
    constexpr int framesPerWrite = 1 << 14;
    const std::array<double, 9> heldRates { 8000.0, 11025.0, 12000.0, 16000.0, 22050.0, 24000.0, 32000.0, 44100.0, 48000.0 };

    /** Reads the snapshot a block at a time, reporting progress; false means stopped. */
    template <typename Consume>
    bool forEachBlock (const AudioSnapshot& snapshot, const std::function<bool (float)>& progress, Consume&& consume)
    {
        juce::AudioBuffer<float> buffer (snapshot.getNumChannels(), framesPerWrite);
        const auto total = snapshot.getNumFrames();

        for (juce::int64 pos = 0; pos < total; pos += framesPerWrite)
        {
            if (progress != nullptr && ! progress ((float) ((double) pos / (double) total)))
                return false;

            const auto count = (int) juce::jmin ((juce::int64) framesPerWrite, total - pos);
            snapshot.read (buffer, 0, pos, count);

            if (! consume (buffer, count))
                return false;
        }

        return true;
    }

    void configureMp3 (lame_global_flags* g, int channels, double sampleRate, const Settings& settings)
    {
        lame_set_num_channels (g, channels);
        lame_set_in_samplerate (g, juce::roundToInt (sampleRate));
        lame_set_out_samplerate (g, juce::roundToInt (outputRate (sampleRate)));
        lame_set_mode (g, channels == 1 ? MONO : JOINT_STEREO);
        lame_set_quality (g, 2);
        lame_set_write_id3tag_automatic (g, 0);
        lame_set_bWriteVbrTag (g, 1);

        if (settings.variable)
        {
            lame_set_VBR (g, vbr_default);
            lame_set_VBR_quality (g, (float) settings.vbrQuality);
        }
        else
        {
            lame_set_VBR (g, vbr_off);
            lame_set_brate (g, settings.kbps);
        }
    }

    juce::String writeMp3 (const AudioSnapshot& snapshot, double sampleRate, const juce::File& file, const Settings& settings,
                           const std::function<bool (float)>& progress, const FileInfo& info)
    {
        const auto channels = snapshot.getNumChannels();
        std::unique_ptr<lame_global_flags, decltype (&lame_close)> lame (lame_init(), &lame_close);

        if (lame == nullptr)
            return "The MP3 encoder could not start.";

        // At constant bit rates up to 224 kbps, LAME scales the audio down by 2 to 5% against clipping. A
        // trial encoder finds that scale, and the real one undoes it, so the file keeps the audio's level.
        std::unique_ptr<lame_global_flags, decltype (&lame_close)> trial (lame_init(), &lame_close);

        if (trial == nullptr)
            return "The MP3 encoder could not start.";

        configureMp3 (trial.get(), channels, sampleRate, settings);

        if (lame_init_params (trial.get()) < 0 || lame_get_scale (trial.get()) <= 0.0f)
            return "The MP3 encoder refused these settings.";

        auto* g = lame.get();
        configureMp3 (g, channels, sampleRate, settings);
        lame_set_scale (g, 1.0f / lame_get_scale (trial.get()));

        if (lame_init_params (g) < 0)
            return "The MP3 encoder refused these settings.";

        juce::FileOutputStream out (file);

        if (out.failedToOpen() || ! out.truncate().wasOk())
            return "Cannot write to " + file.getParentDirectory().getFullPathName();

        // The tags first; LAME's tag frame comes straight after them.
        const auto id3 = FileInfoIO::id3v2 (info);

        if (! out.write (id3.getData(), id3.getSize()))
            return "Writing failed (disk full?).";

        std::vector<unsigned char> encoded ((size_t) (framesPerWrite * 5 / 4 + 7200));
        auto failed = false;

        const auto finished = forEachBlock (snapshot, progress, [&] (const juce::AudioBuffer<float>& buffer, int count)
        {
            const auto* left = buffer.getReadPointer (0);
            const auto* right = buffer.getReadPointer (channels > 1 ? 1 : 0);
            const auto bytes = lame_encode_buffer_ieee_float (g, left, right, count, encoded.data(), (int) encoded.size());
            failed = bytes < 0 || ! out.write (encoded.data(), (size_t) bytes);
            return ! failed;
        });

        if (! finished)
            return failed ? "Writing failed (disk full?)." : "Stopped.";

        const auto bytes = lame_encode_flush (g, encoded.data(), (int) encoded.size());

        if (bytes < 0 || ! out.write (encoded.data(), (size_t) bytes))
            return "Writing failed (disk full?).";

        // The first frame was kept free for the tag that tells players the encoder's delay and padding,
        // so they can play the audio from its first sample to its last.
        std::array<unsigned char, 2880> lameTag {};
        const auto tagBytes = lame_get_lametag_frame (g, lameTag.data(), lameTag.size());

        if (tagBytes > 0 && tagBytes <= lameTag.size())
            if (! out.setPosition ((juce::int64) id3.getSize()) || ! out.write (lameTag.data(), tagBytes))
                return "Writing failed (disk full?).";

        out.flush();
        return out.getStatus().wasOk() ? juce::String() : "Writing failed (disk full?).";
    }

    juce::String writeAac (const AudioSnapshot& snapshot, double sampleRate, const juce::File& file, const Settings& settings,
                           const std::function<bool (float)>& progress, const FileInfo& info)
    {
        const auto channels = snapshot.getNumChannels();
        const auto total = snapshot.getNumFrames();
        juce::int64 pos = 0;

        // The file keeps room for the tags and cover, which go in afterwards: the encoder reserves about 197
        // bytes per second asked for, so this asks for about 1.2 times the room they take, 10 s more.
        auto tagBytes = info.hasCover() ? (double) info.cover->getSize() + 64.0 : 0.0;

        for (const auto* t : { &info.title, &info.artist, &info.album, &info.genre, &info.year, &info.track, &info.comment })
            tagBytes += (double) t->getNumBytesAsUTF8() + 32.0;

        const auto tagRoom = info.hasTags() ? 10.0 + tagBytes / 160.0 : 0.0;
        const auto error = AacWriter::write (file.getFullPathName().toRawUTF8(), sampleRate, outputRate (sampleRate), (unsigned) channels,
                                             settings.kbps, (double) total / sampleRate + tagRoom, [&] (float* const* dest, int maxFrames)
        {
            if (progress != nullptr && ! progress ((float) ((double) pos / (double) total)))
                return -1;

            const auto count = (int) juce::jmin ((juce::int64) maxFrames, total - pos);
            juce::AudioBuffer<float> view (dest, channels, count);
            snapshot.read (view, 0, pos, count);
            pos += count;
            return count;
        });

        if (! error.empty())
            return juce::String::fromUTF8 (error.c_str());

        return FileInfoIO::addMp4Tags (file, info);
    }
}

bool Settings::isValid() const
{
    return variable ? kind == Kind::mp3 && vbrQuality >= 0 && vbrQuality <= 9 : bitRates().contains (kbps);
}

const juce::Array<int>& bitRates()
{
    static const juce::Array<int> rates { 320, 256, 224, 192, 160, 128, 96 };
    return rates;
}

bool isLossy (const juce::File& file)
{
    return file.hasFileExtension ("mp3;m4a");
}

Kind kindFor (const juce::File& file)
{
    return file.hasFileExtension ("m4a") ? Kind::aac : Kind::mp3;
}

double outputRate (double sampleRate)
{
    if (sampleRate > heldRates.back())
        return std::fmod (sampleRate, 44100.0) == 0.0 ? 44100.0 : 48000.0;

    for (auto r : heldRates)
        if (r >= sampleRate)
            return r;

    return heldRates.back();
}

juce::String write (const AudioSnapshot& snapshot, double sampleRate, const juce::File& target, const Settings& settings,
                    const std::function<bool (float)>& progress, const FileInfo& info)
{
    if (! settings.isValid())
        return "Those encoding settings aren't offered.";

    if (snapshot.getNumChannels() < 1 || snapshot.getNumChannels() > 2)
        return juce::String (settings.kind == Kind::mp3 ? "MP3" : "AAC") + " files here hold one or two channels; this audio has "
               + juce::String (snapshot.getNumChannels()) + ".";

    if (sampleRate < heldRates.front())
        return "The sample rate is too low: " + juce::String (sampleRate) + " Hz.";

    juce::TemporaryFile temp (target);
    const auto error = settings.kind == Kind::mp3 ? writeMp3 (snapshot, sampleRate, temp.getFile(), settings, progress, info)
                                                  : writeAac (snapshot, sampleRate, temp.getFile(), settings, progress, info);

    if (error.isNotEmpty())
        return error;

    if (! temp.overwriteTargetFileWithTemporary())
        return "Could not replace " + target.getFullPathName();

    return {};
}
}
