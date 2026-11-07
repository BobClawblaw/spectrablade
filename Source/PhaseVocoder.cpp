#include "PhaseVocoder.h"

namespace PhaseVocoder
{
namespace
{
    using Complex = std::complex<float>;

    constexpr double frameSeconds = 0.085;
    constexpr int overlap = 4;
    constexpr double spanFrames = 0.5;     // played at its own speed either side of an attack, in frames
    constexpr double minFlux = 2.0;        // mean rise per bin (dB) for an attack
    constexpr double fluxFloorDb = -50.0;  // bins lower than this under the frame's loudest count as this low

    double principal (double phase)
    {
        return phase - juce::MathConstants<double>::twoPi * std::floor ((phase + juce::MathConstants<double>::pi) / juce::MathConstants<double>::twoPi);
    }

    std::vector<float> hann (int n)
    {
        std::vector<float> w ((size_t) n);

        for (int i = 0; i < n; ++i)
            w[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) n);

        return w;
    }

    /** One windowed frame of a channel starting at `start` (silence outside it), transformed:
        n / 2 + 1 bins. */
    void transform (juce::dsp::FFT& fft, const float* channel, int length, int start, const std::vector<float>& window,
                    std::vector<float>& work, std::vector<Complex>& bins)
    {
        const auto n = (int) window.size();
        std::fill (work.begin(), work.end(), 0.0f);

        for (int i = 0; i < n; ++i)
        {
            const auto j = start + i;

            if (j >= 0 && j < length)
                work[(size_t) i] = channel[j] * window[(size_t) i];
        }

        fft.performRealOnlyForwardTransform (work.data(), true);

        for (int b = 0; b <= n / 2; ++b)
            bins[(size_t) b] = { work[(size_t) (2 * b)], work[(size_t) (2 * b + 1)] };
    }

    /** The input with `pad` samples either side carried on through each end sample (2 x[0] - x[i]
        before it, likewise after), as TimeStretch pads for Signalsmith Stretch. */
    juce::AudioBuffer<float> reflected (const juce::AudioBuffer<float>& input, int pad)
    {
        const auto length = input.getNumSamples();
        juce::AudioBuffer<float> padded (input.getNumChannels(), length + 2 * pad);

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
        {
            const auto* in = input.getReadPointer (ch);
            auto* out = padded.getWritePointer (ch);
            std::copy (in, in + length, out + pad);

            for (int i = 1; i <= pad; ++i)
            {
                out[pad - i] = 2.0f * in[0] - in[juce::jmin (i, length - 1)];
                out[pad + length - 1 + i] = 2.0f * in[length - 1] - in[juce::jmax (length - 1 - i, 0)];
            }
        }

        return padded;
    }

    /** Input positions for output positions: straight lines between points, 1:1 past the ends. */
    struct TimeMap
    {
        struct Point { double in, out; };
        std::vector<Point> points;

        double inputAt (double out) const
        {
            if (out <= points.front().out)
                return points.front().in + (out - points.front().out);

            if (out >= points.back().out)
                return points.back().in + (out - points.back().out);

            const auto next = std::upper_bound (points.begin(), points.end(), out, [] (double v, const Point& p) { return v < p.out; });
            const auto& b = *next;
            const auto& a = *(next - 1);
            return b.out > a.out ? a.in + (out - a.out) * (b.in - a.in) / (b.out - a.out) : a.in;
        }
    };

    /** A stretch of the input played at its own speed round an attack at `at`. */
    struct Span { double from, to, at; };
}

