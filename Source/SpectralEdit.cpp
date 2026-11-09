#include "SpectralEdit.h"
#include "AudioEdits.h"
#include <complex>

namespace SpectralEdit
{
namespace
{
    constexpr int fftOrder = 12;
    constexpr int size = 1 << fftOrder;
    constexpr int hop = size / 4;
    constexpr double edgeBins = 3.0;   // frequency taper outside the band

    double smoothStep (double x)
    {
        x = juce::jlimit (0.0, 1.0, x);
        return x * x * (3.0 - 2.0 * x);
    }

    /** changeBand, or changeOutsideBand when outside is set: the same frames, the gain on the
        other side of the band's edges. */
    AudioDocument::Snapshot processBand (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end,
                                         juce::Range<double> band, double sampleRate, float gainDb, bool outside)
    {
        const auto length = snapshot.getNumFrames();
        start = juce::jlimit ((juce::int64) 0, length, start);
        end = juce::jlimit (start, length, end);

        if (end <= start || band.isEmpty())
            return nullptr;

        // Work on the selection plus two frames of context each side, so every replaced sample has
        // full window overlap; outside the selection the gain is one and the audio comes back as it was.
        const auto workStart = start - 2 * size;
        auto workLength = (int) (end - start) + 4 * size;
        workLength += (hop - (workLength - size) % hop) % hop;

        juce::AudioBuffer<float> work (snapshot.getNumChannels(), workLength);
        snapshot.read (work, 0, workStart, workLength);
        juce::AudioBuffer<float> out (snapshot.getNumChannels(), workLength);
        out.clear();

        juce::dsp::FFT fft (fftOrder);
        std::vector<float> window ((size_t) size), data ((size_t) size * 2);

        for (int i = 0; i < size; ++i)
            window[(size_t) i] = (float) std::sqrt (0.5 * (1.0 - std::cos (juce::MathConstants<double>::twoPi * i / size)));

        // The windows multiply to a periodic Hann; at 75% overlap those sum to 2. The FFT pair's own
        // scale is measured rather than assumed.
        data.assign (data.size(), 0.0f);
        data[0] = 1.0f;
        fft.performRealOnlyForwardTransform (data.data(), true);
        fft.performRealOnlyInverseTransform (data.data());
        const auto roundTrip = data[0];
        const auto scale = 1.0f / (2.0f * roundTrip);

        // Gain per bin: the target inside the band (or, for outside, everywhere but the band),
        // tapering to one over a few bins at the band's edges.
        const auto gain = juce::Decibels::decibelsToGain (gainDb, -300.0f);
        const auto binHz = sampleRate / size;
        std::vector<double> bandWeight ((size_t) size / 2 + 1);

        for (int b = 0; b <= size / 2; ++b)
        {
            const auto bin = b * binHz;
            const auto below = (band.getStart() - bin) / binHz, above = (bin - band.getEnd()) / binHz;
            const auto inside = 1.0 - smoothStep (juce::jmax (below, above) / edgeBins);
            bandWeight[(size_t) b] = outside ? 1.0 - inside : inside;
        }

        for (int ch = 0; ch < work.getNumChannels(); ++ch)
        {
            const auto* in = work.getReadPointer (ch);
            auto* dest = out.getWritePointer (ch);

            for (int p = 0; p + size <= workLength; p += hop)
            {
                // Time weight from where the frame's centre falls: full inside the selection,
                // easing out over one hop either side.
                const auto centre = (double) (workStart + p + size / 2);
                const auto timeWeight = smoothStep (juce::jmin (centre - ((double) start - hop), ((double) end + hop) - centre) / (2.0 * hop));

                for (int i = 0; i < size; ++i)
                    data[(size_t) i] = in[p + i] * window[(size_t) i];

                std::fill (data.begin() + size, data.end(), 0.0f);

                if (timeWeight > 0.0)
                {
                    fft.performRealOnlyForwardTransform (data.data(), true);

                    for (int b = 0; b <= size / 2; ++b)
                    {
                        const auto g = (float) (1.0 + ((double) gain - 1.0) * timeWeight * bandWeight[(size_t) b]);
                        data[(size_t) (2 * b)] *= g;
                        data[(size_t) (2 * b + 1)] *= g;
                    }

                    fft.performRealOnlyInverseTransform (data.data());
                }
                else
                {
                    // Nothing changes in this frame: skip the transform pair, keeping its gain.
                    for (int i = 0; i < size; ++i)
                        data[(size_t) i] *= roundTrip;
                }

                for (int i = 0; i < size; ++i)
                    dest[p + i] += data[(size_t) i] * window[(size_t) i] * scale;
            }
        }

        // Replace the selection plus one frame each side (inside the file) with the result.
        const auto replaceStart = juce::jmax ((juce::int64) 0, start - size);
        const auto replaceEnd = juce::jmin (length, end + size);
        juce::AudioBuffer<float> result (out.getNumChannels(), (int) (replaceEnd - replaceStart));

        for (int ch = 0; ch < out.getNumChannels(); ++ch)
            result.copyFrom (ch, 0, out, ch, (int) (replaceStart - workStart), result.getNumSamples());

        AudioClip clip { snapshot.getNumChannels(), sampleRate, AudioEdits::fromBuffer (std::move (result))->getSegments() };
        return AudioEdits::replaceWithClip (snapshot, replaceStart, replaceEnd, clip);
    }
}

AudioDocument::Snapshot changeBand (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end,
                                    juce::Range<double> band, double sampleRate, float gainDb)
{
    return processBand (snapshot, start, end, band, sampleRate, gainDb, false);
}

AudioDocument::Snapshot changeOutsideBand (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end,
                                           juce::Range<double> band, double sampleRate, float gainDb)
{
    return processBand (snapshot, start, end, band, sampleRate, gainDb, true);
}

// Drawn shapes ----------------------------------------------------------------------------------

namespace
{
    std::vector<juce::Range<double>> merged (std::vector<juce::Range<double>> ranges)
    {
        std::sort (ranges.begin(), ranges.end(), [] (auto a, auto b) { return a.getStart() < b.getStart(); });
        std::vector<juce::Range<double>> out;

        for (auto r : ranges)
        {
            if (r.isEmpty())
                continue;

            if (! out.empty() && r.getStart() <= out.back().getEnd())
                out.back() = out.back().getUnionWith (r);
            else
                out.push_back (r);
        }

        return out;
    }
}

bool Shape::isEmpty() const noexcept
{
    return std::none_of (lassos.begin(), lassos.end(), [] (auto& l) { return l.size() >= 3; }) && dabs.empty();
}

std::vector<juce::Range<double>> Shape::octavesAt (double frame) const
{
    std::vector<juce::Range<double>> ranges;

    for (auto& lasso : lassos)
    {
        if (lasso.size() < 3)
            continue;

        // Where an upright line at this frame crosses the outline: inside between pairs of crossings.
        std::vector<double> crossings;

        for (size_t i = 0; i < lasso.size(); ++i)
        {
            const auto a = lasso[i], b = lasso[(i + 1) % lasso.size()];

            if ((a.x <= frame && frame < b.x) || (b.x <= frame && frame < a.x))
                crossings.push_back (a.y + (frame - a.x) * (b.y - a.y) / (b.x - a.x));
        }

        std::sort (crossings.begin(), crossings.end());

        for (size_t i = 0; i + 1 < crossings.size(); i += 2)
            ranges.push_back ({ crossings[i], crossings[i + 1] });
    }

    for (auto& d : dabs)
    {
        const auto dx = (frame - d.frame) / juce::jmax (1.0e-9, d.frameRadius);

        if (std::abs (dx) <= 1.0)
        {
            const auto half = d.octaveRadius * std::sqrt (1.0 - dx * dx);
            ranges.push_back ({ d.octave - half, d.octave + half });
        }
    }

    return merged (std::move (ranges));
}

bool Shape::contains (double frame, double frequency) const
{
    if (frequency <= 0.0)
        return false;

    const auto octave = std::log2 (frequency);

    for (auto r : octavesAt (frame))
        if (r.getStart() <= octave && octave < r.getEnd())
            return true;

    return false;
}

juce::Range<double> Shape::frames() const
{
    std::optional<juce::Range<double>> span;
    const auto add = [&span] (juce::Range<double> r) { span = span ? span->getUnionWith (r) : r; };

    for (auto& lasso : lassos)
        if (lasso.size() >= 3)
            for (auto p : lasso)
                add ({ p.x, p.x });

    for (auto& d : dabs)
        add ({ d.frame - d.frameRadius, d.frame + d.frameRadius });

    return span.value_or (juce::Range<double>());
}

juce::Range<double> Shape::frequencies() const
{
    std::optional<juce::Range<double>> span;
    const auto add = [&span] (juce::Range<double> r) { span = span ? span->getUnionWith (r) : r; };

    for (auto& lasso : lassos)
        if (lasso.size() >= 3)
            for (auto p : lasso)
                add ({ p.y, p.y });

    for (auto& d : dabs)
        add ({ d.octave - d.octaveRadius, d.octave + d.octaveRadius });

    return span ? juce::Range<double> (std::exp2 (span->getStart()), std::exp2 (span->getEnd())) : juce::Range<double>();
}

Mask maskFor (const Shape& shape, double sampleRate, juce::int64 numFrames)
{
    Mask mask;
    const auto span = shape.frames();

    if (shape.isEmpty() || sampleRate <= 0.0 || numFrames <= 0 || span.getEnd() < 0.0 || span.getStart() > (double) numFrames)
        return mask;

    constexpr int bins = shapeSize / 2 + 1;
    constexpr int step = shapeHop / 4;          // the shape is looked at every 128 frames
    constexpr int pad = 2;                      // frames of fade each side
    const auto from = juce::jmax ((juce::int64) 0, (juce::int64) std::floor (span.getStart()));
    const auto to = juce::jmin (numFrames, (juce::int64) std::ceil (span.getEnd()));

    // A frame is in wherever its window meets the shape: frames centred up to half a window
    // either side of it.
    const auto first = (juce::int64) std::floor ((double) (from - shapeSize / 2) / shapeHop);
    const auto last = (juce::int64) std::ceil ((double) (to + shapeSize / 2) / shapeHop);
    mask.firstFrame = first - pad;
    std::vector<std::vector<float>> rows ((size_t) (last - first + 1 + 2 * pad), std::vector<float> ((size_t) bins, 0.0f));

    std::vector<double> binOctave ((size_t) bins, -1.0e9);
    const auto binHz = sampleRate / shapeSize;

    for (int b = 1; b < bins; ++b)
        binOctave[(size_t) b] = std::log2 (b * binHz);

    for (auto k = first; k <= last; ++k)
    {
        const auto centre = (double) (k * shapeHop);
        std::vector<juce::Range<double>> inside;

        for (int o = -shapeSize / 2; o <= shapeSize / 2; o += step)
        {
            const auto t = centre + o;

            if (t >= (double) from && t <= (double) to)
                for (auto r : shape.octavesAt (t))
                    inside.push_back (r);
        }

        inside = merged (std::move (inside));
        auto& row = rows[(size_t) (k - mask.firstFrame)];

        for (auto r : inside)
            for (int b = 1; b < bins; ++b)
                if (binOctave[(size_t) b] >= r.getStart() && binOctave[(size_t) b] <= r.getEnd())
                    row[(size_t) b] = 1.0f;

        // A shape narrower than a bin still takes the bin nearest it.
        for (auto r : inside)
        {
            const auto b = juce::jlimit (1, bins - 1, (int) std::round (std::exp2 (0.5 * (r.getStart() + r.getEnd())) / binHz));
            row[(size_t) b] = 1.0f;
        }
    }

    // Soften the edges outward: [1 4 6 4 1] / 16 across bins and across frames, kept at least the
    // hard mask, so everything drawn is fully in and the fade lies outside it.
    auto soft = rows;
    const float kernel[] { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

    for (auto& row : soft)
    {
        const auto copy = row;

        for (int b = 0; b < bins; ++b)
        {
            float sum = 0.0f;

            for (int j = -2; j <= 2; ++j)
                sum += kernel[j + 2] * copy[(size_t) juce::jlimit (0, bins - 1, b + j)];

            row[(size_t) b] = sum;
        }
    }

    const auto across = soft;

    for (size_t k = 0; k < soft.size(); ++k)
        for (int b = 0; b < bins; ++b)
        {
            float sum = 0.0f;

            for (int j = -2; j <= 2; ++j)
            {
                const auto r = (int) k + j;

                if (r >= 0 && r < (int) across.size())
                    sum += kernel[j + 2] * across[(size_t) r][(size_t) b];
            }

            soft[k][(size_t) b] = juce::jmax (sum, rows[k][(size_t) b]);
        }

    // Trim frames with nothing in them from the ends.
    const auto empty = [] (const std::vector<float>& row) { return std::all_of (row.begin(), row.end(), [] (float w) { return w <= 0.0f; }); };
    size_t lo = 0, hi = soft.size();

    while (lo < hi && empty (soft[lo]))
        ++lo;

    while (hi > lo && empty (soft[hi - 1]))
        --hi;

    mask.firstFrame += (juce::int64) lo;
    mask.rows.assign (soft.begin() + (std::ptrdiff_t) lo, soft.begin() + (std::ptrdiff_t) hi);
    return mask;
}

namespace
{
    constexpr int refFrames = 4;   // frames each side that healing takes the level from

    /** Runs the masked frames through the transform: op changes the spectra (frames first - margin
        onwards, JUCE's interleaved real/imaginary layout) where the mask has weight (or in every
        frame of the mask's span, with everyFrame), and the audio under the masked frames is put
        back from them. */
    AudioDocument::Snapshot processMasked (const AudioSnapshot& snapshot, const Mask& mask, double sampleRate,
                                           const std::function<void (std::vector<std::vector<float>>&, juce::int64 firstSpectrum)>& op,
                                           bool everyFrame = false)
    {
        if (mask.rows.empty())
            return nullptr;

        const auto length = snapshot.getNumFrames();
        const auto first = mask.firstFrame, last = mask.firstFrame + (juce::int64) mask.rows.size() - 1;
        const auto k0 = first - 2 * refFrames, k1 = last + 2 * refFrames;
        const auto workStart = k0 * shapeHop - shapeSize / 2;
        const auto workLength = (int) ((k1 - k0) * shapeHop + shapeSize);

        juce::AudioBuffer<float> work (snapshot.getNumChannels(), workLength);
        snapshot.read (work, 0, workStart, workLength);
        juce::AudioBuffer<float> out (snapshot.getNumChannels(), workLength);
        out.clear();

        constexpr int order = 11;
        static_assert ((1 << order) == shapeSize);
        juce::dsp::FFT fft (order);
        std::vector<float> window ((size_t) shapeSize);

        for (int i = 0; i < shapeSize; ++i)
            window[(size_t) i] = (float) std::sqrt (0.5 * (1.0 - std::cos (juce::MathConstants<double>::twoPi * i / shapeSize)));

        std::vector<float> probe ((size_t) shapeSize * 2, 0.0f);
        probe[0] = 1.0f;
        fft.performRealOnlyForwardTransform (probe.data(), true);
        fft.performRealOnlyInverseTransform (probe.data());
        const auto roundTrip = probe[0];
        const auto scale = 1.0f / (2.0f * roundTrip);

        const auto masked = [&mask, first, last, everyFrame] (juce::int64 k)
        {
            if (k < first || k > last)
                return false;

            if (everyFrame)
                return true;

            const auto& row = mask.rows[(size_t) (k - first)];
            return std::any_of (row.begin(), row.end(), [] (float w) { return w > 0.0f; });
        };

        for (int ch = 0; ch < work.getNumChannels(); ++ch)
        {
            const auto* in = work.getReadPointer (ch);
            auto* dest = out.getWritePointer (ch);
            std::vector<std::vector<float>> spectra ((size_t) (k1 - k0 + 1));

            for (auto k = k0; k <= k1; ++k)
            {
                auto& data = spectra[(size_t) (k - k0)];
                data.assign ((size_t) shapeSize * 2, 0.0f);
                const auto p = (int) ((k - k0) * shapeHop);

                for (int i = 0; i < shapeSize; ++i)
                    data[(size_t) i] = in[p + i] * window[(size_t) i];

                fft.performRealOnlyForwardTransform (data.data(), true);
            }

            op (spectra, k0);

            for (auto k = k0; k <= k1; ++k)
            {
                const auto p = (int) ((k - k0) * shapeHop);

                if (masked (k))
                {
                    auto& data = spectra[(size_t) (k - k0)];
                    fft.performRealOnlyInverseTransform (data.data());

                    for (int i = 0; i < shapeSize; ++i)
                        dest[p + i] += data[(size_t) i] * window[(size_t) i] * scale;
                }
                else
                {
                    for (int i = 0; i < shapeSize; ++i)
                        dest[p + i] += in[p + i] * window[(size_t) i] * roundTrip * window[(size_t) i] * scale;
                }
            }
        }

        // Put back only the audio under the masked frames' windows.
        const auto replaceStart = juce::jlimit ((juce::int64) 0, length, first * shapeHop - shapeSize / 2);
        const auto replaceEnd = juce::jlimit (replaceStart, length, last * shapeHop + shapeSize / 2);

        if (replaceEnd <= replaceStart)
            return nullptr;

        juce::AudioBuffer<float> result (out.getNumChannels(), (int) (replaceEnd - replaceStart));

        for (int ch = 0; ch < out.getNumChannels(); ++ch)
            result.copyFrom (ch, 0, out, ch, (int) (replaceStart - workStart), result.getNumSamples());

        AudioClip clip { snapshot.getNumChannels(), sampleRate, AudioEdits::fromBuffer (std::move (result))->getSegments() };
        return AudioEdits::replaceWithClip (snapshot, replaceStart, replaceEnd, clip);
    }

    float weightAt (const Mask& mask, juce::int64 k, int bin)
    {
        const auto r = k - mask.firstFrame;
        return r >= 0 && r < (juce::int64) mask.rows.size() ? mask.rows[(size_t) r][(size_t) bin] : 0.0f;
    }
}

AudioDocument::Snapshot changeShape (const AudioSnapshot& snapshot, const Shape& shape, double sampleRate, float gainDb)
{
    const auto mask = maskFor (shape, sampleRate, snapshot.getNumFrames());
    const auto gain = (double) juce::Decibels::decibelsToGain (gainDb, -300.0f);

    return processMasked (snapshot, mask, sampleRate, [&] (std::vector<std::vector<float>>& spectra, juce::int64 k0)
    {
        for (size_t i = 0; i < spectra.size(); ++i)
            for (int b = 0; b <= shapeSize / 2; ++b)
                if (const auto w = weightAt (mask, k0 + (juce::int64) i, b); w > 0.0f)
                {
                    const auto g = (float) (1.0 + (gain - 1.0) * w);
                    spectra[i][(size_t) (2 * b)] *= g;
                    spectra[i][(size_t) (2 * b + 1)] *= g;
                }
    });
}

AudioDocument::Snapshot changeOutsideShape (const AudioSnapshot& snapshot, const Shape& shape, double sampleRate, float gainDb)
{
    const auto mask = maskFor (shape, sampleRate, snapshot.getNumFrames());
    const auto gain = (double) juce::Decibels::decibelsToGain (gainDb, -300.0f);
    const auto first = mask.firstFrame, last = mask.firstFrame + (juce::int64) mask.rows.size() - 1;

    return processMasked (snapshot, mask, sampleRate, [&] (std::vector<std::vector<float>>& spectra, juce::int64 k0)
    {
        for (size_t i = 0; i < spectra.size(); ++i)
        {
            const auto k = k0 + (juce::int64) i;

            if (k < first || k > last)
                continue;   // beyond the shape's span: left as it is

            for (int b = 0; b <= shapeSize / 2; ++b)
            {
                // The target where the mask has no weight, one where it is full.
                const auto g = (float) (gain + (1.0 - gain) * weightAt (mask, k, b));
                spectra[i][(size_t) (2 * b)] *= g;
                spectra[i][(size_t) (2 * b + 1)] *= g;
            }
        }
    }, true);
}

AudioDocument::Snapshot heal (const AudioSnapshot& snapshot, const Shape& shape, double sampleRate)
{
    const auto mask = maskFor (shape, sampleRate, snapshot.getNumFrames());

    return processMasked (snapshot, mask, sampleRate, [&] (std::vector<std::vector<float>>& spectra, juce::int64 k0)
    {
        const auto n = (int) spectra.size();
        std::vector<std::vector<double>> targets ((size_t) n, std::vector<double> ((size_t) shapeSize / 2 + 1, -1.0));   // -1: left as it is
        const auto original = spectra;   // read from, as healing one bin changes cells the next one's analysis looks at
        const auto magnitude = [&original] (int i, int b)
        {
            return std::hypot ((double) original[(size_t) i][(size_t) (2 * b)], (double) original[(size_t) i][(size_t) (2 * b + 1)]);
        };
        const auto phaseAt = [&original] (int i, int b)
        {
            return std::arg (std::complex<double> (original[(size_t) i][(size_t) (2 * b)], original[(size_t) i][(size_t) (2 * b + 1)]));
        };

        for (int b = 0; b <= shapeSize / 2; ++b)
        {
            for (int i = 0; i < n;)
            {
                if (weightAt (mask, k0 + i, b) <= 0.0f)
                {
                    ++i;
                    continue;
                }

                int j = i;

                while (j < n && weightAt (mask, k0 + j, b) > 0.0f)
                    ++j;

                // The level each side: the RMS magnitude of up to four frames outside the shape
                // (a plain mean of magnitudes would read noise about 1 dB low).
                const auto side = [&] (int from, int direction)
                {
                    double sum = 0.0;
                    int count = 0;

                    for (int r = from; r >= 0 && r < n && count < refFrames && weightAt (mask, k0 + r, b) <= 0.0f; r += direction, ++count)
                        sum += juce::square (magnitude (r, b));

                    return count > 0 ? std::optional<double> (std::sqrt (sum / count)) : std::nullopt;
                };

                auto before = side (i - 1, -1), after = side (j, 1);

                if (! before) before = after;
                if (! after) after = before;

                if (before)
                {
                    const auto lo = std::log (juce::jmax (*before, 1.0e-12)), hi = std::log (juce::jmax (*after, 1.0e-12));

                    // The phase carries on from the frame before the shape at the rate it was
                    // turning there (or back from the frame after it, at the file's start), so a
                    // steady tone runs on through and the frames stay in step with each other.
                    const auto fromBefore = i > 0 && weightAt (mask, k0 + i - 1, b) <= 0.0f;
                    const auto anchor = fromBefore ? i - 1 : j;

                    // Each bin turns at the rate of the peak it lies under in the anchor frame
                    // (found by climbing to the nearest local maximum), so the bins around a peak
                    // keep their phases to each other and stay one sinusoid from frame to frame.
                    auto peak = b;

                    if (anchor < n)
                        for (;;)
                        {
                            const auto here = magnitude (anchor, peak);
                            const auto down = peak > 0 ? magnitude (anchor, peak - 1) : -1.0;
                            const auto up = peak < shapeSize / 2 ? magnitude (anchor, peak + 1) : -1.0;

                            if (up > here && up >= down)       ++peak;
                            else if (down > here)              --peak;
                            else                               break;
                        }

                    const auto peakExpected = juce::MathConstants<double>::twoPi * peak * shapeHop / shapeSize;
                    const auto peakTurn = [&] (int f, int g)
                    {
                        if (f < 0 || f >= n || g < 0 || g >= n)
                            return peakExpected;

                        return peakExpected + std::remainder (phaseAt (g, peak) - phaseAt (f, peak) - peakExpected, juce::MathConstants<double>::twoPi);
                    };
                    const auto rate = fromBefore ? peakTurn (i - 2, i - 1) : peakTurn (j, j + 1);
                    const auto anchorPhase = anchor < n ? phaseAt (anchor, b) : 0.0;

                    for (int r = i; r < j; ++r)
                    {
                        const auto f = (double) (r - i + 1) / (double) (j - i + 1);
                        const auto target = std::exp (lo + (hi - lo) * f);
                        const auto w = (double) weightAt (mask, k0 + r, b);
                        const auto phase = anchorPhase + rate * (r - anchor);
                        auto& re = spectra[(size_t) r][(size_t) (2 * b)];
                        auto& im = spectra[(size_t) r][(size_t) (2 * b + 1)];
                        const auto level = (1.0 - w) * magnitude (r, b) + w * target;
                        re = (float) (level * std::cos (phase));
                        im = (float) (level * std::sin (phase));
                        targets[(size_t) r][(size_t) b] = level;
                    }
                }

                i = j;
            }
        }

        // Sixteen rounds of Griffin and Lim's method on the healed frames: put the frames back
        // together, look again, and keep the levels asked for with the phases found. Each round
        // brings the overlapping frames closer to agreeing, so the level comes out as asked (a
        // frame's noise copied forward alone falls about 4 dB short where the windows overlap).
        constexpr int order = 11, rounds = 16;
        juce::dsp::FFT fft (order);
        std::vector<float> window ((size_t) shapeSize), data ((size_t) shapeSize * 2);

        for (int x = 0; x < shapeSize; ++x)
            window[(size_t) x] = (float) std::sqrt (0.5 * (1.0 - std::cos (juce::MathConstants<double>::twoPi * x / shapeSize)));

        std::fill (data.begin(), data.end(), 0.0f);
        data[0] = 1.0f;
        fft.performRealOnlyForwardTransform (data.data(), true);
        fft.performRealOnlyInverseTransform (data.data());
        const auto scale = 1.0f / (2.0f * data[0]);

        std::vector<int> healed;

        for (int r = 0; r < n; ++r)
            if (std::any_of (targets[(size_t) r].begin(), targets[(size_t) r].end(), [] (double t) { return t >= 0.0; }))
                healed.push_back (r);

        std::vector<float> audio ((size_t) ((n - 1) * shapeHop + shapeSize));

        for (int round = 0; round < rounds && ! healed.empty(); ++round)
        {
            std::fill (audio.begin(), audio.end(), 0.0f);

            for (int r = 0; r < n; ++r)
            {
                std::copy (spectra[(size_t) r].begin(), spectra[(size_t) r].end(), data.begin());
                fft.performRealOnlyInverseTransform (data.data());

                for (int x = 0; x < shapeSize; ++x)
                    audio[(size_t) (r * shapeHop + x)] += data[(size_t) x] * window[(size_t) x] * scale;
            }

            for (auto r : healed)
            {
                std::fill (data.begin(), data.end(), 0.0f);

                for (int x = 0; x < shapeSize; ++x)
                    data[(size_t) x] = audio[(size_t) (r * shapeHop + x)] * window[(size_t) x];

                fft.performRealOnlyForwardTransform (data.data(), true);

                for (int b = 0; b <= shapeSize / 2; ++b)
                {
                    const auto t = targets[(size_t) r][(size_t) b];

                    if (t < 0.0)
                        continue;

                    const auto re = (double) data[(size_t) (2 * b)], im = (double) data[(size_t) (2 * b + 1)];
                    const auto m = std::hypot (re, im);
                    spectra[(size_t) r][(size_t) (2 * b)] = (float) (m > 1.0e-12 ? t * re / m : t);
                    spectra[(size_t) r][(size_t) (2 * b + 1)] = (float) (m > 1.0e-12 ? t * im / m : 0.0);
                }
            }
        }
    });
}
}
