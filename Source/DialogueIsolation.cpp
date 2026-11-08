#include "DialogueIsolation.h"
#include "AudioEdits.h"

namespace DialogueIsolation
{
namespace
{
    constexpr int fftOrder = 12, size = 1 << fftOrder, hop = size / 4, bins = size / 2 + 1;
    constexpr double lowestPitch = 70.0, highestPitch = 400.0;
    constexpr int harmonicsCounted = 12;

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
    return 0.85f - 0.25f * juce::jlimit (0.0f, 100.0f, sensitivity) / 100.0f;
}

Pitch pitchOf (const std::vector<float>& power, double sampleRate)
{
    const auto binHz = sampleRate / size;
    const auto lowBin = (int) std::ceil (80.0 / binHz), highBin = juce::jmin ((int) power.size() - 1, (int) (4000.0 / binHz));
    auto total = 0.0;

    for (int k = lowBin; k <= highBin; ++k)
        total += power[(size_t) k];

    if (total <= 1.0e-12)
        return {};

    // Candidates a sixteenth of an octave apart; each compares the power a bin within a quarter of
    // itself round its first twelve harmonics (counted once a bin) with the power a bin in the gaps
    // between them: the median of the gaps' own densities, so one tone in a gap doesn't count for
    // all, and the gaps are the harmonics' own neighbours (a spectrum falling smoothly with
    // frequency would read as harmonics against the quiet far above them otherwise).
    std::vector<double> cumulative ((size_t) power.size() + 1, 0.0);

    for (size_t k = 0; k < power.size(); ++k)
        cumulative[k + 1] = cumulative[k] + power[k];

    Pitch best;
    std::vector<double> gapDensities;

    for (auto f0 = lowestPitch; f0 <= highestPitch; f0 *= std::pow (2.0, 1.0 / 16.0))
    {
        auto sum = 0.0;
        auto counted = 0, found = 0, lastBin = -1;
        std::vector<std::pair<double, int>> windows;   // each harmonic's power and bins
        gapDensities.clear();

        for (int h = 1; h <= harmonicsCounted; ++h)
        {
            const auto centre = h * f0;

            if (centre > 4000.0)
                break;

            const auto from = juce::jmax (lowBin, (int) std::ceil ((centre - f0 / 4.0) / binHz)), to = juce::jmin (highBin, (int) ((centre + f0 / 4.0) / binHz));
            auto here = 0.0;
            auto width = 0;
            const auto gapFrom = juce::jmax (lowBin, lastBin + 1), gapTo = juce::jmax (from, lastBin + 1) - 1;   // the gap before this window

            if (gapTo >= gapFrom)
                gapDensities.push_back ((cumulative[(size_t) gapTo + 1] - cumulative[(size_t) gapFrom]) / (gapTo - gapFrom + 1));

            for (int k = juce::jmax (from, lastBin + 1); k <= to; ++k)
            {
                here += power[(size_t) k];
                ++width;
            }

            lastBin = juce::jmax (lastBin, to);
            sum += here;
            counted += width;
            windows.emplace_back (here, width);
        }

        if (counted == 0 || gapDensities.empty())
            continue;

        std::sort (gapDensities.begin(), gapDensities.end());
        const auto gapDensity = juce::jmax (1.0e-15, gapDensities[gapDensities.size() / 2]);

        for (const auto& [here, width] : windows)
            if (width > 0 && here / width > 4.0 * gapDensity)
                ++found;

        // At least three harmonics, and two thirds of those looked for (a candidate an octave or
        // more under the pitch finds only every other one); of candidates within 2 % the higher wins.
        if (found < 3 || found * 3 < (int) windows.size() * 2)
            continue;

        const auto ratio = (sum / counted) / gapDensity;
        const auto voicing = ratio / (1.0 + ratio);

        if (voicing > 0.98 * best.voicing)
            best = { f0, voicing };
    }

    return best;
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
        const auto pitch = pitchOf (power, sampleRate);
        const auto voiced = pitch.voicing >= threshold;
        auto highsPower = 0.0, highsBackground = 0.0, lowsPower = 0.0, lowsBackground = 0.0;

        for (int k = eightyHz; k <= eightK; ++k)
        {
            (k >= twoK ? highsPower : lowsPower) += power[(size_t) k];
            (k >= twoK ? highsBackground : lowsBackground) += background[(size_t) k];
        }

        const auto fricative = ! voiced && highsPower > 4.0 * highsBackground && lowsPower < 4.0 * lowsBackground + 1.0e-12;
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
