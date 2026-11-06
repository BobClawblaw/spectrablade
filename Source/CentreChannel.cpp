#include "CentreChannel.h"
#include "AudioEdits.h"

namespace CentreChannel
{
namespace
{
    constexpr float smoothing = 0.5f;          // weight of the previous frame in each band's powers
    constexpr double easeOctaves = 1.0 / 3.0;  // how far past the band's edges the change eases out
    constexpr int outputBlockFrames = 1 << 20;

    int orderFor (double sampleRate)
    {
        return sampleRate > 100000.0 ? 13 : sampleRate > 50000.0 ? 12 : 11;
    }

    double smoothStep (double x)
    {
        x = juce::jlimit (0.0, 1.0, x);
        return x * x * (3.0 - 2.0 * x);
    }

    struct Transform
    {
        explicit Transform (int order) : fft (order), size (1 << order)
        {
            window.resize ((size_t) size);

            for (auto& d : data)
                d.resize ((size_t) size * 2);

            for (int i = 0; i < size; ++i)
                window[(size_t) i] = (float) std::sqrt (0.5 * (1.0 - std::cos (juce::MathConstants<double>::twoPi * i / size)));

            // The windows multiply to a periodic Hann; at 75% overlap those sum to 2. The FFT
            // pair's own scale is measured rather than assumed.
            auto& d = data[0];
            std::fill (d.begin(), d.end(), 0.0f);
            d[0] = 1.0f;
            fft.performRealOnlyForwardTransform (d.data(), true);
            fft.performRealOnlyInverseTransform (d.data());
            scale = 1.0f / (2.0f * d[0]);
        }

        void forward (int ch, const float* frame)
        {
            auto& d = data[(size_t) ch];

            for (int i = 0; i < size; ++i)
                d[(size_t) i] = frame[i] * window[(size_t) i];

            std::fill (d.begin() + size, d.end(), 0.0f);
            fft.performRealOnlyForwardTransform (d.data(), true);
        }

