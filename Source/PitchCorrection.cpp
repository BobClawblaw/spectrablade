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
    constexpr double hopsPerSecond = 1.0 / hopSeconds;

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

namespace
{
    /** Moves the pitch over [start, end) by the shift given for each frame of the track (in cents;
        frames without a pitch, and frames outside the range, are left), by PSOLA. */
    AudioDocument::Snapshot render (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate, const Track& track,
                                    std::vector<double> shift, const std::function<bool (float)>& progress)
    {
        const auto length = snapshot.getNumFrames();
        const auto channels = snapshot.getNumChannels();
        const auto frames = (int) track.hz.size();

        if (end <= start || channels == 0 || frames == 0 || (int) shift.size() != frames)
            return nullptr;

        const auto workStart = track.first, workEnd = juce::jmin (length, track.first + (juce::int64) frames * track.hop);

        for (int f = 0; f < frames; ++f)
        {
            const auto at = track.first + (juce::int64) f * track.hop;

            if (at < start || at >= end || track.hz[(size_t) f] <= 0.0f)
                shift[(size_t) f] = 0.0;
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

            if (progress != nullptr && (count & 1023) == 0 && ! progress ((float) ((t - (double) workStart) / (double) (workEnd - workStart))))
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

    return render (snapshot, start, end, sampleRate, track, std::move (shift), [&] (float p) { return progress == nullptr || progress (0.5f + 0.5f * p); });
}

Track analyse (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate, const std::function<bool (float)>& progress)
{
    const auto length = snapshot.getNumFrames();
    start = juce::jlimit ((juce::int64) 0, length, start);
    end = juce::jlimit (start, length, end);

    if (end <= start || sampleRate <= 0.0)
        return {};

    const auto context = (juce::int64) std::lround (contextSeconds * sampleRate);
    return detect (snapshot, juce::jmax ((juce::int64) 0, start - context), juce::jmin (length, end + context), sampleRate, progress);
}

std::vector<Note> findNotes (const Track& track)
{
    const auto frames = (int) track.hz.size();
    const auto steady = juce::jmax (1, (int) std::lround (0.02 * hopsPerSecond));   // 20 ms of frames
    std::vector<Note> notes;

    const auto midiAt = [&] (int f) { return 69.0 + 12.0 * std::log2 ((double) track.hz[(size_t) f] / 440.0); };

    for (int f = 0; f < frames;)
    {
        if (track.hz[(size_t) f] <= 0.0f)
        {
            ++f;
            continue;
        }

        auto runEnd = f;

        while (runEnd < frames && track.hz[(size_t) runEnd] > 0.0f)
            ++runEnd;

        // The run's pitch, each frame the median of five around it (within the run).
        std::vector<double> smooth;

        for (int g = f; g < runEnd; ++g)
        {
            std::vector<double> around;

            for (int h = juce::jmax (f, g - 2); h <= juce::jmin (runEnd - 1, g + 2); ++h)
                around.push_back (midiAt (h));

            std::nth_element (around.begin(), around.begin() + (long) around.size() / 2, around.end());
            smooth.push_back (around[around.size() / 2]);
        }

        // Splits where the pitch stays away from the note's for long enough.
        auto noteStart = 0;

        for (int g = 0; g < (int) smooth.size();)
        {
            std::vector<double> sofar (smooth.begin() + noteStart, smooth.begin() + g + 1);
            std::nth_element (sofar.begin(), sofar.begin() + (long) sofar.size() / 2, sofar.end());
            const auto pitch = sofar[sofar.size() / 2];
            auto away = g + 1;

            while (away < (int) smooth.size() && std::abs (smooth[(size_t) away] - pitch) > 0.6 && away - g <= steady)
                ++away;

            if (away - (g + 1) >= steady)
            {
                notes.push_back ({ f + noteStart, f + g + 1, 0.0, 0.0 });
                noteStart = g + 1;
                g = g + 1;
                continue;
            }

            ++g;
        }

        notes.push_back ({ f + noteStart, runEnd, 0.0, 0.0 });
        f = runEnd;
    }

    for (auto& n : notes)
        n.pitch = pitchOf (track, n.first, n.end);

    return notes;
}

double pitchOf (const Track& track, int first, int end)
{
    std::vector<double> pitches;

    for (int g = juce::jmax (0, first); g < juce::jmin ((int) track.hz.size(), end); ++g)
        if (track.hz[(size_t) g] > 0.0f)
            pitches.push_back (69.0 + 12.0 * std::log2 ((double) track.hz[(size_t) g] / 440.0));

    if (pitches.empty())
        return 0.0;

    std::nth_element (pitches.begin(), pitches.begin() + (long) pitches.size() / 2, pitches.end());
    return pitches[pitches.size() / 2];
}

std::vector<Note> splitNote (const Track& track, std::vector<Note> notes, int index, int at)
{
    if (! juce::isPositiveAndBelow (index, (int) notes.size()))
        return notes;

    const auto whole = notes[(size_t) index];

    if (at - whole.first < shortestNote || whole.end - at < shortestNote)
        return notes;

    Note left = whole, right = whole;
    left.end = right.first = at;
    left.pitch = pitchOf (track, left.first, left.end);
    right.pitch = pitchOf (track, right.first, right.end);
    notes[(size_t) index] = left;
    notes.insert (notes.begin() + index + 1, right);
    return notes;
}

bool canJoin (const std::vector<Note>& notes, int index)
{
    return index >= 0 && index + 1 < (int) notes.size() && notes[(size_t) index].end == notes[(size_t) index + 1].first;
}

std::vector<Note> joinNotes (const Track& track, std::vector<Note> notes, int index)
{
    if (! canJoin (notes, index))
        return notes;

    const auto& a = notes[(size_t) index];
    const auto& b = notes[(size_t) index + 1];
    Note joined;
    joined.first = a.first;
    joined.end = b.end;
    joined.pitch = pitchOf (track, joined.first, joined.end);
    joined.shiftCents = b.end - b.first > a.end - a.first ? b.shiftCents : a.shiftCents;
    notes[(size_t) index] = joined;
    notes.erase (notes.begin() + index + 1);
    return notes;
}

std::vector<Note> snapped (std::vector<Note> notes)
{
    for (auto& n : notes)
        n.shiftCents = (std::round (n.pitch) - n.pitch) * 100.0;

    return notes;
}

std::vector<double> shiftsFor (const Track& track, const std::vector<Note>& notes)
{
    const auto frames = (int) track.hz.size();
    std::vector<double> each ((size_t) frames, 0.0);
    std::vector<bool> inNote ((size_t) frames, false);

    for (auto& n : notes)
        for (int f = juce::jmax (0, n.first); f < juce::jmin (frames, n.end); ++f)
        {
            each[(size_t) f] = n.shiftCents;
            inNote[(size_t) f] = true;
        }

    // Eased where one note meets the next: the mean over five frames (25 ms) of notes only.
    std::vector<double> out ((size_t) frames, 0.0);

    for (int f = 0; f < frames; ++f)
    {
        if (! inNote[(size_t) f])
            continue;

        double sum = 0.0;
        auto count = 0;

        for (int g = juce::jmax (0, f - 2); g <= juce::jmin (frames - 1, f + 2); ++g)
            if (inNote[(size_t) g])
            {
                sum += each[(size_t) g];
                ++count;
            }

        out[(size_t) f] = sum / count;
    }

    return out;
}

AudioDocument::Snapshot moveNotes (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate, const Track& track,
                                   const std::vector<Note>& notes, const std::function<bool (float)>& progress)
{
    const auto length = snapshot.getNumFrames();
    start = juce::jlimit ((juce::int64) 0, length, start);
    end = juce::jlimit (start, length, end);

    if (end <= start || sampleRate <= 0.0 || track.hz.empty() || track.first > start
        || track.first + (juce::int64) track.hz.size() * track.hop < end)
        return nullptr;

    return render (snapshot, start, end, sampleRate, track, shiftsFor (track, notes), progress);
}
}
