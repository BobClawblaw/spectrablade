#include "SampleRateConverter.h"

#if JUCE_ARM
 #include <arm_neon.h>
#endif

namespace SampleRateConverter
{
namespace
{
    constexpr double passbandEdge = 0.455;    // of the lower rate
    constexpr double stopbandEdge = 0.5;
    constexpr double attenuationDb = 140.0;
    constexpr int maxExactPhases = 4096;
    constexpr int framesPerBlock = 1 << 20;

    bool isWholeNumber (double rate)              { return std::abs (rate - std::round (rate)) < 1.0e-9 && rate >= 1.0; }

    double besselI0 (double x)
    {
        // Power series; converges quickly for the arguments a Kaiser window needs.
        double sum = 1.0, term = 1.0;

        for (int k = 1; k < 200; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;

            if (term < sum * 1.0e-17)
                break;
        }

        return sum;
    }

    /** Coefficient tables: row p holds the taps for an output that falls p/phases of the way
        from input frame i to i + 1; tap k multiplies input frame i - half + 1 + k. */
    struct Filter
    {
        int phases = 1;          // rows (plus one extra row for interpolation when inexact)
        bool exact = true;
        juce::int64 up = 1, down = 1;   // exact ratio out/in = up/down
        int half = 1;            // taps on each side
        int taps = 2;
        std::vector<float> table;

        const float* row (int p) const          { return table.data() + (size_t) p * (size_t) taps; }

        Filter (double inRate, double outRate)
        {
            const auto minRatio = juce::jmin (1.0, outRate / inRate);
            const auto cutoff = 0.5 * (passbandEdge + stopbandEdge) * minRatio;   // cycles per input frame
            const auto transition = (stopbandEdge - passbandEdge) * minRatio;
            const auto length = (attenuationDb - 7.95) / (14.36 * transition);    // Kaiser's estimate
            const auto beta = 0.1102 * (attenuationDb - 8.7);

            half = (int) std::ceil (length * 0.5);
            taps = 2 * half;

            if (isWholeNumber (inRate) && isWholeNumber (outRate))
            {
                const auto in = (juce::int64) std::llround (inRate);
                const auto out = (juce::int64) std::llround (outRate);
                const auto g = std::gcd (in, out);
                up = out / g;
                down = in / g;
            }

            exact = up <= maxExactPhases && isWholeNumber (inRate) && isWholeNumber (outRate);
            phases = exact ? (int) up : maxExactPhases;
            const auto rows = exact ? phases : phases + 1;
            table.resize ((size_t) rows * (size_t) taps);

            const auto i0Beta = besselI0 (beta);

            for (int p = 0; p < rows; ++p)
            {
                std::vector<double> coeffs ((size_t) taps);
                double sum = 0.0;

                for (int k = 0; k < taps; ++k)
                {
                    // Distance from the output instant to the input frame this tap reads.
                    const auto x = (double) p / phases + (double) (half - 1 - k);
                    const auto r = x / (double) half;
                    const auto window = std::abs (r) >= 1.0 ? 0.0 : besselI0 (beta * std::sqrt (1.0 - r * r)) / i0Beta;
                    const auto u = 2.0 * cutoff * x;
                    const auto sinc = std::abs (u) < 1.0e-12 ? 1.0 : std::sin (juce::MathConstants<double>::pi * u)
                                                                       / (juce::MathConstants<double>::pi * u);
                    coeffs[(size_t) k] = 2.0 * cutoff * sinc * window;
                    sum += coeffs[(size_t) k];
                }

                // Unity gain at DC for every phase.
                for (int k = 0; k < taps; ++k)
                    table[(size_t) p * (size_t) taps + (size_t) k] = (float) (coeffs[(size_t) k] / sum);
            }
        }
    };

    double dot (const float* a, const float* b, int n)
    {
       #if JUCE_ARM
        float64x2_t s0 = vdupq_n_f64 (0.0), s1 = s0;
        int i = 0;

        for (; i + 4 <= n; i += 4)
        {
            const auto x = vld1q_f32 (a + i);
            const auto y = vld1q_f32 (b + i);
            s0 = vfmaq_f64 (s0, vcvt_f64_f32 (vget_low_f32 (x)), vcvt_f64_f32 (vget_low_f32 (y)));
            s1 = vfmaq_f64 (s1, vcvt_high_f64_f32 (x), vcvt_high_f64_f32 (y));
        }

        auto sum = vaddvq_f64 (vaddq_f64 (s0, s1));

        for (; i < n; ++i)
            sum += (double) a[i] * (double) b[i];

        return sum;
       #else
        double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
        int i = 0;

        for (; i + 4 <= n; i += 4)
        {
            s0 += (double) a[i] * b[i];
            s1 += (double) a[i + 1] * b[i + 1];
            s2 += (double) a[i + 2] * b[i + 2];
            s3 += (double) a[i + 3] * b[i + 3];
        }

        for (; i < n; ++i)
            s0 += (double) a[i] * b[i];

        return (s0 + s1) + (s2 + s3);
       #endif
    }

