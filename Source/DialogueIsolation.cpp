#include "DialogueIsolation.h"
#include "AudioEdits.h"

namespace DialogueIsolation
{
namespace
{
    constexpr int fftOrder = 12, size = 1 << fftOrder, hop = size / 4, bins = size / 2 + 1;
    constexpr double lowestPitch = 70.0, highestPitch = 400.0;
    constexpr int harmonicsCounted = 12, pitchOrder = 12, pitchFrame = 1 << pitchOrder;

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
    return s.reductionDb >= 0.0f && s.reductionDb <= 40.0f && s.sensitivity >= 0.0f && s.sensitivity <= 100.0f;
}

float thresholdFor (float sensitivity)
{
    return 0.75f - 0.3f * juce::jlimit (0.0f, 100.0f, sensitivity) / 100.0f;
}

Pitch pitchOf (const juce::AudioBuffer<float>& audio, int at, double sampleRate)
{
    // The frame (the channels mixed, Hann-windowed, zero-padded to twice its length so the
    // autocorrelation is the linear one) and its power spectrum.
    static const juce::dsp::FFT fft (pitchOrder + 1);
    static const auto window = [] { std::vector<float> w ((size_t) pitchFrame); for (int i = 0; i < pitchFrame; ++i) w[(size_t) i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / pitchFrame)); return w; }();
    static const auto windowCorrelation = [] { std::vector<double> c ((size_t) pitchFrame); for (int lag = 0; lag < pitchFrame; ++lag) { auto sum = 0.0; for (int i = 0; i + lag < pitchFrame; ++i) sum += (double) window[(size_t) i] * window[(size_t) (i + lag)]; c[(size_t) lag] = sum; } return c; }();
    constexpr int padded = 2 * pitchFrame, paddedBins = padded / 2 + 1;
    std::vector<float> data ((size_t) (2 * padded), 0.0f);
    const auto channels = audio.getNumChannels();

    if (channels < 1 || sampleRate <= 0.0)
        return {};

    for (int i = 0; i < pitchFrame; ++i)
    {
        const auto index = at + i;

        if (index < 0 || index >= audio.getNumSamples())
            continue;

        auto mix = 0.0f;

        for (int ch = 0; ch < channels; ++ch)
            mix += audio.getSample (ch, index);

        data[(size_t) i] = mix / (float) channels * window[(size_t) i];
    }

    fft.performRealOnlyForwardTransform (data.data(), true);
    const auto binHz = sampleRate / padded;
    std::vector<float> power ((size_t) paddedBins);

    for (int k = 0; k < paddedBins; ++k)
        power[(size_t) k] = data[(size_t) (2 * k)] * data[(size_t) (2 * k)] + data[(size_t) (2 * k + 1)] * data[(size_t) (2 * k + 1)];

    // The autocorrelation of the frame's 100 Hz to 1.5 kHz (the pitch's first harmonics; under
    // 100 Hz lies rumble, whose slow swings repeat by chance at a low voice's period), as the
    // inverse transform of that band's power.
    for (int k = 0; k < paddedBins; ++k)
    {
        const auto hz = k * binHz;
        data[(size_t) (2 * k)] = hz >= 100.0 && hz <= 1500.0 ? power[(size_t) k] : 0.0f;
        data[(size_t) (2 * k + 1)] = 0.0f;
    }

    fft.performRealOnlyInverseTransform (data.data());
    const auto energy = (double) data[0];

    if (energy <= 1.0e-12)
        return {};

    // How well the frame repeats at each lag from the highest pitch's period to the lowest's, the
    // window's own taper taken out; the best, and the shortest lag whose own peak comes within 15 %
    // of it (the period itself over its multiples, even where something between the harmonics
    // turns over at the period and not at twice it), refined between the lags round it.
    const auto shortest = juce::jmax (1, (int) (sampleRate / highestPitch)), longest = juce::jmin (pitchFrame - 2, (int) (sampleRate / lowestPitch));

    if (longest <= shortest)
        return {};

    const auto correlationAt = [&] (int lag) { return juce::jlimit (-1.0, 1.0, (double) data[(size_t) lag] / energy / (windowCorrelation[(size_t) lag] / windowCorrelation[0])); };
    std::vector<double> correlation ((size_t) (longest + 2), 0.0);

    for (int lag = 1; lag <= longest + 1; ++lag)
        correlation[(size_t) lag] = lag < pitchFrame ? correlationAt (lag) : 0.0;

    // A repeat counts only past the first trough: something periodic falls away from lag 0 before
    // it comes round again, where rumble or a hum's lows (which only fall away slowly) never do.
    auto trough = 0;

    while (trough <= longest && correlation[(size_t) trough + 1] > 0.3)
        ++trough;

    const auto first = juce::jmax (shortest, trough + 1);
    auto best = 0.0;

