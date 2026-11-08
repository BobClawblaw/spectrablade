#include "WowFlutter.h"
#include "AudioEdits.h"

namespace WowFlutter
{
namespace
{
    constexpr int fftOrder = 12, size = 1 << fftOrder, hop = size / 4, bins = size / 2 + 1;
    constexpr double matchWithin = 0.03, peakFloorDb = -60.0;

    struct Peak { double frequency, power; };

    /** The spectral peaks of a frame (bins over both neighbours and over the floor), each placed
        between bins by the parabola through it and its neighbours (in log power). */
    std::vector<Peak> peaksOf (const std::vector<float>& power, double binHz)
    {
        std::vector<Peak> peaks;
        const auto floor = (float) std::pow (10.0, peakFloorDb / 10.0);

        for (int k = 2; k < bins - 2; ++k)
        {
            const auto p = power[(size_t) k];

            if (p < floor || p <= power[(size_t) k - 1] || p < power[(size_t) k + 1])
                continue;

            const auto a = std::log (juce::jmax (1.0e-20f, power[(size_t) k - 1])), b = std::log (p), c = std::log (juce::jmax (1.0e-20f, power[(size_t) k + 1]));
            const auto denominator = a - 2.0 * b + c;
            const auto offset = std::abs (denominator) > 1.0e-12 ? 0.5 * (a - c) / denominator : 0.0;
            peaks.push_back ({ (k + juce::jlimit (-0.5, 0.5, offset)) * binHz, (double) p });
        }

        return peaks;
    }

    /** Low-passes values (at frameRate) to cutoffHz: two second-order Butterworth sections run
        forwards and then back (zero phase), over the values extended by reflection at each end so
        the filter settles before the values start. */
    void smoothTo (std::vector<double>& values, double frameRate, double cutoffHz)
    {
        const auto n = (int) values.size();

        if (n < 3 || cutoffHz >= frameRate / 2.0)
            return;

        const auto pad = juce::jmin (n - 1, juce::jmax (8, (int) std::lround (2.0 * frameRate / cutoffHz)));
        std::vector<double> x ((size_t) (n + 2 * pad));

        for (int i = 0; i < n + 2 * pad; ++i)
        {
            const auto j = i - pad;
            x[(size_t) i] = values[(size_t) (j < 0 ? -j : j >= n ? 2 * (n - 1) - j : j)];
        }

        for (auto q : { 0.5412, 1.3066 })
        {
            const auto c = juce::dsp::IIR::Coefficients<double>::makeLowPass (frameRate, cutoffHz, q);
            const auto* k = c->getRawCoefficients();   // b0 b1 b2 a1 a2 (a0 normalised)

            for (auto pass = 0; pass < 2; ++pass)
            {
                auto x1 = x.front(), x2 = x.front(), y1 = x.front(), y2 = x.front();

                for (int i = 0; i < (int) x.size(); ++i)
                {
                    const auto at = pass == 0 ? i : (int) x.size() - 1 - i;
                    const auto x0 = x[(size_t) at];
                    const auto y0 = k[0] * x0 + k[1] * x1 + k[2] * x2 - k[3] * y1 - k[4] * y2;
                    x2 = x1; x1 = x0; y2 = y1; y1 = y0;
                    x[(size_t) at] = y0;
                }
            }
        }

        for (int i = 0; i < n; ++i)
            values[(size_t) i] = x[(size_t) (i + pad)];
    }
}

bool isValid (const Settings& s)
{
    return s.fastestHz >= 0.5 && s.fastestHz <= 20.0 && s.amountPercent >= 0.0 && s.amountPercent <= 100.0;
}

Drift measure (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings,
               const std::function<bool (float)>& progress)
{
    Drift d;
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (! isValid (settings) || sampleRate <= 0.0 || end - start < (juce::int64) (0.5 * sampleRate) || end - start > std::numeric_limits<int>::max() / 4)
        return d;

    const auto channels = s.getNumChannels();
    const auto frames = (int) (end - start);
    const auto steps = (frames - size) / hop + 1;
    d.hop = hop;
    juce::dsp::FFT fft (fftOrder);
    std::vector<float> data ((size_t) (2 * size)), power ((size_t) bins), hann ((size_t) size);
    juce::AudioBuffer<float> block (channels, size);

    for (int i = 0; i < size; ++i)
        hann[(size_t) i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / size));

    // Each frame's peaks, each on a track: the peak within 3 % of it in the frame before continues
    // that peak's track, any other begins one.
    struct Tracked { int track; double logFrequency, power; };
    std::vector<std::vector<Tracked>> tracked ((size_t) steps);
    std::vector<Peak> previous;
    std::vector<int> previousTracks;
    auto tracks = 0;
    const auto binHz = sampleRate / size;

