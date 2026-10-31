#include "NoiseReduction.h"
#include "AudioEdits.h"

namespace NoiseReduction
{
namespace
{
    constexpr int fftOrder = 12;
    constexpr int size = frameSize;
    constexpr int hop = size / 4;
    constexpr int bins = size / 2 + 1;
    constexpr double smoothing = 0.98;          // weight of the previous frame in the SNR estimate
    constexpr int outputBlockFrames = 1 << 20;

    static_assert (size == 1 << fftOrder);

    double smoothStep (double x)
    {
        x = juce::jlimit (0.0, 1.0, x);
        return x * x * (3.0 - 2.0 * x);
    }

    struct Transform
    {
        Transform()
        {
            for (int i = 0; i < size; ++i)
                window[(size_t) i] = (float) std::sqrt (0.5 * (1.0 - std::cos (juce::MathConstants<double>::twoPi * i / size)));

            // The windows multiply to a periodic Hann; at 75% overlap those sum to 2. The FFT
            // pair's own scale is measured rather than assumed.
            data.assign (data.size(), 0.0f);
            data[0] = 1.0f;
            fft.performRealOnlyForwardTransform (data.data(), true);
            fft.performRealOnlyInverseTransform (data.data());
            scale = 1.0f / (2.0f * data[0]);
        }

        /** Windows frame (size samples) into data and transforms it. */
        void forward (const float* frame)
        {
            for (int i = 0; i < size; ++i)
                data[(size_t) i] = frame[i] * window[(size_t) i];

            std::fill (data.begin() + size, data.end(), 0.0f);
            fft.performRealOnlyForwardTransform (data.data(), true);
        }

        float power (int bin) const
        {
            const auto re = data[(size_t) (2 * bin)], im = data[(size_t) (2 * bin + 1)];
            return re * re + im * im;
        }