    for (int lag = first; lag <= longest; ++lag)
        best = juce::jmax (best, correlation[(size_t) lag]);

    if (best <= 0.0)
        return {};

    auto chosen = 0;

    for (int lag = first; lag <= longest && chosen == 0; ++lag)
        if (correlation[(size_t) lag] >= 0.85 * best && correlation[(size_t) lag] >= correlation[(size_t) lag - 1] && correlation[(size_t) lag] >= correlation[(size_t) lag + 1])
            chosen = lag;

    if (chosen == 0)
        return {};

    // A lone tone repeats just as well at half the period (its own, or a multiple of its own);
    // a voice, whose odd harmonics turn over there, does not.
    if (correlation[(size_t) (chosen / 2)] >= 0.9 * correlation[(size_t) chosen])
        return {};

    const auto left = correlation[(size_t) chosen - 1], middle = correlation[(size_t) chosen], right = correlation[(size_t) chosen + 1];
    const auto curve = left - 2.0 * middle + right;
    const auto offset = curve < 0.0 ? juce::jlimit (-0.5, 0.5, 0.5 * (left - right) / curve) : 0.0;
    const auto period = chosen + offset;
    const auto voicing = juce::jlimit (0.0, 1.0, middle - 0.25 * (left - right) * offset);
    const auto frequency = sampleRate / period;

    // A voice has harmonics: of the first twelve (under 4 kHz), each holding twice the power a bin
    // of the gaps round them, or this is a lone tone or a chance repeat in noise.
    const auto lowBin = (int) std::ceil (lowestPitch / binHz);
    std::vector<std::pair<double, double>> windows;   // each harmonic's power a bin, and the gap's before it
    auto lastBin = lowBin - 1;

    for (int h = 1; h <= harmonicsCounted && h * frequency <= 4000.0; ++h)
    {
        const auto centre = h * frequency;
        const auto from = juce::jmax (lastBin + 1, (int) std::ceil ((centre - frequency / 4.0) / binHz)), to = juce::jmin (paddedBins - 1, (int) ((centre + frequency / 4.0) / binHz));

        if (to < from)
            continue;

        auto here = 0.0, gap = 0.0;

        for (int k = from; k <= to; ++k)
            here += power[(size_t) k];

        for (int k = lastBin + 1; k < from; ++k)
            gap += power[(size_t) k];

        windows.emplace_back (here / (to - from + 1), from > lastBin + 1 ? gap / (from - lastBin - 1) : -1.0);
        lastBin = to;
    }

    auto found = 0;

    for (size_t i = 0; i < windows.size(); ++i)
    {
        // Over both the gaps either side (the one before, and the one before the next), so a
        // spectrum falling through them, or a hump of narrow noise, is not a peak.
        const auto before = windows[i].second, after = i + 1 < windows.size() ? windows[i + 1].second : -1.0;
        const auto gap = juce::jmax (before, after);

        if (gap >= 0.0 && windows[i].first > 2.0 * gap)
            ++found;
    }

    if (found < 3)
        return {};

    return { frequency, voicing };
}