    for (int step = 0; step < steps; ++step)
    {
        if (progress && (step % 16) == 0 && ! progress (0.6f * (float) step / (float) steps))
            return {};

        s.read (block, 0, start + (juce::int64) step * hop, size);
        std::fill (data.begin(), data.end(), 0.0f);

        for (int i = 0; i < size; ++i)
        {
            auto mix = 0.0f;

            for (int ch = 0; ch < channels; ++ch)
                mix += block.getSample (ch, i);

            data[(size_t) i] = mix / (float) channels * hann[(size_t) i];
        }

        fft.performRealOnlyForwardTransform (data.data(), true);

        for (int k = 0; k < bins; ++k)
            power[(size_t) k] = data[(size_t) (2 * k)] * data[(size_t) (2 * k)] + data[(size_t) (2 * k + 1)] * data[(size_t) (2 * k + 1)];

        auto peaks = peaksOf (power, binHz);
        std::vector<int> ids (peaks.size(), -1);
        std::vector<char> claimed (previous.size(), 0);
        auto& here = tracked[(size_t) step];

        for (size_t i = 0; i < peaks.size(); ++i)
        {
            auto nearest = -1;

            for (size_t j = 0; j < previous.size(); ++j)
                if (! claimed[j] && std::abs (previous[j].frequency - peaks[i].frequency) <= matchWithin * peaks[i].frequency
                    && (nearest < 0 || std::abs (previous[j].frequency - peaks[i].frequency) < std::abs (previous[(size_t) nearest].frequency - peaks[i].frequency)))
                    nearest = (int) j;

            if (nearest >= 0)
            {
                claimed[(size_t) nearest] = 1;
                ids[i] = previousTracks[(size_t) nearest];
            }
            else
                ids[i] = tracks++;

            here.push_back ({ ids[i], std::log (peaks[i].frequency), peaks[i].power });
        }

        previous = std::move (peaks);
        previousTracks = std::move (ids);
    }

    // Each track's members in order (a track runs over consecutive frames).
    std::vector<std::vector<std::pair<int, int>>> members ((size_t) tracks);   // frame, index in it

    for (int step = 0; step < steps; ++step)
        for (size_t i = 0; i < tracked[(size_t) step].size(); ++i)
            members[(size_t) tracked[(size_t) step][i].track].emplace_back (step, (int) i);

    // A peak's deviation: its log frequency less its track's median over the 3 s round it, where
    // the track has run at least 0.3 s (a note held, not a passing transient). Each frame's speed
    // is the power-weighted median of its peaks' deviations, taken only where the peaks on such
    // tracks agree as a machine's speed makes them (four fifths of their weight within 0.3 % of
    // the median) and three or more of those agreeing have held a second or longer (notes held
    // under a voice, where a voice alone, whose harmonics also move together, holds few): where
    // the partials disagree, or only a voice is moving, there is no reading, and the speed is left
    // at 1. Read this way, nothing is summed from frame to frame, so an error stays in its own
    // frame; and no reading goes past 3 %, as no machine's wow does.
    const auto frameRate = sampleRate / hop;
    const auto reach = juce::jmax (1, (int) std::lround (1.5 * frameRate)), shortest = juce::jmax (2, (int) std::lround (0.3 * frameRate));
    const auto held = juce::jmax (shortest, (int) std::lround (1.0 * frameRate));
    std::vector<std::vector<double>> deviations ((size_t) steps);
    std::vector<std::vector<char>> long_ ((size_t) steps);

    for (int step = 0; step < steps; ++step)
    {
        deviations[(size_t) step].assign (tracked[(size_t) step].size(), std::numeric_limits<double>::quiet_NaN());
        long_[(size_t) step].assign (tracked[(size_t) step].size(), 0);
    }

    std::vector<double> window;

    for (int t = 0; t < tracks; ++t)
    {
        const auto& m = members[(size_t) t];

        if ((int) m.size() < shortest)
            continue;

        if (progress && (t % 256) == 0 && ! progress (0.6f + 0.2f * (float) t / (float) tracks))
            return {};

        for (int j = 0; j < (int) m.size(); ++j)
        {
            // The 3 s round the member, slid along within the track at its ends so the span stays
            // whole (a wow's periods balance over a whole span, not over a part of one).
            const auto from = juce::jlimit (0, juce::jmax (0, (int) m.size() - 1 - 2 * reach), j - reach), to = juce::jmin ((int) m.size() - 1, from + 2 * reach);
            window.clear();

            for (int i = from; i <= to; ++i)
                window.push_back (tracked[(size_t) m[(size_t) i].first][(size_t) m[(size_t) i].second].logFrequency);

            std::nth_element (window.begin(), window.begin() + (long) window.size() / 2, window.end());
            const auto reference = window[window.size() / 2];
            deviations[(size_t) m[(size_t) j].first][(size_t) m[(size_t) j].second] = tracked[(size_t) m[(size_t) j].first][(size_t) m[(size_t) j].second].logFrequency - reference;
            long_[(size_t) m[(size_t) j].first][(size_t) m[(size_t) j].second] = (int) m.size() >= held;
        }
    }

