#include "AdaptiveDenoise.h"
#include "AudioEdits.h"

namespace AdaptiveDenoise
{
namespace
{
    constexpr int fftOrder = 12, size = 1 << fftOrder, hop = size / 4, bins = size / 2 + 1;
    constexpr float smoothing = 0.98f, powerSmoothing = 0.9f, minimumBias = 3.0f, nearMinimum = 4.0f;   // the least is about a third of the mean; near is within 6 dB of it

    std::vector<float> rootHann()
    {
        std::vector<float> w ((size_t) size);

        for (int i = 0; i < size; ++i)
            w[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / size));

        return w;
    }
}

bool isValid (const Settings& s)
{
    return s.reductionDb >= 0.0f && s.reductionDb <= 40.0f && s.thresholdDb >= 0.0f && s.thresholdDb <= 12.0f
        && s.trackingSeconds >= 0.5f && s.trackingSeconds <= 5.0f;
}

AudioDocument::Snapshot reduce (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, Settings settings,
                                const std::function<bool (float)>& progress)
{
    if (! isValid (settings) || sampleRate <= 0.0)
        return nullptr;

    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (start == end || end - start > std::numeric_limits<int>::max() / 4)
        return nullptr;

    const auto channels = s.getNumChannels();
    const auto frames = (int) (end - start);
    const auto window = rootHann();
    const auto floorGain = std::pow (10.0f, -settings.reductionDb / 20.0f);
    const auto threshold = std::pow (10.0f, settings.thresholdDb / 10.0f);
    const auto span = juce::jmax (1, (int) (settings.trackingSeconds * sampleRate / hop));

    // The range with a frame either side: the audio there where the file has it (for the tracking
    // to settle on), silence where it hasn't.
    const auto padded = frames + 2 * size;
    juce::AudioBuffer<float> in (channels, padded), out (channels, padded);
    in.clear();
    out.clear();
    const auto before = (int) juce::jmin ((juce::int64) size, start), after = (int) juce::jmin ((juce::int64) size, s.getNumFrames() - end);
    s.read (in, size - before, start - before, frames + before + after);

    juce::dsp::FFT fft (fftOrder);
    std::vector<float> data ((size_t) (2 * size));

    // Per channel and bin: the smoothed power, the Wiener's clean estimate, the noise in use, and
    // four sub-windows (a quarter of the span each, the newest first) of the least smoothed power
    // and the sum and count of the frames near the least.
    struct Tracking
    {
        std::vector<float> smoothed, clean, noise;
        std::array<std::vector<float>, 4> least, nearSum, nearCount;
    };
    std::vector<Tracking> tracking ((size_t) channels);

    for (auto& t : tracking)
    {
        t.smoothed.assign ((size_t) bins, 0.0f);
        t.clean.assign ((size_t) bins, 0.0f);
        t.noise.assign ((size_t) bins, 0.0f);

        for (int w = 0; w < 4; ++w)
        {
            t.least[(size_t) w].assign ((size_t) bins, 1.0e30f);
            t.nearSum[(size_t) w].assign ((size_t) bins, 0.0f);
            t.nearCount[(size_t) w].assign ((size_t) bins, 0.0f);
        }
    }

    const auto steps = (padded - size) / hop + 1;
    const auto subSpan = juce::jmax (2, span / 4);
    auto inSubWindow = 0;

    for (int step = 0; step < steps; ++step)
    {
        const auto pos = step * hop;

        if (progress && (step % 16) == 0 && ! progress ((float) step / (float) steps))
            return nullptr;

        // Full inside the range, easing over a frame at each edge (the padding's frames are silence).
        const auto centre = pos + size / 2 - size;
        const auto edge = juce::jmin (centre, frames - centre);
        const auto timeWeight = (float) juce::jlimit (0.0, 1.0, (double) (edge + size / 2) / size);

        // Tracked only from frames wholly within the audio there is (a frame over the silence
        // either side would read as noise falling to nothing).
        const auto tracked = pos >= size - before && pos + size <= size + frames + after;

        for (int ch = 0; ch < channels; ++ch)
        {
            std::fill (data.begin(), data.end(), 0.0f);

            for (int i = 0; i < size; ++i)
                data[(size_t) i] = in.getSample (ch, pos + i) * window[(size_t) i];

            fft.performRealOnlyForwardTransform (data.data(), true);
            auto& t = tracking[(size_t) ch];

            if (tracked)
            {
                // The smoothed power and this sub-window's least; and whether the least has risen past
                // twice the estimate in most bins from 100 Hz to 8 kHz (noise rising, which is broad,
                // against speech starting, which raises its own bins).
                auto risen = 0, counted = 0;
                const auto lowBin = (int) (100.0 / (sampleRate / size)), highBin = juce::jmin (bins - 1, (int) (8000.0 / (sampleRate / size)));

                for (int b = 0; b < bins; ++b)
                {
                    const auto re = data[(size_t) (2 * b)], im = data[(size_t) (2 * b + 1)];
                    const auto p = re * re + im * im;
                    auto& sm = t.smoothed[(size_t) b];
                    sm = sm <= 0.0f ? p : powerSmoothing * sm + (1.0f - powerSmoothing) * p;
                    auto& current = t.least[0][(size_t) b];
                    current = juce::jmin (current, sm);

                    if (b >= lowBin && b <= highBin && inSubWindow >= 8 && t.noise[(size_t) b] > 0.0f)
                    {
                        ++counted;

                        if (current > 2.0f * t.noise[(size_t) b])
                            ++risen;
                    }
                }

                const auto broadRise = counted > 0 && risen * 5 >= counted * 3;

                for (int b = 0; b < bins; ++b)
                {
                    const auto sm = t.smoothed[(size_t) b];
                    const auto current = t.least[0][(size_t) b];

                    // The least over the span (the four sub-windows); in a broad rise, this
                    // sub-window's fresh least instead. Frames within 6 dB of that are noise.
                    auto spanLeast = current;

                    for (int w = 1; w < 4; ++w)
                        spanLeast = juce::jmin (spanLeast, t.least[(size_t) w][(size_t) b]);

                    const auto reference = broadRise ? juce::jmax (spanLeast, current) : spanLeast;

                    if (sm <= nearMinimum * reference)
                    {
                        t.nearSum[0][(size_t) b] += sm;
                        t.nearCount[0][(size_t) b] += 1.0f;
                    }

                    // The estimate: the mean of the near frames over the span; in a broad rise this
                    // sub-window's own where it stands twice over that; the least biased up where too
                    // few frames have been near.
                    auto sum = 0.0f, count = 0.0f;

                    for (int w = 0; w < 4; ++w)
                    {
                        sum += t.nearSum[(size_t) w][(size_t) b];
                        count += t.nearCount[(size_t) w][(size_t) b];
                    }

                    const auto subCount = t.nearCount[0][(size_t) b];
                    const auto subMean = subCount > 0.0f ? t.nearSum[0][(size_t) b] / subCount : 0.0f;
                    auto estimate = count >= 8.0f ? sum / count : spanLeast * minimumBias;

                    if (broadRise && subCount >= 8.0f && subMean > 2.0f * estimate)
                        estimate = subMean;

                    t.noise[(size_t) b] = estimate;
                }
            }

            for (int b = 0; b < bins; ++b)
            {
                const auto re = data[(size_t) (2 * b)], im = data[(size_t) (2 * b + 1)];
                const auto p = re * re + im * im;
                const auto noiseHere = t.noise[(size_t) b] * threshold;
                auto gain = 1.0f;

                if (noiseHere > 0.0f)
                {
                    auto& cl = t.clean[(size_t) b];
                    const auto posterior = p / noiseHere;
                    const auto prior = smoothing * cl / noiseHere + (1.0f - smoothing) * juce::jmax (0.0f, posterior - 1.0f);
                    const auto wiener = prior / (1.0f + prior);
                    cl = wiener * wiener * p;
                    gain = floorGain + (1.0f - floorGain) * wiener;
                }

                gain = 1.0f + (gain - 1.0f) * timeWeight;
                data[(size_t) (2 * b)] *= gain;
                data[(size_t) (2 * b + 1)] *= gain;
            }

            fft.performRealOnlyInverseTransform (data.data());

            for (int i = 0; i < size; ++i)
                out.addSample (ch, pos + i, data[(size_t) i] * window[(size_t) i] / 2.0f);
        }

        if (tracked && ++inSubWindow >= subSpan)
        {
            // The sub-windows roll: the oldest dropped, a new one begun.
            for (auto& t : tracking)
                for (int w = 3; w > 0; --w)
                {
                    t.least[(size_t) w] = t.least[(size_t) w - 1];
                    t.nearSum[(size_t) w] = t.nearSum[(size_t) w - 1];
                    t.nearCount[(size_t) w] = t.nearCount[(size_t) w - 1];
                }

            for (auto& t : tracking)
            {
                std::fill (t.least[0].begin(), t.least[0].end(), 1.0e30f);
                std::fill (t.nearSum[0].begin(), t.nearSum[0].end(), 0.0f);
                std::fill (t.nearCount[0].begin(), t.nearCount[0].end(), 0.0f);
            }

            inSubWindow = 0;
        }

    }

    return AudioEdits::processRange (s, start, end, [&out] (juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            b.copyFrom (ch, 0, out, ch, size, b.getNumSamples());
    });
}
}
