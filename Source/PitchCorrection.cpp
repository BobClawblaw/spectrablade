#include "PitchCorrection.h"
#include "AudioEdits.h"

namespace PitchCorrection
{
namespace
{
    constexpr double hopSeconds = 0.005;
    constexpr double threshold = 0.15;          // YIN's dip that counts as a period
    constexpr double quietest = 1.0e-6;         // mean power below which there is no pitch (-60 dBFS)
    constexpr double contextSeconds = 0.05;     // audio either side of the range the work reaches into
    constexpr double easeSeconds = 0.01;
    constexpr double stickCents = 10.0;         // past halfway to the next note before a target changes
    constexpr int shortestRun = 3;              // frames: a voiced run shorter than this has no pitch

    /** The channels' mix over [from, from + n). */
    std::vector<float> mixOf (const AudioSnapshot& s, juce::int64 from, int n)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), n);
        s.read (b, 0, from, n);
        std::vector<float> mix ((size_t) n, 0.0f);

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            juce::FloatVectorOperations::add (mix.data(), b.getReadPointer (ch), n);

        juce::FloatVectorOperations::multiply (mix.data(), 1.0f / (float) juce::jmax (1, b.getNumChannels()), n);
        return mix;
    }
}

bool isValid (const Settings& s)
{
    return s.key >= 0 && s.key <= 11 && (s.scale == Scale::chromatic || s.scale == Scale::major || s.scale == Scale::minor)
           && std::isfinite (s.speedMs) && s.speedMs >= 0.0f && s.speedMs <= Settings::slowestMs
           && std::isfinite (s.strengthPercent) && s.strengthPercent >= 0.0f && s.strengthPercent <= 100.0f;
}

bool inScale (int note, const Settings& s)
{
    static constexpr bool major[12] { true, false, true, false, true, true, false, true, false, true, false, true };
    static constexpr bool minor[12] { true, false, true, true, false, true, false, true, true, false, true, false };
    const auto step = ((note - s.key) % 12 + 12) % 12;
    return s.scale == Scale::chromatic || (s.scale == Scale::major ? major[step] : minor[step]);
}

Track detect (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress)
{
    Track track;
    start = juce::jlimit ((juce::int64) 0, snapshot.getNumFrames(), start);
    end = juce::jlimit (start, snapshot.getNumFrames(), end);

    if (end <= start || sampleRate <= 0.0 || snapshot.getNumChannels() == 0)
        return track;

    track.first = start;
    track.hop = juce::jmax (1, (int) std::lround (hopSeconds * sampleRate));
    const auto minLag = juce::jmax (2, (int) std::floor (sampleRate / highestHz));
    const auto maxLag = (int) std::ceil (sampleRate / lowestHz);
    const auto window = maxLag, span = window + maxLag + 2;
    int order = 1;

    while ((1 << order) < span + window)
        ++order;

    const auto size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> a ((size_t) size * 2), b ((size_t) size * 2);

    // The FFT pair's own scale, measured.
    std::fill (a.begin(), a.end(), 0.0f);
    a[0] = 1.0f;
    fft.performRealOnlyForwardTransform (a.data(), true);
    fft.performRealOnlyInverseTransform (a.data());
    const auto scale = 1.0 / a[0];

    const auto frames = (int) ((end - start + track.hop - 1) / track.hop);
    track.hz.assign ((size_t) frames, 0.0f);

    // The whole range and enough either side, mixed once.
    const auto from = start - span / 2 - 1;
    const auto mix = mixOf (snapshot, from, (int) (end - start) + span + 2);
    std::vector<double> energy ((size_t) span + 1), cmnd ((size_t) maxLag + 2);

    for (int f = 0; f < frames; ++f)
    {
        if (progress != nullptr && (f & 255) == 0 && ! progress ((float) f / (float) frames))
            return {};

        const auto* s = mix.data() + (start + (juce::int64) f * track.hop - span / 2 - from);

        energy[0] = 0.0;

        for (int i = 0; i < span; ++i)
            energy[(size_t) i + 1] = energy[(size_t) i] + (double) s[i] * s[i];

        const auto e0 = energy[(size_t) window];

        if (e0 / window < quietest)
            continue;

        // r(τ) = Σ s[j] s[j + τ] over the window, by the FFT.
        std::fill (a.begin(), a.end(), 0.0f);
        std::fill (b.begin(), b.end(), 0.0f);
        std::copy (s, s + span, a.begin());
        std::copy (s, s + window, b.begin());
        fft.performRealOnlyForwardTransform (a.data(), true);
        fft.performRealOnlyForwardTransform (b.data(), true);

        for (int k = 0; k <= size / 2; ++k)
        {
            const auto ar = a[(size_t) (2 * k)], ai = a[(size_t) (2 * k + 1)], br = b[(size_t) (2 * k)], bi = b[(size_t) (2 * k + 1)];
            a[(size_t) (2 * k)] = ar * br + ai * bi;
            a[(size_t) (2 * k + 1)] = ai * br - ar * bi;
        }

        std::fill (a.begin() + size + 2, a.end(), 0.0f);
        fft.performRealOnlyInverseTransform (a.data());

        // The difference function, normalised by its running mean.
        double running = 0.0;
        cmnd[0] = 1.0;

        for (int tau = 1; tau <= maxLag + 1; ++tau)
        {
            const auto d = juce::jmax (0.0, e0 + (energy[(size_t) (tau + window)] - energy[(size_t) tau]) - 2.0 * scale * a[(size_t) tau]);
            running += d;
            cmnd[(size_t) tau] = running > 0.0 ? d * tau / running : 1.0;
        }

        auto tau = minLag;

        while (tau <= maxLag && cmnd[(size_t) tau] >= threshold)
            ++tau;

        if (tau > maxLag)
            continue;

        while (tau + 1 <= maxLag && cmnd[(size_t) (tau + 1)] < cmnd[(size_t) tau])
            ++tau;

        const auto x0 = cmnd[(size_t) (tau - 1)], x1 = cmnd[(size_t) tau], x2 = cmnd[(size_t) (tau + 1)];
        const auto bend = x0 - 2.0 * x1 + x2;
        const auto period = tau + (bend > 0.0 ? juce::jlimit (-0.5, 0.5, 0.5 * (x0 - x2) / bend) : 0.0);
        track.hz[(size_t) f] = (float) (sampleRate / period);
    }

    // A voiced run too short to be a note: no pitch.
    for (int f = 0; f < frames;)
    {
        auto g = f;

        while (g < frames && track.hz[(size_t) g] > 0.0f)
            ++g;

        if (g - f < shortestRun)
            std::fill (track.hz.begin() + f, track.hz.begin() + g, 0.0f);

        f = g + 1;
    }

    return track;
}