    /** Output frames [first, last) of one channel; padded holds the input with half zeros on each side. */
    void convertRange (const Filter& f, const float* padded, juce::int64 inputFrames, double inRate, double outRate,
                       float* out, juce::int64 first, juce::int64 last, std::atomic<juce::int64>& done,
                       const std::atomic<bool>& cancelled)
    {
        juce::ignoreUnused (inputFrames);

        for (auto n = first; n < last; ++n)
        {
            if (((n - first) & 4095) == 0)
            {
                if (cancelled.load())
                    return;

                done += juce::jmin ((juce::int64) 4096, last - n);
            }

            if (f.exact)
            {
                // Output n sits at input time n * down / up.
                const auto t = n * f.down;
                const auto i = t / f.up;
                const auto p = (int) (t % f.up);
                out[n] = (float) dot (f.row (p), padded + i + 1, f.taps);
            }
            else
            {
                const auto t = (double) n * (inRate / outRate);
                const auto i = (juce::int64) std::floor (t);
                const auto pos = (t - (double) i) * f.phases;
                const auto p = juce::jmin (f.phases - 1, (int) pos);
                const auto a = pos - p;
                const auto lower = dot (f.row (p), padded + i + 1, f.taps);
                const auto upper = dot (f.row (p + 1), padded + i + 1, f.taps);
                out[n] = (float) (lower + a * (upper - lower));
            }
        }
    }
}

juce::int64 getOutputLength (juce::int64 inputFrames, double inRate, double outRate)
{
    if (isWholeNumber (inRate) && isWholeNumber (outRate))
    {
        const auto in = (juce::int64) std::llround (inRate), out = (juce::int64) std::llround (outRate);
        const auto g = std::gcd (in, out);
        return (inputFrames * (out / g) + in / g - 1) / (in / g);
    }

    return (juce::int64) std::ceil ((double) inputFrames * outRate / inRate);
}

bool convert (const juce::AudioBuffer<float>& input, double inRate, double outRate,
              juce::AudioBuffer<float>& output, const std::function<bool (float)>& progress)
{
    const auto inFrames = (juce::int64) input.getNumSamples();
    const auto outFrames = getOutputLength (inFrames, inRate, outRate);
    const auto channels = input.getNumChannels();
    output.setSize (channels, (int) outFrames);

    const Filter filter (inRate, outRate);
    std::vector<float> padded ((size_t) (inFrames + 2 * filter.half + 2), 0.0f);
    std::atomic<juce::int64> done { 0 };
    std::atomic<bool> cancelled { false };
    const auto total = juce::jmax ((juce::int64) 1, outFrames * channels);
    const auto threads = (int) juce::jlimit (1u, 16u, std::thread::hardware_concurrency());

    for (int ch = 0; ch < channels; ++ch)
    {
        std::copy (input.getReadPointer (ch), input.getReadPointer (ch) + inFrames, padded.begin() + filter.half);

        std::vector<std::thread> workers;
        const auto per = (outFrames + threads - 1) / threads;

        for (int t = 0; t < threads; ++t)
        {
            const auto first = juce::jmin (outFrames, per * t), last = juce::jmin (outFrames, per * (t + 1));

            if (first < last)
                workers.emplace_back ([&, first, last, ch]
                {
                    convertRange (filter, padded.data(), inFrames, inRate, outRate, output.getWritePointer (ch),
                                  first, last, done, cancelled);
                });
        }

        // Report progress from this thread while the workers run.
        while (progress != nullptr && done.load() < (ch + 1) * outFrames && ! cancelled.load())
        {
            if (! progress ((float) ((double) done.load() / (double) total)))
                cancelled = true;

            std::this_thread::sleep_for (std::chrono::milliseconds (20));
        }

        for (auto& w : workers)
            w.join();

        if (cancelled.load())
            return false;
    }

    if (progress != nullptr)
        progress (1.0f);

    return true;
}

AudioDocument::Snapshot convert (const AudioSnapshot& snapshot, double inRate, double outRate,
                                 const std::function<bool (float)>& progress)
{
    juce::AudioBuffer<float> in (snapshot.getNumChannels(), (int) snapshot.getNumFrames());
    snapshot.read (in, 0, 0, in.getNumSamples());

    juce::AudioBuffer<float> out;

    if (! convert (in, inRate, outRate, out, progress))
        return nullptr;

    in.setSize (0, 0);

    // Store the result in blocks of the usual size, like a loaded file.
    std::vector<AudioSnapshot::Segment> segments;

    for (int pos = 0; pos < out.getNumSamples(); pos += framesPerBlock)
    {
        const auto count = juce::jmin (framesPerBlock, out.getNumSamples() - pos);
        juce::AudioBuffer<float> block (out.getNumChannels(), count);

        for (int ch = 0; ch < out.getNumChannels(); ++ch)
            block.copyFrom (ch, 0, out, ch, pos, count);

        segments.push_back ({ std::make_shared<const AudioBlock> (std::move (block)), 0, count });
    }

    return std::make_shared<const AudioSnapshot> (snapshot.getNumChannels(), std::move (segments));
}

AudioClip convert (const AudioClip& clip, double outRate, const std::function<bool (float)>& progress)
{
    const AudioSnapshot source (clip.numChannels, clip.segments);
    const auto converted = convert (source, clip.sampleRate, outRate, progress);

    if (converted == nullptr)
        return {};

    return { clip.numChannels, outRate, converted->getSegments() };
}

Markers convert (const Markers& markers, double inRate, double outRate)
{
    Markers result;

    for (auto m : markers)
    {
        const auto end = (juce::int64) std::llround ((double) m.getEnd() * outRate / inRate);
        m.start = (juce::int64) std::llround ((double) m.start * outRate / inRate);
        m.length = m.isRegion() ? juce::jmax ((juce::int64) 1, end - m.start) : 0;
        result.push_back (m);
    }

    return result;
}
}
