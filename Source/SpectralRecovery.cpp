#include "SpectralRecovery.h"
#include "AudioEdits.h"

namespace SpectralRecovery
{
namespace
{
    constexpr int fftOrder = 12, size = 1 << fftOrder, hop = size / 4, bins = size / 2 + 1;

    std::vector<float> rootHann()
    {
        std::vector<float> w ((size_t) size);

        for (int i = 0; i < size; ++i)
            w[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / size));

        return w;
    }

    /** The mean power of the bins from one frequency to another. */
    double bandPower (const std::vector<double>& power, double fromHz, double toHz, double binHz)
    {
        const auto from = juce::jlimit (1, bins - 1, (int) std::ceil (fromHz / binHz)), to = juce::jlimit (from, bins - 1, (int) (toHz / binHz));
        auto sum = 0.0;

        for (int k = from; k <= to; ++k)
            sum += power[(size_t) k];

        return sum / (to - from + 1);
    }
}

bool isValid (const Settings& s, double sampleRate)
{
    return sampleRate > 0.0 && (juce::exactlyEqual (s.cutoffHz, 0.0) || (s.cutoffHz >= 1000.0 && s.cutoffHz <= 16000.0)) && s.levelDb >= -24.0 && s.levelDb <= 6.0
        && s.highestHz >= 2000.0 && s.highestHz <= 20000.0 && s.highestHz < 0.5 * sampleRate;
}

double findCutoff (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate)
{
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (sampleRate <= 0.0 || end - start < size)
        return 0.0;

    // The long-term spectrum: the frames' power averaged (hopped by a whole window, enough to see).
    const auto channels = s.getNumChannels();
    juce::dsp::FFT fft (fftOrder);
    std::vector<float> data ((size_t) (2 * size)), hann ((size_t) size);
    std::vector<double> power ((size_t) bins, 0.0);
    juce::AudioBuffer<float> block (channels, size);
    auto frames = 0;

    for (int i = 0; i < size; ++i)
        hann[(size_t) i] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / size));

    for (auto pos = start; pos + size <= end; pos += size)
    {
        s.read (block, 0, pos, size);
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
            power[(size_t) k] += data[(size_t) (2 * k)] * data[(size_t) (2 * k)] + data[(size_t) (2 * k + 1)] * data[(size_t) (2 * k + 1)];

        ++frames;
    }

    if (frames == 0)
        return 0.0;

    // Scanning down from the top in sixth-octave steps: the first band holding within 30 dB of the
    // band an octave lower, where that band itself is within 60 dB of the spectrum's mean (so
    // residue against residue above the cutoff doesn't count), is where the spectrum is still
    // there; the cutoff is its top.
    const auto binHz = sampleRate / size;
    const auto top = juce::jmin (20000.0, 0.45 * sampleRate);
    auto mean = 0.0;

    for (int k = 1; k < bins; ++k)
        mean += power[(size_t) k];

    mean /= (bins - 1);

    if (mean <= 0.0)
        return 0.0;

    for (auto hz = top; hz > 1000.0; hz /= std::pow (2.0, 1.0 / 6.0))
    {
        const auto here = bandPower (power, hz / std::pow (2.0, 1.0 / 6.0), hz, binHz), lower = bandPower (power, hz / 2.0 / std::pow (2.0, 1.0 / 6.0), hz / 2.0, binHz);

        if (lower > mean * 1.0e-6 && here > lower * 0.001)
            return hz >= top / std::pow (2.0, 1.0 / 6.0) ? 0.0 : hz;
    }

    return 0.0;
}