AudioDocument::Snapshot correct (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate, Settings settings,
                                 const std::function<bool (float)>& progress)
{
    const auto length = snapshot.getNumFrames();
    const auto channels = snapshot.getNumChannels();
    start = juce::jlimit ((juce::int64) 0, length, start);
    end = juce::jlimit (start, length, end);

    if (end <= start || channels == 0 || sampleRate <= 0.0 || ! isValid (settings))
        return nullptr;

    const auto context = (juce::int64) std::lround (contextSeconds * sampleRate);
    const auto workStart = juce::jmax ((juce::int64) 0, start - context), workEnd = juce::jmin (length, end + context);
    const auto track = detect (snapshot, workStart, workEnd, sampleRate, [&] (float p) { return progress == nullptr || progress (0.5f * p); });

    if (track.hz.empty())
        return nullptr;

    // Each frame's shift in cents: towards its note, by the strength, inside the range only,
    // smoothed over the speed (from none at each note's start, so its start is eased too).
    const auto frames = (int) track.hz.size();
    std::vector<double> shift ((size_t) frames, 0.0);
    const auto keep = settings.speedMs > 0.0f ? std::exp (-(double) track.hop / (settings.speedMs * 0.001 * sampleRate)) : 0.0;
    auto target = -1;
    auto smoothed = 0.0;

    for (int f = 0; f < frames; ++f)
    {
        const auto hz = (double) track.hz[(size_t) f];

        if (hz <= 0.0)
        {
            target = -1;
            smoothed = 0.0;
            continue;
        }

        const auto note = 69.0 + 12.0 * std::log2 (hz / 440.0);

        if (target < 0 || std::abs (note - target) > 0.5 + stickCents / 100.0)
        {
            auto best = -1;

            for (int n = (int) std::lround (note) - 2; n <= (int) std::lround (note) + 2; ++n)
                if (inScale (n, settings) && (best < 0 || std::abs (note - n) < std::abs (note - best)))
                    best = n;

            target = best;
        }

        const auto at = track.first + (juce::int64) f * track.hop;
        const auto wanted = at >= start && at < end ? (target - note) * 100.0 * settings.strengthPercent / 100.0 : 0.0;
        smoothed = keep * smoothed + (1.0 - keep) * wanted;
        shift[(size_t) f] = smoothed;
    }

    // The audio, with room for the grains either side.
    const auto unvoiced = juce::jmax (1, (int) std::lround (sampleRate / 200.0));   // marks 5 ms apart where there is no pitch
    const auto pad = (juce::int64) std::ceil (sampleRate / PitchCorrection::lowestHz) * 2 + 2;
    const auto bufferStart = workStart - pad;
    const auto bufferFrames = (int) (workEnd - workStart + 2 * pad);
    juce::AudioBuffer<float> in (channels, bufferFrames), sum (channels, bufferFrames);
    snapshot.read (in, 0, bufferStart, bufferFrames);
    sum.clear();
    std::vector<float> weights ((size_t) bufferFrames, 0.0f);

    const auto frameAt = [&] (double t) { return juce::jlimit (0, frames - 1, (int) std::lround ((t - (double) track.first) / track.hop)); };

    // Analysis marks. Where there is a pitch, a period apart from the loudest frame of the
    // channels' mix in the note's first period, so every grain is cut around the same point of
    // its period, as the voice's pulses are; 5 ms apart where there isn't.
    struct Mark { double at; int period; bool voiced; };
    std::vector<Mark> marks;
    std::vector<float> mix ((size_t) bufferFrames, 0.0f);

    for (int ch = 0; ch < channels; ++ch)
        juce::FloatVectorOperations::add (mix.data(), in.getReadPointer (ch), bufferFrames);

    const auto loudestIn = [&] (double from, double to)
    {
        auto best = (int) std::ceil (from - (double) bufferStart);

        for (auto i = best; i < to - (double) bufferStart; ++i)
            if (std::abs (mix[(size_t) i]) > std::abs (mix[(size_t) best]))
                best = i;

        return (double) best + (double) bufferStart;
    };

    auto wasVoiced = false;

    for (auto t = (double) workStart; t < (double) workEnd;)
    {
        const auto hz = (double) track.hz[(size_t) frameAt (t)];

        if (hz <= 0.0)
        {
            marks.push_back ({ t, unvoiced, false });
            t += unvoiced;
            wasVoiced = false;
            continue;
        }

        const auto period = sampleRate / hz;
        if (! wasVoiced)
            t = loudestIn (t, t + period);

        marks.push_back ({ t, juce::jmax (2, (int) std::lround (period)), true });
        t += period;
        wasVoiced = true;
    }

    // The input between frames: a cubic (Catmull-Rom) curve, exact on a frame.
    const auto read = [&] (int ch, double at)
    {
        const auto base = (int) std::floor (at);
        const auto x = (float) (at - base);

        if (base < 1 || base + 2 >= bufferFrames)
            return 0.0f;

        const auto* d = in.getReadPointer (ch);
        const auto p0 = d[base - 1], p1 = d[base], p2 = d[base + 1], p3 = d[base + 2];
        return p1 + 0.5f * x * (p2 - p0 + x * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + x * (3.0f * (p1 - p2) + p3 - p0)));
    };

    // Synthesis: from each output mark to the next a period of the new pitch, each taking the
    // grain of the analysis mark nearest it, the grain's centre put exactly at the mark. A note's
    // first grain goes where it was cut, so the note starts in step with the audio before it.
    size_t k = 0;
    auto count = 0;
    auto inNote = false;

    for (auto t = (double) workStart; t < (double) workEnd; ++count)
    {
        while (k + 1 < marks.size() && std::abs (marks[k + 1].at - t) <= std::abs (marks[k].at - t))
            ++k;

        const auto& m = marks[k];

        if (m.voiced && ! inNote)
            t = m.at;

        inNote = m.voiced;
        const auto f = frameAt (t);
        const auto hz = (double) track.hz[(size_t) f];
        const auto step = hz > 0.0 ? sampleRate / hz / std::pow (2.0, shift[(size_t) f] / 1200.0) : (double) m.period;
        const auto centre = t - (double) bufferStart, source = m.at - (double) bufferStart;

        for (auto to = (int) std::ceil (centre - m.period); to < centre + m.period; ++to)
        {
            const auto u = (double) to - centre;
            const auto w = (float) (0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * u / m.period));

            if (to < 0 || to >= bufferFrames)
                continue;

            weights[(size_t) to] += w;

            for (int ch = 0; ch < channels; ++ch)
                sum.addSample (ch, to, w * read (ch, source + u));
        }

        t += step;

        if (progress != nullptr && (count & 1023) == 0 && ! progress (0.5f + 0.5f * (float) ((t - (double) workStart) / (double) (workEnd - workStart))))
            return nullptr;
    }

    // The range: the overlap-add divided by the windows' sum, easing from the audio as it was at
    // edges inside the file.
    const auto n = (int) (end - start);
    juce::AudioBuffer<float> out (channels, n);
    const auto ease = juce::jmax ((juce::int64) 1, juce::jmin ((juce::int64) std::lround (easeSeconds * sampleRate), (end - start) / 2));

    for (int i = 0; i < n; ++i)
    {
        const auto frame = start + i;
        const auto at = (int) (frame - bufferStart);
        const auto w = weights[(size_t) at];
        auto mixIn = 1.0f;

        if (start > 0 && frame - start < ease)
            mixIn = (float) ((double) (frame - start) / (double) ease);
        else if (end < length && end - 1 - frame < ease)
            mixIn = (float) ((double) (end - 1 - frame) / (double) ease);

        for (int ch = 0; ch < channels; ++ch)
        {
            const auto was = in.getSample (ch, at);
            const auto now = w > 0.25f ? sum.getSample (ch, at) / w : was;
            out.setSample (ch, i, was + (now - was) * mixIn);
        }
    }

    std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (out)), 0, (juce::int64) n } };
    const AudioClip clip { channels, sampleRate, std::move (segments) };
    return AudioEdits::replaceWithClip (snapshot, start, end, clip);
}
}
