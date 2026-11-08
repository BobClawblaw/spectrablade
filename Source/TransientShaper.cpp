#include "TransientShaper.h"
#include "Dynamics.h"

namespace TransientShaper
{
bool isValid (const Settings& s, double sampleRate)
{
    if (! Multiband::isValid (Multiband::Width { s.lowCrossover, s.highCrossover, { 100.0, 100.0, 100.0 } }, sampleRate) || std::abs (s.outputDb) > 24.0)
        return false;

    for (const auto& b : s.bands)
        if (std::abs (b.attackDb) > 12.0 || std::abs (b.sustainDb) > 12.0)
            return false;

    return true;
}

Settings spaced (Settings s, double sampleRate)
{
    const auto w = Multiband::spaced (Multiband::Width { s.lowCrossover, s.highCrossover, { 100.0, 100.0, 100.0 } }, sampleRate);
    s.lowCrossover = w.lowCrossover;
    s.highCrossover = w.highCrossover;
    return s;
}

void Core::prepare (int numChannels, double sampleRate)
{
    rate = sampleRate;
    channels.assign ((size_t) juce::jmax (1, numChannels), {});
    fastUp = Dynamics::smoothingCoefficient (0.5, rate);
    slowUp = Dynamics::smoothingCoefficient (15.0, rate);
    holdUp = Dynamics::smoothingCoefficient (1.0, rate);

    for (size_t b = 0; b < 3; ++b)
    {
        const auto low = b == 0;
        fastDown[b] = Dynamics::smoothingCoefficient (low ? 80.0 : 20.0, rate);
        slowDown[b] = Dynamics::smoothingCoefficient (low ? 150.0 : 50.0, rate);
        holdDown[b] = Dynamics::smoothingCoefficient (low ? 300.0 : 150.0, rate);
    }
    set (settings);
    reset();
}

void Core::reset() noexcept
{
    for (auto& ch : channels)
    {
        for (auto* c : { &ch.first, &ch.second, &ch.lowAlign })
            c->reset();

        ch.parts.fill (0.0);
    }

    envFast.fill (0.0);
    envSlow.fill (0.0);
    envHold.fill (0.0);
    gainDb.fill (0.0);
}

void Core::set (const Settings& s)
{
    settings = s;
    output = std::pow (10.0, s.outputDb / 20.0);

    for (auto& ch : channels)
    {
        ch.first.set (s.lowCrossover, rate);
        ch.second.set (s.highCrossover, rate);
        ch.lowAlign.set (s.highCrossover, rate);
    }
}

void Core::process (float* const* data, int numChannels, int numFrames) noexcept
{
    const auto n = juce::jmin (numChannels, (int) channels.size());

    for (int i = 0; i < numFrames; ++i)
    {
        std::array<double, 3> loudest {};

        for (int c = 0; c < n; ++c)
        {
            auto& ch = channels[(size_t) c];
            double low = 0.0, rest = 0.0, middle = 0.0, high = 0.0;
            ch.first.split (data[c][i], low, rest);
            ch.second.split (rest, middle, high);
            low = ch.lowAlign.allPass (low);
            ch.parts = { low, middle, high };

            for (size_t b = 0; b < 3; ++b)
                loudest[b] = juce::jmax (loudest[b], std::abs (ch.parts[b]));
        }

        std::array<double, 3> factors {};

        for (size_t b = 0; b < 3; ++b)
        {
            const auto x = loudest[b];
            envFast[b] = x > envFast[b] ? x + fastUp * (envFast[b] - x) : x + fastDown[b] * (envFast[b] - x);
            envSlow[b] = x > envSlow[b] ? x + slowUp * (envSlow[b] - x) : x + slowDown[b] * (envSlow[b] - x);
            envHold[b] = x > envHold[b] ? x + holdUp * (envHold[b] - x) : x + holdDown[b] * (envHold[b] - x);

            // Attacking where the fast envelope stands over the slow, sustaining where the held peak
            // stands over the fast; each from 1.5 dB over, in proportion to 12 dB; nothing under -60 dBFS.
            auto attacking = 0.0, sustaining = 0.0;

            if (envFast[b] > 1.0e-3)
            {
                attacking = juce::jlimit (0.0, 1.0, (20.0 * std::log10 (envFast[b] / juce::jmax (1.0e-9, envSlow[b])) - 1.5) / 10.5);
                sustaining = juce::jlimit (0.0, 1.0, (20.0 * std::log10 (envHold[b] / envFast[b]) - 1.5) / 10.5);
            }

            gainDb[b] = settings.bands[b].attackDb * attacking + settings.bands[b].sustainDb * sustaining;
            factors[b] = std::pow (10.0, gainDb[b] / 20.0) * output;
        }

        for (int c = 0; c < n; ++c)
        {
            const auto& p = channels[(size_t) c].parts;
            data[c][i] = (float) (p[0] * factors[0] + p[1] * factors[1] + p[2] * factors[2]);
        }
    }
}

AudioEdits::Snapshot apply (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings)
{
    if (! isValid (settings, sampleRate))
        return nullptr;

    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (start == end || end - start > std::numeric_limits<int>::max() / 2)
        return nullptr;

    const auto prime = (int) juce::jmin (start, (juce::int64) std::ceil (sampleRate));

    return AudioEdits::processRange (s, start - prime, end, [&settings, sampleRate, prime] (juce::AudioBuffer<float>& b)
    {
        juce::AudioBuffer<float> before (b.getNumChannels(), prime);

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            before.copyFrom (ch, 0, b, ch, 0, prime);

        Core core;
        core.prepare (b.getNumChannels(), sampleRate);
        core.set (settings);
        core.process (b.getArrayOfWritePointers(), b.getNumChannels(), b.getNumSamples());

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            b.copyFrom (ch, 0, before, ch, 0, prime);
    });
}
}
