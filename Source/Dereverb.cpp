#include "Dereverb.h"
#include "AudioEdits.h"

namespace Dereverb
{
namespace
{
    constexpr double delaySeconds = 0.08;     // how far back the reverberation is predicted from
    constexpr float smoothing = 0.7f;         // weight of the previous frame in each band's power
    constexpr int spread = 8;                 // bins either side each band's power is averaged over
    constexpr float release = 0.5f;           // weight of the previous frame in a falling gain
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
            data.resize ((size_t) size * 2);

            for (int i = 0; i < size; ++i)
                window[(size_t) i] = (float) std::sqrt (0.5 * (1.0 - std::cos (juce::MathConstants<double>::twoPi * i / size)));

            // The windows multiply to a periodic Hann; at 75% overlap those sum to 2. The FFT
            // pair's own scale is measured rather than assumed.
            std::fill (data.begin(), data.end(), 0.0f);
            data[0] = 1.0f;
            fft.performRealOnlyForwardTransform (data.data(), true);
            fft.performRealOnlyInverseTransform (data.data());
            scale = 1.0f / (2.0f * data[0]);
        }

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

        juce::dsp::FFT fft;
        int size;
        std::vector<float> window, data;
        float scale = 1.0f;
    };

    /** One channel's state from frame to frame. */
    struct Channel
    {
        std::vector<float> power;                    // each band's smoothed power
        std::vector<std::vector<float>> history;     // the last delayFrames frames of it, a ring
        std::vector<float> gain;                     // each band's gain in the frame before
        int next = 0;                                // the oldest frame in the ring
    };
}

bool isValid (const Settings& s)
{
    return std::isfinite (s.reverbSeconds) && std::isfinite (s.reductionDb) && s.reverbSeconds >= Settings::shortestReverb
           && s.reverbSeconds <= Settings::longestReverb && s.reductionDb >= 0.0f && s.reductionDb <= Settings::mostReduction;
}

int frameSize (double sampleRate)
{
    return 1 << orderFor (sampleRate);
}

std::optional<float> measure (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate,
                              const std::function<bool (float)>& progress)
{
    constexpr std::array<std::pair<double, double>, 4> bands { { { 250.0, 500.0 }, { 500.0, 1000.0 }, { 1000.0, 2000.0 }, { 2000.0, 4000.0 } } };
    constexpr double gateDb = 20.0, floorShare = 0.1;

    const auto length = snapshot.getNumFrames();
    const auto channels = snapshot.getNumChannels();
    start = juce::jlimit ((juce::int64) 0, length, start);
    end = juce::jlimit (start, length, end);

    if (channels == 0 || sampleRate <= 0.0)
        return std::nullopt;

    Transform t (orderFor (sampleRate));
    const auto size = t.size, hop = size / 4;
    const auto delayFrames = juce::jmax (1, (int) std::lround (delaySeconds * sampleRate / hop));
    const auto frames = (int) juce::jmax ((juce::int64) 0, (end - start - size) / hop + 1);

    if (frames <= delayFrames)
        return std::nullopt;

    // Each frame's power in each band, the channels summed.
    std::vector<std::array<double, 4>> power ((size_t) frames, std::array<double, 4> {});
    juce::AudioBuffer<float> frame (channels, size);
    std::array<std::pair<int, int>, 4> bins {};

    for (size_t b = 0; b < bands.size(); ++b)
        bins[b] = { (int) std::ceil (bands[b].first * size / sampleRate), juce::jmin (size / 2, (int) std::ceil (bands[b].second * size / sampleRate)) };

    for (int k = 0; k < frames; ++k)
    {
        snapshot.read (frame, 0, start + (juce::int64) k * hop, size);

        for (int ch = 0; ch < channels; ++ch)
        {
            t.forward (frame.getReadPointer (ch));

            for (size_t b = 0; b < bands.size(); ++b)
                for (int bin = bins[b].first; bin < bins[b].second; ++bin)
                    power[(size_t) k][b] += t.power (bin);
        }

        if (progress != nullptr && (k & 63) == 0 && ! progress ((float) k / (float) frames))
            return std::nullopt;
    }

    // The fall over the delay from every frame that stood over the gate, in every band; the
    // typical one is the room's decay.
    std::vector<double> falls;

    for (size_t b = 0; b < bands.size(); ++b)
    {
        std::vector<double> levels ((size_t) frames);
        double loudest = 0.0;

        for (int k = 0; k < frames; ++k)
        {
            levels[(size_t) k] = power[(size_t) k][b];
            loudest = juce::jmax (loudest, levels[(size_t) k]);
        }

        const auto at = (size_t) std::floor (floorShare * (frames - 1));
        std::nth_element (levels.begin(), levels.begin() + (long) at, levels.end());
        const auto floor = juce::jmax (levels[at], loudest * 1.0e-12, 1.0e-30);
        const auto gate = floor * std::pow (10.0, gateDb / 10.0);

        for (int k = delayFrames; k < frames; ++k)
        {
            const auto before = power[(size_t) (k - delayFrames)][b], now = power[(size_t) k][b];

            if (before >= gate && now < before)
                falls.push_back (10.0 * std::log10 (before / juce::jmax (now, 1.0e-30)));
        }
    }

    if (falls.empty())
        return std::nullopt;

    const auto middle = falls.begin() + (long) (falls.size() / 2);
    std::nth_element (falls.begin(), middle, falls.end());
    return (float) (60.0 * delayFrames * hop / sampleRate / juce::jmax (*middle, 1.0e-3));
}