    std::vector<double> logSpeed ((size_t) steps, 0.0);
    struct Reading { double deviation, power; bool held; };
    std::vector<Reading> readings;

    for (int step = 0; step < steps; ++step)
    {
        readings.clear();

        for (size_t i = 0; i < tracked[(size_t) step].size(); ++i)
            if (! std::isnan (deviations[(size_t) step][i]))
                readings.push_back ({ deviations[(size_t) step][i], tracked[(size_t) step][i].power, long_[(size_t) step][i] != 0 });

        if (readings.size() < 3)
            continue;

        std::sort (readings.begin(), readings.end(), [] (const Reading& x, const Reading& y) { return x.deviation < y.deviation; });
        auto total = 0.0;

        for (const auto& r : readings)
            total += r.power;

        auto sum = 0.0;
        auto middle = readings.front().deviation;

        for (const auto& r : readings)
        {
            sum += r.power;

            if (sum >= 0.5 * total)
            {
                middle = r.deviation;
                break;
            }
        }

        auto agreeing = 0.0;
        auto heldAgreeing = 0;

        for (const auto& r : readings)
            if (std::abs (r.deviation - middle) <= 0.003)
            {
                agreeing += r.power;
                heldAgreeing += r.held;
            }

        if (agreeing >= 0.8 * total && heldAgreeing >= 3)
            logSpeed[(size_t) step] = juce::jlimit (-0.03, 0.03, middle);
    }

    // Smoothed to the drift asked for (a fourth-order Butterworth low-pass at the fastest drift,
    // run forwards and back so nothing shifts in time, over the curve extended by reflection at
    // its ends), then centred on its mean, so the length stays.
    std::vector<double> smoothed = logSpeed;
    smoothTo (smoothed, frameRate, settings.fastestHz);

    auto mean = 0.0;

    for (auto v : smoothed)
        mean += v;

    mean /= steps;
    d.speed.resize ((size_t) steps);

    for (int i = 0; i < steps; ++i)
        d.speed[(size_t) i] = std::exp ((smoothed[(size_t) i] - mean) * settings.amountPercent / 100.0);

    return d;
}

AudioDocument::Snapshot correct (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings,
                                 const std::function<bool (float)>& progress)
{
    const auto drift = measure (s, start, end, sampleRate, settings, progress);

    if (drift.speed.empty())
        return nullptr;

    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    return AudioEdits::processRange (s, start, end, [&drift, &progress] (juce::AudioBuffer<float>& b)
    {
        // The speed at any frame: between the curve's points (each at its frame's centre), flat past
        // the ends. The audio is read at the inverse speed; the read position is scaled at the end
        // so the whole range is covered exactly.
        const auto frames = b.getNumSamples();
        const auto speedAt = [&drift] (double frame)
        {
            const auto position = (frame - drift.hop * 2.0) / drift.hop;   // the first frame's centre is at 2 hops (half a window)
            const auto i = juce::jlimit (0, (int) drift.speed.size() - 1, (int) std::floor (position));
            const auto j = juce::jmin ((int) drift.speed.size() - 1, i + 1);
            const auto t = juce::jlimit (0.0, 1.0, position - i);
            return drift.speed[(size_t) i] + t * (drift.speed[(size_t) j] - drift.speed[(size_t) i]);
        };

        std::vector<double> readAt ((size_t) frames + 1);
        readAt[0] = 0.0;

        for (int n = 0; n < frames; ++n)
            readAt[(size_t) n + 1] = readAt[(size_t) n] + 1.0 / speedAt (readAt[(size_t) n]);

        const auto scale = (double) frames / readAt[(size_t) frames];

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            const juce::AudioBuffer<float> source (b.getNumChannels(), frames);
            const_cast<juce::AudioBuffer<float>&> (source).copyFrom (ch, 0, b, ch, 0, frames);
            const auto* x = source.getReadPointer (ch);
            auto* y = b.getWritePointer (ch);
            const auto at = [x, frames] (int i) { return (double) x[juce::jlimit (0, frames - 1, i)]; };

            for (int n = 0; n < frames; ++n)
            {
                const auto p = readAt[(size_t) n] * scale;
                const auto i = (int) std::floor (p);
                const auto t = p - i;
                // Four-point (Catmull-Rom) interpolation between the samples round p.
                const auto a = at (i - 1), b0 = at (i), c = at (i + 1), d0 = at (i + 2);
                const auto v = b0 + 0.5 * t * (c - a + t * (2.0 * a - 5.0 * b0 + 4.0 * c - d0 + t * (3.0 * (b0 - c) + d0 - a)));
                y[n] = (float) v;
            }

            if (progress && ! progress (0.8f + 0.2f * (float) (ch + 1) / (float) b.getNumChannels()))
                return;
        }
    });
}
}