AudioDocument::Snapshot isolate (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, Settings settings,
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
    const auto floorGain = std::pow (10.0, -settings.reductionDb / 20.0);
    const auto threshold = thresholdFor (settings.sensitivity);
    const auto binHz = sampleRate / size;
    const auto fourK = juce::jmin (bins - 1, (int) (4000.0 / binHz)), eightK = juce::jmin (bins - 1, (int) (8000.0 / binHz)), twoK = (int) (2000.0 / binHz);
    const auto eightyHz = (int) std::ceil (80.0 / binHz);

    // The audio with a frame of silence before and after, so every frame of the range is covered.
    const auto padded = frames + 2 * size;
    juce::AudioBuffer<float> in (channels, padded), out (channels, padded);
    in.clear();
    out.clear();
    s.read (in, size, start, frames);

    juce::dsp::FFT fft (fftOrder);
    std::vector<std::vector<float>> spectra ((size_t) channels, std::vector<float> ((size_t) (2 * size)));
    std::vector<float> power ((size_t) bins), smoothed ((size_t) bins, 0.0f), background ((size_t) bins, 1.0e30f), gains ((size_t) bins, 1.0f);
    std::vector<float> minimumOver ((size_t) bins, 1.0e30f);   // the running minimum being gathered
    const auto steps = (padded - size) / hop + 1;
    const auto minimumSpan = juce::jmax (1, (int) (1.5 * sampleRate / hop));   // frames the minimum is taken over
    auto gathered = 0;
    auto cancelled = false;
    const auto holdFrames = juce::jmax (1, (int) std::lround (0.08 * sampleRate / hop));
    Pitch held;
    auto heldFor = 0;

    for (int step = 0; step < steps; ++step)
    {
        const auto pos = step * hop;

        if (progress && (step % 16) == 0 && ! progress ((float) step / (float) steps))
        {
            cancelled = true;
            break;
        }

        // Each channel's spectrum, and the mix's power.
        std::fill (power.begin(), power.end(), 0.0f);

        for (int ch = 0; ch < channels; ++ch)
        {
            auto& data = spectra[(size_t) ch];
            std::fill (data.begin(), data.end(), 0.0f);

            for (int i = 0; i < size; ++i)
                data[(size_t) i] = in.getSample (ch, pos + i) * window[(size_t) i];

            fft.performRealOnlyForwardTransform (data.data(), true);

            for (int k = 0; k < bins; ++k)
            {
                const auto re = data[(size_t) (2 * k)], im = data[(size_t) (2 * k + 1)];
                power[(size_t) k] += (re * re + im * im) / (float) channels;
            }
        }

        // The background: each bin's power smoothed over time, its minimum over the last span.
        for (int k = 0; k < bins; ++k)
        {
            smoothed[(size_t) k] = step == 0 ? power[(size_t) k] : 0.7f * smoothed[(size_t) k] + 0.3f * power[(size_t) k];
            minimumOver[(size_t) k] = juce::jmin (minimumOver[(size_t) k], smoothed[(size_t) k]);
            background[(size_t) k] = juce::jmin (background[(size_t) k], smoothed[(size_t) k]);
        }

        if (++gathered >= minimumSpan)
        {
            background = minimumOver;
            std::fill (minimumOver.begin(), minimumOver.end(), 1.0e30f);
            gathered = 0;
        }

        // What to keep: the voiced harmonics and the highs over the background, or a fricative's band.
        // Voiced at the threshold, and staying so for 80 ms after while the voicing holds within
        // 0.15 of it (a voice's breathier moments and its ends), at the last pitch clearly read.
        auto pitch = pitchOf (in, pos, sampleRate);

        if (pitch.voicing >= threshold)
        {
            held = pitch;
            heldFor = 0;
        }
        else if (held.voicing > 0.0 && heldFor < holdFrames && pitch.voicing >= threshold - 0.15f)
        {
            ++heldFor;
            pitch = held;
        }
        else
            held = {};

        const auto voiced = held.voicing > 0.0;
        auto highsPower = 0.0, highsBackground = 0.0, lowsPower = 0.0, lowsBackground = 0.0;

        for (int k = eightyHz; k <= eightK; ++k)
        {
            (k >= twoK ? highsPower : lowsPower) += power[(size_t) k];
            (k >= twoK ? highsBackground : lowsBackground) += background[(size_t) k];
        }

        // A fricative: unvoiced, its highs (2 to 8 kHz) well over the background and over its lows.
        const auto fricative = ! voiced && highsPower > 4.0 * highsBackground && highsPower > lowsPower;
        std::vector<char> keep ((size_t) bins, 0);

        if (voiced)
        {
            for (int h = 1; h * pitch.frequency <= 4000.0; ++h)
            {
                const auto centre = h * pitch.frequency;
                const auto from = juce::jmax (eightyHz, (int) std::ceil ((centre - pitch.frequency / 4.0) / binHz)), to = juce::jmin (fourK, (int) ((centre + pitch.frequency / 4.0) / binHz));

                for (int k = from; k <= to; ++k)
                    keep[(size_t) k] = 1;
            }

            for (int k = fourK + 1; k <= eightK; ++k)
                if (power[(size_t) k] > 2.0f * background[(size_t) k])
                    keep[(size_t) k] = 1;
        }
        else if (fricative)
        {
            for (int k = twoK; k <= eightK; ++k)
                keep[(size_t) k] = 1;
        }

        // The gains ease: up at once, down over a few frames.
        for (int k = 0; k < bins; ++k)
        {
            const auto target = (float) (keep[(size_t) k] ? 1.0 : floorGain);
            gains[(size_t) k] = target > gains[(size_t) k] ? target : target + 0.6f * (gains[(size_t) k] - target);
        }

        for (int ch = 0; ch < channels; ++ch)
        {
            auto& data = spectra[(size_t) ch];

            for (int k = 0; k < bins; ++k)
            {
                data[(size_t) (2 * k)] *= gains[(size_t) k];
                data[(size_t) (2 * k + 1)] *= gains[(size_t) k];
            }

            fft.performRealOnlyInverseTransform (data.data());

            for (int i = 0; i < size; ++i)
                out.addSample (ch, pos + i, data[(size_t) i] * window[(size_t) i] / 2.0f);   // the windows sum to 2 at 75 % overlap
        }
    }

    if (cancelled)
        return nullptr;

    return AudioEdits::processRange (s, start, end, [&out] (juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            b.copyFrom (ch, 0, out, ch, size, b.getNumSamples());
    });
}
}
