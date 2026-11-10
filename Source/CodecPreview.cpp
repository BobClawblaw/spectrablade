#include "CodecPreview.h"
#include "AudioFileIO.h"
#include "AudioEdits.h"
#include "SampleRateConverter.h"

namespace CodecPreview
{
namespace
{
    constexpr int mostDelay = 8192;

    /** The lag (0 to mostDelay) at which the decoded mix lines up best with the original's over the
        first second: the one with the largest correlation. */
    int delayOf (const juce::AudioBuffer<float>& original, const juce::AudioBuffer<float>& decoded)
    {
        const auto n = juce::jmin (48000, original.getNumSamples(), decoded.getNumSamples() - mostDelay);

        if (n < 1024)
            return 0;

        std::vector<double> a ((size_t) n), b ((size_t) (n + mostDelay));

        for (int i = 0; i < n; ++i)
            for (int ch = 0; ch < original.getNumChannels(); ++ch)
                a[(size_t) i] += original.getSample (ch, i);

        for (int i = 0; i < n + mostDelay; ++i)
            for (int ch = 0; ch < decoded.getNumChannels(); ++ch)
                b[(size_t) i] += decoded.getSample (juce::jmin (ch, decoded.getNumChannels() - 1), i);

        auto best = 0;
        auto bestSum = -1.0e300;

        for (int lag = 0; lag <= mostDelay; ++lag)
        {
            auto sum = 0.0;

            for (int i = 0; i < n; i += 1)
                sum += a[(size_t) i] * b[(size_t) (i + lag)];

            if (sum > bestSum)
            {
                bestSum = sum;
                best = lag;
            }
        }

        return best;
    }
}

std::optional<Result> roundTrip (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const LossyFiles::Settings& settings,
                                 juce::String& error, const std::function<bool (float)>& progress)
{
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (! settings.isValid() || sampleRate <= 0.0 || s.getNumChannels() < 1 || s.getNumChannels() > 2 || start == end || end - start > std::numeric_limits<int>::max() / 4)
    {
        error = "The codec preview needs valid settings, one or two channels and a selection.";
        return std::nullopt;
    }

    const auto frames = (int) (end - start);
    juce::AudioBuffer<float> original (s.getNumChannels(), frames);
    s.read (original, 0, start, frames);

    // The range, with a second of silence after it so the encoder's delay doesn't lose its end,
    // through the encoder to a temporary file.
    juce::AudioBuffer<float> padded (s.getNumChannels(), frames + (int) sampleRate);
    padded.clear();

    for (int ch = 0; ch < s.getNumChannels(); ++ch)
        padded.copyFrom (ch, 0, original, ch, 0, frames);

    const auto paddedSnapshot = AudioEdits::fromBuffer (std::move (padded));
    const juce::TemporaryFile temporary (settings.kind == LossyFiles::Kind::mp3 ? ".mp3" : ".m4a");
    const auto file = temporary.getFile();
    const auto written = LossyFiles::write (*paddedSnapshot, sampleRate, file, settings, [&progress] (float p) { return progress == nullptr || progress (0.7f * p); });

    if (written.isNotEmpty())
    {
        error = written;
        return std::nullopt;
    }

    juce::AudioFormatManager formats;
    AudioFileIO::registerFormats (formats);
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr || reader->lengthInSamples < 1)
    {
        error = "The encoded file couldn't be read back.";
        return std::nullopt;
    }

    juce::AudioBuffer<float> decoded ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&decoded, 0, decoded.getNumSamples(), 0, true, true);
    const auto bytes = (double) file.getSize();

    if (progress && ! progress (0.85f))
        return std::nullopt;

    // Back to the audio's rate where the file's differs.
    if (! juce::exactlyEqual (reader->sampleRate, sampleRate))
    {
        juce::AudioBuffer<float> atRate;
        SampleRateConverter::convert (decoded, reader->sampleRate, sampleRate, atRate);
        decoded = std::move (atRate);
    }

    // Lined up and trimmed to the range.
    const auto delay = delayOf (original, decoded);
    juce::AudioBuffer<float> lined (s.getNumChannels(), frames);
    lined.clear();

    for (int ch = 0; ch < s.getNumChannels(); ++ch)
    {
        const auto from = juce::jmin (ch, decoded.getNumChannels() - 1);
        const auto count = juce::jmax (0, juce::jmin (frames, decoded.getNumSamples() - delay));

        if (count > 0)
            lined.copyFrom (ch, 0, decoded, from, delay, count);
    }

    Result r;
    r.delayFrames = delay;
    r.bytesPerSecond = bytes / ((frames + sampleRate) / sampleRate);
    r.snapshot = AudioEdits::processRange (s, start, end, [&lined] (juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            b.copyFrom (ch, 0, lined, ch, 0, b.getNumSamples());
    });

    if (r.snapshot == nullptr)
    {
        error = "The preview couldn't be put in place.";
        return std::nullopt;
    }

    return r;
}
}