std::vector<int> onsets (const juce::AudioBuffer<float>& input, int from, int to, double sampleRate)
{
    constexpr int order = 10, n = 1 << order, hop = n / 4;
    juce::dsp::FFT fft (order);
    const auto window = hann (n);
    std::vector<float> work ((size_t) (2 * n));
    std::vector<Complex> bins ((size_t) (n / 2 + 1));
    std::vector<double> previous ((size_t) (n / 2 + 1), 0.0), flux;
    std::vector<int> centres;
    const auto length = input.getNumSamples();
    from = juce::jlimit (0, length, from);
    to = juce::jlimit (from, length, to);

    // The spectral flux: how far each bin's level rose since the frame before, in dB (from no
    // lower than fluxFloorDb under the loudest, so quiet bins between notes, swinging as the notes
    // beat, don't count), each at most 30 dB, averaged over the bins.
    for (int start = from - n / 2; start + n / 2 < to; start += hop)
    {
        std::vector<double> level ((size_t) (n / 2 + 1), 0.0);

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
        {
            transform (fft, input.getReadPointer (ch), length, start, window, work, bins);

            for (size_t b = 0; b < bins.size(); ++b)
                level[b] += std::abs (bins[b]);
        }

        auto loudest = 1.0e-9;

        for (size_t b = 1; b < level.size(); ++b)
            loudest = juce::jmax (loudest, level[b], previous[b]);

        const auto floor = juce::jmax (loudest * juce::Decibels::decibelsToGain (fluxFloorDb), 1.0e-6);
        auto rise = 0.0;

        for (size_t b = 1; b < level.size(); ++b)
        {
            const auto change = 20.0 * std::log10 (juce::jmax (level[b], floor) / juce::jmax (previous[b], floor));
            rise += juce::jlimit (0.0, 30.0, change);
        }

        flux.push_back (rise / (double) (n / 2));
        centres.push_back (start + n / 2);
        previous = std::move (level);
    }

    // Peaks of the flux, over 1.5 times its mean for 100 ms either side and over minFlux; each
    // placed at the steepest rise of a 1 ms envelope near its frame, at least 50 ms after the last.
    std::vector<int> found;
    const auto around = (int) std::lround (0.1 * sampleRate / hop);
    const auto smooth = juce::jmax (1, (int) std::lround (0.001 * sampleRate));
    const auto count = (int) flux.size();

    for (int j = 1; j + 1 < count; ++j)
    {
        auto mean = 0.0;
        auto counted = 0;

        for (int q = juce::jmax (0, j - around); q <= juce::jmin (count - 1, j + around); ++q, ++counted)
            mean += flux[(size_t) q];

        mean /= counted;
        auto isPeak = true;

        for (int q = juce::jmax (0, j - 3); q <= juce::jmin (count - 1, j + 3) && isPeak; ++q)
            isPeak = flux[(size_t) q] <= flux[(size_t) j];

        if (! isPeak || flux[(size_t) j] < 1.5 * mean || flux[(size_t) j] < minFlux)
            continue;

        const auto lo = juce::jmax (from, centres[(size_t) j] - n / 2), hi = juce::jmin (to - 1, centres[(size_t) j] + n / 2);
        std::vector<double> envelope ((size_t) (hi - lo + 1), 0.0);

        for (int i = lo; i <= hi; ++i)
            for (int ch = 0; ch < input.getNumChannels(); ++ch)
                envelope[(size_t) (i - lo)] += std::abs (input.getSample (ch, i));

        std::vector<double> smoothed (envelope.size());
        auto running = 0.0;

        for (size_t i = 0; i < envelope.size(); ++i)
        {
            running += envelope[i];

            if (i >= (size_t) smooth)
                running -= envelope[i - (size_t) smooth];

            smoothed[i] = running / smooth;
        }

        auto steepest = -1.0;
        auto at = centres[(size_t) j];

        for (size_t i = (size_t) smooth; i < smoothed.size(); ++i)
        {
            const auto rise = smoothed[i] - smoothed[i - (size_t) smooth];

            if (rise > steepest)
            {
                steepest = rise;
                at = lo + (int) i - smooth;
            }
        }

        // Back to where the rise starts: the first sample, from 2 ms before the steepest point, whose
        // level is a tenth of the way from the lowest there (smoothed) to the loudest in the 2 ms after.
        const auto steep = at - lo;
        auto lowest = std::numeric_limits<double>::max(), loudest = 0.0;

        for (int i = juce::jmax (0, steep - 2 * smooth); i <= steep; ++i)
            lowest = juce::jmin (lowest, smoothed[(size_t) i]);

        for (int i = steep; i <= juce::jmin ((int) envelope.size() - 1, steep + 2 * smooth); ++i)
            loudest = juce::jmax (loudest, envelope[(size_t) i]);

        const auto threshold = lowest + 0.1 * (loudest - lowest);

        for (int i = juce::jmax (0, steep - 2 * smooth); i <= juce::jmin ((int) envelope.size() - 1, steep + 2 * smooth); ++i)
        {
            if (envelope[(size_t) i] > threshold)
            {
                at = lo + i;
                break;
            }
        }

        if (found.empty() || at - found.back() > (int) (0.05 * sampleRate))
            found.push_back (at);
    }

    return found;
}