        juce::dsp::FFT fft;
        int size;
        std::vector<float> window;
        std::vector<float> data[2];
        float scale = 1.0f;
    };
}

bool isValid (const Settings& s)
{
    return std::isfinite (s.lowHz) && std::isfinite (s.highHz) && std::isfinite (s.reductionDb) && s.lowHz >= Settings::lowest
           && s.highHz <= Settings::highest && s.lowHz < s.highHz && s.reductionDb >= 0.0f && s.reductionDb <= Settings::mostReduction;
}

int frameSize (double sampleRate)
{
    return 1 << orderFor (sampleRate);
}

AudioDocument::Snapshot process (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate,
                                 Settings settings, const std::function<bool (float)>& progress)
{
    const auto length = snapshot.getNumFrames();
    start = juce::jlimit ((juce::int64) 0, length, start);
    end = juce::jlimit (start, length, end);

    if (end <= start || snapshot.getNumChannels() != 2 || sampleRate <= 0.0 || ! isValid (settings))
        return nullptr;

    Transform t (orderFor (sampleRate));
    const auto size = t.size, hop = size / 4, bins = size / 2 + 1;
    const auto lowered = std::pow (10.0f, -settings.reductionDb / 20.0f);

    // How much each bin is worked on: fully inside the band, easing out past its edges.
    std::vector<float> inBand ((size_t) bins);

    for (int b = 0; b < bins; ++b)
    {
        const auto hz = juce::jmax (1.0, b * sampleRate / size);
        const auto below = std::log2 (settings.lowHz / hz), above = std::log2 (hz / settings.highHz);
        inBand[(size_t) b] = (float) smoothStep (1.0 - juce::jmax (below, above) / easeOctaves);
    }

    // Frames start eight hops before the range, so the smoothed powers have history there; the
    // range plus one frame each side is replaced.
    const auto workStart = start - (juce::int64) 8 * hop;
    const auto replaceStart = juce::jmax ((juce::int64) 0, start - size);
    const auto replaceEnd = juce::jmin (length, end + size);
    const auto totalFrames = (replaceEnd - workStart + hop - 1) / hop;

    std::vector<float> common ((size_t) bins, 0.0f), own ((size_t) bins, 0.0f);   // smoothed powers
    std::vector<float> rawCommon ((size_t) bins), rawOwn ((size_t) bins);
    juce::AudioBuffer<float> frame (2, size), sum (2, size);   // sum: the overlap-add of [q, q + size)
    sum.clear();
    std::vector<AudioSnapshot::Segment> segments;
    juce::AudioBuffer<float> block (2, outputBlockFrames);
    int blockFill = 0;

    const auto emit = [&] (juce::int64 from, int count)
    {
        const auto first = juce::jmax (from, replaceStart);
        const auto last = juce::jmin (from + count, replaceEnd);

        for (auto pos = first; pos < last;)
        {
            const auto n = (int) juce::jmin (last - pos, (juce::int64) (outputBlockFrames - blockFill));

            for (int ch = 0; ch < 2; ++ch)
                block.copyFrom (ch, blockFill, sum, ch, (int) (pos - from), n);

            blockFill += n;
            pos += n;

            if (blockFill == outputBlockFrames)
            {
                segments.push_back ({ std::make_shared<const AudioBlock> (std::move (block)), 0, outputBlockFrames });
                block = juce::AudioBuffer<float> (2, outputBlockFrames);
                blockFill = 0;
            }
        }
    };

    juce::int64 frameIndex = 0;

    for (auto q = workStart; q < replaceEnd; q += hop, ++frameIndex)
    {
        snapshot.read (frame, 0, q, size);

        // Full inside the range, easing out over one hop either side.
        const auto centre = (double) (q + size / 2);
        const auto timeWeight = (float) smoothStep (juce::jmin (centre - ((double) start - hop), ((double) end + hop) - centre) / (2.0 * hop));

        t.forward (0, frame.getReadPointer (0));
        t.forward (1, frame.getReadPointer (1));
        auto& l = t.data[0];
        auto& r = t.data[1];

        for (int b = 0; b < bins; ++b)
        {
            const auto lr = l[(size_t) (2 * b)], li = l[(size_t) (2 * b + 1)], rr = r[(size_t) (2 * b)], ri = r[(size_t) (2 * b + 1)];
            rawCommon[(size_t) b] = lr * rr + li * ri;                                // Re (L R*)
            rawOwn[(size_t) b] = 0.5f * (lr * lr + li * li + rr * rr + ri * ri);      // (|L|² + |R|²) / 2
        }

        for (int b = 0; b < bins; ++b)
        {
            // Over the bin and its neighbours, and over time.
            const auto lo = juce::jmax (0, b - 1), hi = juce::jmin (bins - 1, b + 1);
            auto c = 0.0f, o = 0.0f;

            for (int k = lo; k <= hi; ++k)
            {
                c += rawCommon[(size_t) k];
                o += rawOwn[(size_t) k];
            }

            common[(size_t) b] = smoothing * common[(size_t) b] + (1.0f - smoothing) * c;
            own[(size_t) b] = smoothing * own[(size_t) b] + (1.0f - smoothing) * o;
        }

        for (int b = 0; b < bins; ++b)
        {
            // With L = C + Sl and R = C + Sr, sides unrelated to the middle and to each other, the
            // common power is |C|² and the own power |C|² plus the sides' mean; the mid channel
            // holds C and a quarter of the sides' summed power, so its share of C is this.
            const auto c = juce::jmax (0.0f, common[(size_t) b]), o = own[(size_t) b];
            const auto share = o + c > 1.0e-20f ? juce::jmin (1.0f, 2.0f * c / (o + c)) : 0.0f;
            const auto m = share * share * inBand[(size_t) b] * timeWeight;   // how much of the mid is taken as the middle

            for (int part = 0; part < 2; ++part)
            {
                const auto i = (size_t) (2 * b + part);
                const auto mid = 0.5f * (l[i] + r[i]) * m;

                if (settings.keepCentre)
                {
                    // The middle stays; the rest of each channel is lowered (in this band).
                    const auto keep = 1.0f + (lowered - 1.0f) * inBand[(size_t) b] * timeWeight;
                    l[i] = mid + keep * (l[i] - mid);
                    r[i] = mid + keep * (r[i] - mid);
                }
                else
                {
                    l[i] -= (1.0f - lowered) * mid;
                    r[i] -= (1.0f - lowered) * mid;
                }
            }
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            auto& d = t.data[ch];
            t.fft.performRealOnlyInverseTransform (d.data());
            auto* dest = sum.getWritePointer (ch);

            for (int i = 0; i < size; ++i)
                dest[i] += d[(size_t) i] * t.window[(size_t) i] * t.scale;
        }

        // No later frame reaches [q, q + hop): it is finished.
        emit (q, hop);

        for (int ch = 0; ch < 2; ++ch)
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
        block.setSize (2, blockFill, true);
        segments.push_back ({ std::make_shared<const AudioBlock> (std::move (block)), 0, blockFill });
    }

    const AudioClip clip { 2, sampleRate, std::move (segments) };
    jassert (clip.getNumFrames() == replaceEnd - replaceStart);
    return AudioEdits::replaceWithClip (snapshot, replaceStart, replaceEnd, clip);
}
}
