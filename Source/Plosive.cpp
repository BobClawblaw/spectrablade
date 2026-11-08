#include "Plosive.h"
#include "Dynamics.h"

namespace Plosive
{
bool isValid (const Settings& s, double sampleRate)
{
    return sampleRate > 0.0 && s.cornerHz >= 60.0 && s.cornerHz <= 400.0 && s.cornerHz < 0.45 * sampleRate && s.thresholdDb >= 3.0 && s.thresholdDb <= 30.0
        && s.reductionDb >= 0.0 && s.reductionDb <= 40.0;
}

void Core::prepare (int numChannels, double sampleRate)
{
    rate = sampleRate;
    ahead = juce::jmax (1, (int) std::lround (0.01 * rate));
    channels.assign ((size_t) juce::jmax (1, numChannels), {});

    for (auto& ch : channels)
    {
        ch.lowDelay.assign ((size_t) ahead, 0.0);
        ch.highDelay.assign ((size_t) ahead, 0.0);
    }

    lows.assign (channels.size(), 0.0);
    highs.assign (channels.size(), 0.0);
    fastUp = Dynamics::smoothingCoefficient (2.0, rate);
    fastDown = Dynamics::smoothingCoefficient (40.0, rate);
    slow = Dynamics::smoothingCoefficient (1000.0, rate);
    slower = Dynamics::smoothingCoefficient (5000.0, rate);
    ramp = Dynamics::smoothingCoefficient (5.0, rate);
    longest = juce::jmax (1, (int) (0.25 * rate));
    set (settings);
    reset();
}

void Core::reset() noexcept
{
    for (auto& ch : channels)
    {
        ch.split.reset();
        std::fill (ch.lowDelay.begin(), ch.lowDelay.end(), 0.0);
        std::fill (ch.highDelay.begin(), ch.highDelay.end(), 0.0);
    }

    delayPos = 0;
    envLow = envHigh = 0.0;
    typicalLow = typicalHigh = 1.0e-6;
    gain = 1.0;
    onFor = 0;
}

void Core::set (const Settings& s)
{
    settings = s;

    for (auto& ch : channels)
        ch.split.set (s.cornerHz, rate);

    floorGain = std::pow (10.0, -s.reductionDb / 20.0);
    threshold = std::pow (10.0, s.thresholdDb / 20.0);
}

void Core::process (float* const* data, int numChannels, int numFrames) noexcept
{
    const auto n = juce::jmin (numChannels, (int) channels.size());

    for (int i = 0; i < numFrames; ++i)
    {
        auto lowPeak = 0.0, highPeak = 0.0;

        for (int c = 0; c < n; ++c)
        {
            channels[(size_t) c].split.split (data[c][i], lows[(size_t) c], highs[(size_t) c]);
            lowPeak = juce::jmax (lowPeak, std::abs (lows[(size_t) c]));
            highPeak = juce::jmax (highPeak, std::abs (highs[(size_t) c]));
        }

        envLow = lowPeak > envLow ? lowPeak + fastUp * (envLow - lowPeak) : lowPeak + fastDown * (envLow - lowPeak);
        envHigh = highPeak > envHigh ? highPeak + fastUp * (envHigh - highPeak) : highPeak + fastDown * (envHigh - highPeak);

        // A plosive: the low band over its typical level by the threshold, the band above not risen
        // with it by as much (within 6 dB of its own typical level, or the low band's rise is at
        // least 6 dB more than the high band's).
        const auto lowRise = envLow / juce::jmax (1.0e-9, typicalLow), highRise = envHigh / juce::jmax (1.0e-9, typicalHigh);
        auto plosive = envLow > 1.0e-4 && lowRise > threshold && lowRise > 2.0 * highRise;

        // One lasts at most `longest`; after that, not again until the lows have come back down.
        if (plosive && ++onFor > longest)
            plosive = false;
        else if (! plosive && lowRise < threshold / 2.0)
            onFor = 0;
        else if (! plosive && onFor > longest)
            onFor = longest + 1;   // still waiting for the lows to come down

        // The typical levels follow the envelopes: slowly in a plosive, so the thump hardly counts.
        const auto follow = plosive || onFor > longest ? slower : slow;
        typicalLow = envLow + follow * (typicalLow - envLow);
        typicalHigh = envHigh + follow * (typicalHigh - envHigh);

        const auto target = plosive ? floorGain : 1.0;
        gain = target + ramp * (gain - target);

        // Out go the bands from `ahead` frames ago, the low one at the gain found now.
        for (int c = 0; c < n; ++c)
        {
            auto& ch = channels[(size_t) c];
            const auto oldLow = ch.lowDelay[(size_t) delayPos], oldHigh = ch.highDelay[(size_t) delayPos];
            ch.lowDelay[(size_t) delayPos] = lows[(size_t) c];
            ch.highDelay[(size_t) delayPos] = highs[(size_t) c];
            data[c][i] = (float) (oldLow * gain + oldHigh);
        }

        if (++delayPos == ahead)
            delayPos = 0;
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
        const auto channels = b.getNumChannels(), frames = b.getNumSamples();
        Core core;
        core.prepare (channels, sampleRate);
        core.set (settings);
        const auto latency = core.getLatency();

        // The audio plus the core's delay of silence after it, through the core; what comes out from
        // the delay on is the range, in place (the primed part kept as it was).
        juce::AudioBuffer<float> work (channels, frames + latency);
        work.clear();

        for (int ch = 0; ch < channels; ++ch)
            work.copyFrom (ch, 0, b, ch, 0, frames);

        core.process (work.getArrayOfWritePointers(), channels, work.getNumSamples());

        for (int ch = 0; ch < channels; ++ch)
            b.copyFrom (ch, prime, work, ch, latency + prime, frames - prime);
    });
}
}