juce::AudioBuffer<float> stretch (const juce::AudioBuffer<float>& input, double ratio, double sampleRate,
                                  const std::function<bool (float)>& progress)
{
    ratio = juce::jlimit (0.25, 4.0, ratio);
    const auto channels = input.getNumChannels();
    const auto length = input.getNumSamples();
    const auto outLength = (int) std::llround ((double) length * ratio);
    juce::AudioBuffer<float> result (channels, outLength);
    result.clear();

    if (length == 0 || outLength == 0 || channels == 0)
        return result;

    const auto order = juce::jlimit (8, 15, (int) std::lround (std::log2 (sampleRate * frameSeconds)));
    const auto n = 1 << order;
    const auto hop = n / overlap;
    const auto pad = n;
    const auto x = reflected (input, pad);
    const auto total = x.getNumSamples();

    // The time map. Each attack T stays where the stretch puts it, and up to half a frame either
    // side of it plays 1:1, no nearer the last span than leaves the audio between them a ratio
    // from 1/4 to 4; between spans the audio stretches to fit. The padding plays 1:1.
    const auto place = [&] (double t) { return pad + (t - pad) * ratio; };
    const auto reach = spanFrames * n;
    std::vector<Span> spans;

    for (auto t : onsets (x, pad, pad + length, sampleRate))
    {
        const auto inFrom = spans.empty() ? (double) pad : spans.back().to;
        const auto outFrom = spans.empty() ? (double) pad : place (spans.back().at) + (spans.back().to - spans.back().at);
        const auto gapIn = t - inFrom, gapOut = place (t) - outFrom;
        const auto fits = [&] (double before) { return gapOut - before <= 4.0 * (gapIn - before) && gapOut - before >= 0.25 * (gapIn - before); };
        auto before = juce::jmin (reach, juce::jmax (0.0, juce::jmin (gapIn, gapOut)));

        while (before > 0.0 && ! fits (before))
            before -= 16.0;

        before = juce::jmax (0.0, before);

        if (! fits (before))
            continue;

        const auto after = juce::jmin (reach, (double) (pad + length - t), pad + length * ratio - place (t));
        spans.push_back ({ t - before, t + juce::jmax (0.0, after), (double) t });
    }

    // A span reaching past where the next one starts (in the input or the output) ends there.
    for (size_t i = 0; i + 1 < spans.size(); ++i)
    {
        auto& s = spans[i];
        const auto& next = spans[i + 1];
        const auto nextOut = place (next.at) - (next.at - next.from);
        s.to = juce::jmin (s.to, next.from, s.at + (nextOut - place (s.at)));
        s.to = juce::jmax (s.to, s.at);
    }

    TimeMap map;
    map.points = { { 0.0, 0.0 }, { (double) pad, (double) pad } };

    for (auto& s : spans)
    {
        map.points.push_back ({ s.from, place (s.at) - (s.at - s.from) });
        map.points.push_back ({ s.to, place (s.at) + (s.to - s.at) });
    }

    map.points.push_back ({ (double) (pad + length), pad + length * ratio });
    map.points.push_back ({ (double) total, pad + length * ratio + pad });
    const auto outTotal = (int) std::ceil (pad + length * ratio + pad);

    const auto spanOf = [&spans] (double at)
    {
        for (size_t i = 0; i < spans.size(); ++i)
            if (at >= spans[i].from && at < spans[i].to)
                return (int) i;

        return -1;
    };

    juce::dsp::FFT fft (order);
    const auto window = hann (n);
    const auto bins = n / 2 + 1;
    std::vector<float> work ((size_t) (2 * n));
    std::vector<std::vector<Complex>> now ((size_t) channels, std::vector<Complex> ((size_t) bins)), before = now;
    std::vector<double> turn ((size_t) bins, 0.0), next ((size_t) bins), level ((size_t) bins), speed ((size_t) bins);
    std::vector<int> peaks, owner ((size_t) bins);
    juce::AudioBuffer<float> y (channels, outTotal);
    y.clear();
    std::vector<double> weight ((size_t) outTotal, 0.0);
    auto lastSpan = -1, previousStart = 0;
    const auto frames = (outTotal + n) / hop + 1;

    for (int k = 0;; ++k)
    {
        const auto outStart = k * hop - n;

        if (outStart >= outTotal)
            break;

        if (progress != nullptr && k % 32 == 0 && ! progress ((float) k / (float) frames))
            return {};

        const auto inStart = (int) std::lround (map.inputAt (outStart + n / 2.0) - n / 2.0);
        const auto inHop = k == 0 ? hop : inStart - previousStart;

        // Each bin's speed, from the turn of every channel's phase over one hop (weighted by level).
        for (int ch = 0; ch < channels; ++ch)
        {
            transform (fft, x.getReadPointer (ch), total, inStart, window, work, now[(size_t) ch]);
            transform (fft, x.getReadPointer (ch), total, inStart - hop, window, work, before[(size_t) ch]);
        }

        for (int b = 0; b < bins; ++b)
        {
            std::complex<double> across;
            auto sum = 0.0;

            for (int ch = 0; ch < channels; ++ch)
            {
                const auto a = std::complex<double> (now[(size_t) ch][(size_t) b]), p = std::complex<double> (before[(size_t) ch][(size_t) b]);
                across += a * std::conj (p);
                sum += std::norm (a);
            }

            level[(size_t) b] = sum;
            const auto nominal = juce::MathConstants<double>::twoPi * b / n;
            speed[(size_t) b] = nominal + principal (std::arg (across) - nominal * hop) / hop;
        }

        const auto span = spanOf (inStart + n / 2.0);

        if (span >= 0 && span != lastSpan)
        {
            // Into an attack's span: the input's own phases, so it comes out as it went in.
            std::fill (turn.begin(), turn.end(), 0.0);
        }
        else if (k > 0)
        {
            // Each peak's turn moves on by its speed times how much further the output went than
            // the input; the bins round each peak (to the lowest bin between it and the next) take its turn.
            for (int b = 0; b < bins; ++b)
                next[(size_t) b] = turn[(size_t) b] + speed[(size_t) b] * (hop - inHop);

            peaks.clear();

            for (int b = 2; b < bins - 2; ++b)
                if (level[(size_t) b] > level[(size_t) b - 1] && level[(size_t) b] >= level[(size_t) b + 1]
                    && level[(size_t) b] > level[(size_t) b - 2] && level[(size_t) b] >= level[(size_t) b + 2])
                    peaks.push_back (b);

            if (peaks.empty())
            {
                turn = next;
            }
            else
            {
                auto b = 0;

                for (size_t p = 0; p < peaks.size(); ++p)
                {
                    auto end = bins - 1;

                    if (p + 1 < peaks.size())
                    {
                        end = peaks[p];

                        for (int q = peaks[p]; q <= peaks[p + 1]; ++q)
                            if (level[(size_t) q] < level[(size_t) end])
                                end = q;
                    }

                    for (; b <= end; ++b)
                        owner[(size_t) b] = peaks[p];
                }

                for (b = 0; b < bins; ++b)
                    turn[(size_t) b] = next[(size_t) owner[(size_t) b]];
            }
        }

        lastSpan = span;
        previousStart = inStart;

        for (int ch = 0; ch < channels; ++ch)
        {
            const auto& spectrum = now[(size_t) ch];

            for (int b = 0; b < bins; ++b)
            {
                const auto turned = spectrum[(size_t) b] * std::polar (1.0f, (float) turn[(size_t) b]);
                work[(size_t) (2 * b)] = turned.real();
                work[(size_t) (2 * b + 1)] = turned.imag();
            }

            fft.performRealOnlyInverseTransform (work.data());
            auto* out = y.getWritePointer (ch);

            for (int i = 0; i < n; ++i)
            {
                const auto t = outStart + i;

                if (t < 0 || t >= outTotal)
                    continue;

                out[t] += work[(size_t) i] * window[(size_t) i];

                if (ch == 0)
                    weight[(size_t) t] += (double) window[(size_t) i] * window[(size_t) i];
            }
        }
    }

    for (int ch = 0; ch < channels; ++ch)
    {
        const auto* from = y.getReadPointer (ch);
        auto* to = result.getWritePointer (ch);

        for (int i = 0; i < outLength; ++i)
        {
            const auto w = weight[(size_t) (pad + i)];
            to[i] = w > 1.0e-6 ? (float) (from[pad + i] / w) : 0.0f;
        }
    }

    return result;
}
}