AudioDocument::Snapshot reduce (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate,
                                Settings settings, const std::function<bool (float)>& progress, float* reverbSecondsUsed)
{
    const auto length = snapshot.getNumFrames();
    const auto channels = snapshot.getNumChannels();
    start = juce::jlimit ((juce::int64) 0, length, start);
    end = juce::jlimit (start, length, end);

    if (end <= start || channels == 0 || sampleRate <= 0.0 || ! isValid (settings))
        return nullptr;

    // The room's own time caps the setting, so a sound that stops faster than the setting's tail
    // would (a dry voice's syllables) isn't read as one.
    auto stopped = false;
    const auto measured = measure (snapshot, start, end, sampleRate, [&] (float p)
    {
        if (progress == nullptr || progress (0.2f * p))
            return true;

        stopped = true;
        return false;
    });

    if (stopped)
        return nullptr;

    if (measured.has_value())
        settings.reverbSeconds = juce::jmax (Settings::shortestReverb, juce::jmin (settings.reverbSeconds, *measured));

    if (reverbSecondsUsed != nullptr)
        *reverbSecondsUsed = settings.reverbSeconds;

    Transform t (orderFor (sampleRate));
    const auto size = t.size, hop = size / 4, bins = size / 2 + 1;

    // The reverberation predicted from delayFrames back: that frame's power, decayed as the
    // reverb time has it (60 dB over reverbSeconds).
    const auto delayFrames = juce::jmax (1, (int) std::lround (delaySeconds * sampleRate / hop));
    const auto decay = (float) std::pow (10.0, -6.0 * delayFrames * hop / sampleRate / settings.reverbSeconds);
    const auto floorGain = std::pow (10.0f, -settings.reductionDb / 20.0f);

    // Frames start the delay and eight hops before the range, so the prediction has history
    // there; the range plus one frame each side is replaced.
    const auto workStart = start - (juce::int64) (delayFrames + 8) * hop;
    const auto replaceStart = juce::jmax ((juce::int64) 0, start - size);
    const auto replaceEnd = juce::jmin (length, end + size);
    const auto totalFrames = (replaceEnd - workStart + hop - 1) / hop;

    std::vector<Channel> state ((size_t) channels);

    for (auto& c : state)
    {
        c.power.assign ((size_t) bins, 0.0f);
        c.history.assign ((size_t) delayFrames, std::vector<float> ((size_t) bins, 0.0f));
        c.gain.assign ((size_t) bins, 1.0f);
    }

    juce::AudioBuffer<float> frame (channels, size), sum (channels, size);   // sum: the overlap-add of [q, q + size)
    sum.clear();
    std::vector<float> wanted ((size_t) bins), raw ((size_t) bins);
    std::vector<AudioSnapshot::Segment> segments;
    juce::AudioBuffer<float> block (channels, outputBlockFrames);
    int blockFill = 0;

    const auto emit = [&] (juce::int64 from, int count)
    {
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

        // Full inside the range, easing out over one hop either side.
        const auto centre = (double) (q + size / 2);
        const auto timeWeight = (float) smoothStep (juce::jmin (centre - ((double) start - hop), ((double) end + hop) - centre) / (2.0 * hop));

        for (int ch = 0; ch < channels; ++ch)
        {
            auto& c = state[(size_t) ch];
            auto& before = c.history[(size_t) c.next];   // delayFrames ago
            t.forward (frame.getReadPointer (ch));

            for (int b = 0; b < bins; ++b)
                raw[(size_t) b] = t.power (b);

            // A single bin's power jumps about from frame to frame even in a smooth tail, so it is
            // averaged over its neighbours (a running sum) and over time before it is compared.
            double across = 0.0;

            for (int k = 0; k <= juce::jmin (spread, bins - 1); ++k)
                across += raw[(size_t) k];

            for (int b = 0; b < bins; ++b)
            {
                const auto lo = juce::jmax (0, b - spread), hi = juce::jmin (bins - 1, b + spread);
                auto& p = c.power[(size_t) b];
                p = smoothing * p + (1.0f - smoothing) * (float) (across / (hi - lo + 1));

                if (b + spread + 1 < bins)
                    across += raw[(size_t) (b + spread + 1)];

                if (b - spread >= 0)
                    across -= raw[(size_t) (b - spread)];

                // How much of the band is the predicted tail: 1 for a pure tail, `decay` for a
                // steady level, less for a rising one. Mapped so a steady level keeps its gain.
                const auto ratio = p > 1.0e-20f ? decay * before[(size_t) b] / p : 0.0f;
                wanted[(size_t) b] = juce::jlimit (0.0f, 1.0f, (1.0f - ratio) / (1.0f - decay));
            }

            before = c.power;
            c.next = (c.next + 1) % delayFrames;

            for (int b = 0; b < bins; ++b)
            {
                // Over the band and its neighbours; rising at once, falling more slowly.
                const auto lo = juce::jmax (0, b - 1), hi = juce::jmin (bins - 1, b + 1);
                auto g = 0.0f;

                for (int k = lo; k <= hi; ++k)
                    g += wanted[(size_t) k];

                g = floorGain + (1.0f - floorGain) * g / (float) (hi - lo + 1);
                auto& held = c.gain[(size_t) b];
                held = g >= held ? g : release * held + (1.0f - release) * g;

                const auto applied = 1.0f + (held - 1.0f) * timeWeight;
                t.data[(size_t) (2 * b)] *= applied;
                t.data[(size_t) (2 * b + 1)] *= applied;
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

        if (progress != nullptr && (frameIndex & 63) == 0 && ! progress (0.2f + 0.8f * (float) ((double) frameIndex / (double) totalFrames)))
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