AudioDocument::Snapshot recover (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings,
                                 const std::function<bool (float)>& progress)
{
    if (! isValid (settings, sampleRate))
        return nullptr;

    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (end - start < size || end - start > std::numeric_limits<int>::max() / 4)
        return nullptr;

    const auto cutoff = settings.cutoffHz > 0.0 ? settings.cutoffHz : findCutoff (s, start, end, sampleRate);

    if (cutoff <= 0.0 || cutoff >= settings.highestHz)
        return nullptr;

    const auto channels = s.getNumChannels();
    const auto frames = (int) (end - start);
    const auto window = rootHann();
    const auto binHz = sampleRate / size;
    const auto cutoffBin = (int) (cutoff / binHz), highestBin = juce::jmin (bins - 1, (int) (settings.highestHz / binHz));
    const auto level = std::pow (10.0, settings.levelDb / 20.0);

    if (cutoffBin < 8 || highestBin <= cutoffBin)
        return nullptr;

    const auto padded = frames + 2 * size;
    juce::AudioBuffer<float> in (channels, padded), out (channels, padded);
    in.clear();
    out.clear();
    s.read (in, size, start, frames);

    juce::dsp::FFT fft (fftOrder);
    std::vector<float> data ((size_t) (2 * size));
    std::vector<std::vector<double>> phaseAdvance ((size_t) channels, std::vector<double> ((size_t) bins, 0.0));   // the copies' running phase offsets
    const auto steps = (padded - size) / hop + 1;

    for (int step = 0; step < steps; ++step)
    {
        const auto pos = step * hop;

        if (progress && (step % 16) == 0 && ! progress ((float) step / (float) steps))
            return nullptr;

        for (int ch = 0; ch < channels; ++ch)
        {
            std::fill (data.begin(), data.end(), 0.0f);

            for (int i = 0; i < size; ++i)
                data[(size_t) i] = in.getSample (ch, pos + i) * window[(size_t) i];

            fft.performRealOnlyForwardTransform (data.data(), true);

            // The slope just below the cutoff: the octave below it against the octave below that, in
            // this frame; the rebuilt band continues it (within 24 dB an octave either way).
            std::vector<double> power ((size_t) bins);

            for (int k = 0; k < bins; ++k)
                power[(size_t) k] = (double) data[(size_t) (2 * k)] * data[(size_t) (2 * k)] + (double) data[(size_t) (2 * k + 1)] * data[(size_t) (2 * k + 1)];

            const auto upper = bandPower (power, cutoff / 2.0, cutoff, binHz), lowerOctave = bandPower (power, cutoff / 4.0, cutoff / 2.0, binHz);
            const auto slopeDb = lowerOctave > 1.0e-15 && upper > 1.0e-15 ? juce::jlimit (-24.0, 24.0, 10.0 * std::log10 (upper / lowerOctave)) : -6.0;
            const auto octaveGain = std::pow (10.0, slopeDb / 20.0) * level;   // the first copy's amplitude against its source

            // Copies: each source bin k in [cutoff / 2, cutoff) lands at 2k; the next copy from the first.
            auto& advance = phaseAdvance[(size_t) ch];
            auto gain = octaveGain;

            for (int copy = 0; copy < 3; ++copy)
            {
                const auto fillFrom = cutoffBin << copy, fillTo = juce::jmin (highestBin, cutoffBin << (copy + 1));

                if (fillFrom >= highestBin)
                    break;

                for (int target = fillFrom; target < fillTo; ++target)
                {
                    const auto source = target / 2;   // an octave down
                    const auto re = (double) data[(size_t) (2 * source)], im = (double) data[(size_t) (2 * source + 1)];
                    const auto magnitude = std::sqrt (re * re + im * im) * gain;

                    if (magnitude <= 0.0)
                        continue;

                    // The copy's phase: the source's, plus the advance the doubled frequency makes
                    // over a hop more than the source's own, kept running from frame to frame.
                    advance[(size_t) target] += juce::MathConstants<double>::twoPi * (target - source) * hop / (double) size;
                    advance[(size_t) target] = std::fmod (advance[(size_t) target], juce::MathConstants<double>::twoPi);
                    const auto phase = std::atan2 (im, re) + advance[(size_t) target];
                    const auto newRe = (float) (magnitude * std::cos (phase)), newIm = (float) (magnitude * std::sin (phase));
                    const auto have = power[(size_t) target];

                    if (magnitude * magnitude > have)
                    {
                        data[(size_t) (2 * target)] = newRe;
                        data[(size_t) (2 * target + 1)] = newIm;
                        power[(size_t) target] = magnitude * magnitude;
                    }
                }

                gain *= std::pow (10.0, slopeDb / 20.0);
            }

            fft.performRealOnlyInverseTransform (data.data());

            for (int i = 0; i < size; ++i)
                out.addSample (ch, pos + i, data[(size_t) i] * window[(size_t) i] / 2.0f);
        }
    }

    return AudioEdits::processRange (s, start, end, [&out] (juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            b.copyFrom (ch, 0, out, ch, size, b.getNumSamples());
    });
}
}