        juce::dsp::FFT fft { fftOrder };
        std::vector<float> window = std::vector<float> ((size_t) size);
        std::vector<float> data = std::vector<float> ((size_t) size * 2);
        float scale = 1.0f;
    };
}

std::optional<Print> capture (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate)
{
    start = juce::jmax ((juce::int64) 0, start);
    end = juce::jmin (snapshot.getNumFrames(), end);

    if (end - start < size)
        return std::nullopt;

    const auto channels = snapshot.getNumChannels();
    Print print { sampleRate, std::vector<std::vector<float>> ((size_t) channels, std::vector<float> ((size_t) bins)) };
    std::vector<std::vector<double>> sums ((size_t) channels, std::vector<double> ((size_t) bins));
    Transform t;
    juce::AudioBuffer<float> frame (channels, size);
    int frames = 0;

    for (auto p = start; p + size <= end; p += hop, ++frames)
    {
        snapshot.read (frame, 0, p, size);

        for (int ch = 0; ch < channels; ++ch)
        {
            t.forward (frame.getReadPointer (ch));

            for (int b = 0; b < bins; ++b)
                sums[(size_t) ch][(size_t) b] += t.power (b);
        }
    }

    for (int ch = 0; ch < channels; ++ch)
        for (int b = 0; b < bins; ++b)
            print.power[(size_t) ch][(size_t) b] = (float) (sums[(size_t) ch][(size_t) b] / frames);

    return print;
}

AudioDocument::Snapshot reduce (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate,
                                const Print& print, Settings settings, const std::function<bool (float)>& progress)
{
    const auto length = snapshot.getNumFrames();
    const auto channels = snapshot.getNumChannels();
    start = juce::jlimit ((juce::int64) 0, length, start);
    end = juce::jlimit (start, length, end);

    if (end <= start || print.power.empty())
        return nullptr;

    // The noise each channel is compared with, raised by the threshold.
    const auto threshold = std::pow (10.0f, settings.thresholdDb / 10.0f);
    std::vector<std::vector<float>> noise ((size_t) channels, std::vector<float> ((size_t) bins));

    for (int ch = 0; ch < channels; ++ch)
    {
        for (int b = 0; b < bins; ++b)
        {
            float n = 0.0f;

            if ((int) print.power.size() == channels)
                n = print.power[(size_t) ch][(size_t) b];
            else
            {
                for (auto& p : print.power)
                    n += p[(size_t) b];

                n /= (float) print.power.size();
            }

            noise[(size_t) ch][(size_t) b] = n * threshold;
        }
    }

    const auto floorGain = std::pow (10.0f, -juce::jmax (0.0f, settings.reductionDb) / 20.0f);

    // Frames start two frames before the selection, so the SNR estimate has settled and every
    // replaced sample has full window overlap; the selection plus one frame each side is replaced.
    const auto workStart = start - 2 * size;
    const auto replaceStart = juce::jmax ((juce::int64) 0, start - size);
    const auto replaceEnd = juce::jmin (length, end + size);
    const auto totalFrames = (replaceEnd - workStart + hop - 1) / hop;

    Transform t;
    juce::AudioBuffer<float> frame (channels, size);
    juce::AudioBuffer<float> sum (channels, size);    // overlap-add of [q, q + size)
    sum.clear();
    std::vector<std::vector<float>> previousClean ((size_t) channels, std::vector<float> ((size_t) bins));

    std::vector<AudioSnapshot::Segment> segments;
    juce::AudioBuffer<float> block (channels, outputBlockFrames);
    int blockFill = 0;

    auto emit = [&] (juce::int64 from, int count)
    {
        // Keep only what falls in the replaced range.
        const auto first = juce::jmax (from, replaceStart);
        const auto last = juce::jmin (from + count, replaceEnd);

        for (auto pos = first; pos < last;)
        {
            const auto n = (int) juce::jmin (last - pos, (juce::int64) (outputBlockFrames - blockFill));

            for (int ch = 0; ch < channels; ++ch)
                block.copyFrom (ch, blockFill, sum, ch, (int) (pos - from), n);

            blockFill += n;
            pos += n;

            if (blockFill == outputBlockFrames)
            {
                segments.push_back ({ std::make_shared<const AudioBlock> (std::move (block)), 0, outputBlockFrames });
                block = juce::AudioBuffer<float> (channels, outputBlockFrames);
                blockFill = 0;
            }
        }
    };

    juce::int64 frameIndex = 0;

    for (auto q = workStart; q < replaceEnd; q += hop, ++frameIndex)
    {
        snapshot.read (frame, 0, q, size);

        // Full inside the selection, easing out over one hop either side.
        const auto centre = (double) (q + size / 2);
        const auto timeWeight = (float) smoothStep (juce::jmin (centre - ((double) start - hop), ((double) end + hop) - centre) / (2.0 * hop));

        for (int ch = 0; ch < channels; ++ch)
        {
            t.forward (frame.getReadPointer (ch));
            const auto& n = noise[(size_t) ch];
            auto& clean = previousClean[(size_t) ch];

            for (int b = 0; b < bins; ++b)
            {
                const auto p = t.power (b);
                float g = 1.0f;

                if (n[(size_t) b] > 0.0f)
                {
                    const auto posterior = p / n[(size_t) b];
                    const auto prior = (float) (smoothing * clean[(size_t) b] / n[(size_t) b]
                                                + (1.0 - smoothing) * juce::jmax (0.0f, posterior - 1.0f));
                    const auto wiener = prior / (1.0f + prior);
                    clean[(size_t) b] = wiener * wiener * p;
                    g = floorGain + (1.0f - floorGain) * wiener;
                }

                g = 1.0f + (g - 1.0f) * timeWeight;
                t.data[(size_t) (2 * b)] *= g;
                t.data[(size_t) (2 * b + 1)] *= g;
            }

            t.fft.performRealOnlyInverseTransform (t.data.data());
            auto* dest = sum.getWritePointer (ch);

            for (int i = 0; i < size; ++i)
                dest[i] += t.data[(size_t) i] * t.window[(size_t) i] * t.scale;
        }

        // No later frame reaches [q, q + hop): it is finished.
        emit (q, hop);

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* s = sum.getWritePointer (ch);
            std::memmove (s, s + hop, sizeof (float) * (size_t) (size - hop));
            juce::FloatVectorOperations::clear (s + size - hop, hop);
        }

        if (progress != nullptr && (frameIndex & 63) == 0 && ! progress ((float) ((double) frameIndex / (double) totalFrames)))
            return nullptr;
    }

    if (blockFill > 0)
    {
        block.setSize (channels, blockFill, true);
        segments.push_back ({ std::make_shared<const AudioBlock> (std::move (block)), 0, blockFill });
    }

    const AudioClip clip { channels, sampleRate, std::move (segments) };
    jassert (clip.getNumFrames() == replaceEnd - replaceStart);
    return AudioEdits::replaceWithClip (snapshot, replaceStart, replaceEnd, clip);
}
}
