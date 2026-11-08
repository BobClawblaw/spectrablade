#include "MatchEq.h"
#include "Analysis.h"

namespace MatchEq
{
double frequencyOf (int point)
{
    return lowestHz * std::pow (2.0, (double) point / pointsPerOctave);
}

std::optional<Profile> measure (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate)
{
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);
    constexpr int window = Analysis::fftSize, hop = window / 2;

    if (sampleRate <= 0.0 || end - start < window)
        return std::nullopt;

    // The windows' power averaged, bin by bin, over the channels' mix.
    const auto bins = (size_t) (window / 2 + 1);
    std::vector<double> power (bins, 0.0);
    std::vector<float> mono ((size_t) window);
    juce::AudioBuffer<float> block (s.getNumChannels(), window);
    auto windows = 0;

    for (auto pos = start; pos + window <= end; pos += hop)
    {
        s.read (block, 0, pos, window);

        for (int i = 0; i < window; ++i)
        {
            auto v = 0.0f;

            for (int ch = 0; ch < block.getNumChannels(); ++ch)
                v += block.getSample (ch, i);

            mono[(size_t) i] = v / (float) block.getNumChannels();
        }

        const auto db = Analysis::spectrumDb (mono.data(), window);

        for (size_t k = 0; k < bins; ++k)
            power[k] += std::pow (10.0, db[k] / 10.0);

        ++windows;
    }

    if (windows == 0)
        return std::nullopt;

    for (auto& p : power)
        p /= windows;

    // Each point: the mean power of the bins within its twelfth of an octave, or of the bins round
    // it where a bin is wider than that.
    Profile profile;
    profile.sampleRate = sampleRate;
    const auto binHz = sampleRate / window;
    auto anything = false;

    for (int i = 0; i < numPoints; ++i)
    {
        const auto centre = frequencyOf (i);
        const auto low = centre / std::pow (2.0, 0.5 / pointsPerOctave), high = centre * std::pow (2.0, 0.5 / pointsPerOctave);
        auto from = (int) std::floor (low / binHz), to = (int) std::ceil (high / binHz);

        if (to <= from)
            to = from + 1;

        from = juce::jlimit (0, (int) bins - 1, from);
        to = juce::jlimit (from + 1, (int) bins, to);
        auto sum = 0.0;

        for (int k = from; k < to; ++k)
            sum += power[(size_t) k];

        profile.levelDb[(size_t) i] = 10.0 * std::log10 (juce::jmax (1.0e-15, sum / (to - from)));
        anything = anything || profile.levelDb[(size_t) i] > -100.0;
    }

    if (! anything)
        return std::nullopt;

    return profile;
}

bool isValid (const Settings& s)
{
    return s.amountPercent >= 0.0 && s.amountPercent <= 100.0 && s.smoothingOctaves >= 1.0 / 12.0 - 1.0e-9 && s.smoothingOctaves <= 2.0
        && s.mostDb >= 0.0 && s.mostDb <= 24.0;
}

std::array<double, numPoints> correctionDb (const Profile& reference, const Profile& sound, const Settings& s)
{
    std::array<double, numPoints> raw {}, out {};
    std::array<bool, numPoints> counts {};

    for (int i = 0; i < numPoints; ++i)
    {
        counts[(size_t) i] = reference.levelDb[(size_t) i] > -100.0 && sound.levelDb[(size_t) i] > -100.0;
        raw[(size_t) i] = counts[(size_t) i] ? reference.levelDb[(size_t) i] - sound.levelDb[(size_t) i] : 0.0;
    }

    // Smoothed: each point the mean of the counted points within half the width either side.
    const auto reach = juce::jmax (0, juce::roundToInt (s.smoothingOctaves * pointsPerOctave / 2.0));

    for (int i = 0; i < numPoints; ++i)
    {
        auto sum = 0.0;
        auto n = 0;

        for (int j = juce::jmax (0, i - reach); j <= juce::jmin (numPoints - 1, i + reach); ++j)
            if (counts[(size_t) j])
            {
                sum += raw[(size_t) j];
                ++n;
            }

        out[(size_t) i] = n > 0 && counts[(size_t) i] ? sum / n : 0.0;
    }

    // The mean over 100 Hz to 10 kHz out, so the loudness stays; then within the most, by the amount.
    auto mean = 0.0;
    auto n = 0;

    for (int i = 0; i < numPoints; ++i)
        if (const auto hz = frequencyOf (i); hz >= 100.0 && hz <= 10000.0 && counts[(size_t) i])
        {
            mean += out[(size_t) i];
            ++n;
        }

    mean = n > 0 ? mean / n : 0.0;

    for (int i = 0; i < numPoints; ++i)
        out[(size_t) i] = counts[(size_t) i] ? juce::jlimit (-s.mostDb, s.mostDb, out[(size_t) i] - mean) * s.amountPercent / 100.0 : 0.0;

    return out;
}

double correctionAt (const std::array<double, numPoints>& c, double hz)
{
    if (hz <= lowestHz)
        return c.front();

    const auto position = std::log2 (hz / lowestHz) * pointsPerOctave;

    if (position >= numPoints - 1)
        return c.back();

    const auto i = (int) std::floor (position);
    const auto t = position - i;
    return c[(size_t) i] + t * (c[(size_t) i + 1] - c[(size_t) i]);
}

AudioEdits::Snapshot apply (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Profile& reference, const Settings& settings)
{
    if (! isValid (settings) || ! juce::exactlyEqual (reference.sampleRate, sampleRate))
        return nullptr;

    const auto sound = measure (s, start, end, sampleRate);

    if (! sound.has_value())
        return nullptr;

    const auto correction = correctionDb (reference, *sound, settings);
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (end - start > std::numeric_limits<int>::max() / 4)
        return nullptr;

    const auto prime = (int) juce::jmin (start, (juce::int64) Equalizer::LinearPhase::taps);

    return AudioEdits::processRange (s, start - prime, end, [&correction, sampleRate, prime] (juce::AudioBuffer<float>& b)
    {
        const auto channels = b.getNumChannels(), frames = b.getNumSamples();
        Equalizer::LinearPhase engine;
        engine.prepare ([&correction] (double hz) { return correctionAt (correction, hz); }, sampleRate, channels);

        juce::AudioBuffer<float> work (channels, frames + Equalizer::LinearPhase::latency);
        work.clear();

        for (int ch = 0; ch < channels; ++ch)
            work.copyFrom (ch, 0, b, ch, 0, frames);

        engine.process (work.getArrayOfWritePointers(), channels, work.getNumSamples());

        for (int ch = 0; ch < channels; ++ch)
            b.copyFrom (ch, prime, work, ch, Equalizer::LinearPhase::latency + prime, frames - prime);
    });
}
}
