#include "Maximizer.h"

namespace Maximizer
{
const juce::StringArray& characterNames()
{
    static const juce::StringArray names { "Transparent", "Balanced", "Loud" };
    return names;
}

bool isValid (const Settings& s)
{
    return s.gainDb >= 0.0 && s.gainDb <= 24.0 && s.ceilingDb >= -24.0 && s.ceilingDb <= 0.0 && s.emphasisPercent >= 0.0 && s.emphasisPercent <= 100.0;
}

Timing timingOf (Character c)
{
    switch (c)
    {
        case Character::transparent: return { 10.0, 400.0, 60.0, 0.2 };
        case Character::loud:        return { 2.0, 80.0, 10.0, 0.85 };
        case Character::balanced:    break;
    }

    return { 5.0, 200.0, 30.0, 0.5 };
}

int latencyFrames (Character c, double sampleRate)
{
    return Oversampler4::upLatency + juce::jmax (1, juce::roundToInt (timingOf (c).lookaheadMs / 1000.0 * sampleRate));
}

void Core::prepare (int numChannels, double sampleRate)
{
    rate = sampleRate;
    channelsPrepared = juce::jmax (1, numChannels);
    detectors.assign ((size_t) channelsPrepared, {});
    set (settings);
    reset();
}

void Core::set (const Settings& s)
{
    const auto characterChanged = s.character != settings.character || delay.getNumSamples() == 0;
    settings = s;
    input = std::pow (10.0, s.gainDb / 20.0);
    top = Dynamics::ceilingGain (s.ceilingDb);
    const auto t = timingOf (s.character);
    blend = t.blend;
    slow.setRelease (Dynamics::smoothingCoefficient (t.slowReleaseMs, rate));
    fast.setRelease (Dynamics::smoothingCoefficient (t.fastReleaseMs, rate));

    // The attack envelope: 0.5 ms up and 20 ms down; the body: 10 ms up and 100 ms down.
    fastUp = Dynamics::smoothingCoefficient (0.5, rate);
    fastDown = Dynamics::smoothingCoefficient (20.0, rate);
    slowUp = Dynamics::smoothingCoefficient (10.0, rate);
    slowDown = Dynamics::smoothingCoefficient (100.0, rate);

    if (characterChanged)
        reset();
}

void Core::reset()
{
    ahead = juce::jmax (1, juce::roundToInt (timingOf (settings.character).lookaheadMs / 1000.0 * rate));
    const auto t = timingOf (settings.character);
    slow.prepare (ahead, Dynamics::smoothingCoefficient (t.slowReleaseMs, rate));
    fast.prepare (ahead, Dynamics::smoothingCoefficient (t.fastReleaseMs, rate));

    for (auto& d : detectors)
        d.reset();

    delay.setSize (channelsPrepared, Oversampler4::upLatency + ahead);
    delay.clear();
    emphasisGains.assign ((size_t) (Oversampler4::upLatency + ahead), 1.0);
    delayPos = 0;
    envFast = envSlow = 0.0;
    reductionDb = 0.0;
}

void Core::process (float* const* channels, int numChannels, int numFrames) noexcept
{
    const auto n = juce::jmin (numChannels, channelsPrepared);
    const auto emphasis = settings.emphasisPercent / 100.0;
    const auto length = delay.getNumSamples();

    for (int i = 0; i < numFrames; ++i)
    {
        // The frame's true peak (for the frame upLatency back) and its sample peak (for this one).
        double truePeak = 0.0, samplePeak = 0.0;

        for (int ch = 0; ch < n; ++ch)
        {
            const double x = channels[ch][i];
            truePeak = juce::jmax (truePeak, detectors[(size_t) ch].peakTick (x));
            samplePeak = juce::jmax (samplePeak, std::abs (x));
        }

        // The attack lift for this frame, from how far the fast envelope stands over the slow one
        // (up to 12 dB counts), kept with the frame until it comes out of the delay.
        envFast = samplePeak > envFast ? samplePeak + fastUp * (envFast - samplePeak) : samplePeak + fastDown * (envFast - samplePeak);
        envSlow = samplePeak > envSlow ? samplePeak + slowUp * (envSlow - samplePeak) : samplePeak + slowDown * (envSlow - samplePeak);
        const auto overDb = envFast > 1.0e-9 && envSlow > 1.0e-9 ? juce::jlimit (0.0, 12.0, 20.0 * std::log10 (envFast / envSlow)) : 0.0;
        const auto lift = std::pow (10.0, emphasis * overDb / 12.0 * 6.0 / 20.0);

        // The lift the delayed (detected) frame carries: the one upLatency frames back.
        const auto detectedPos = (delayPos + length - Oversampler4::upLatency) % length;
        const auto detectedLift = emphasisGains[(size_t) detectedPos];
        const auto need = Dynamics::limiterNeed (truePeak * detectedLift, input, top);
        const auto slowGain = slow.push (need), fastGain = fast.push (need);

        // Out goes the frame a whole delay old, with its lift and the blended gain.
        for (int ch = 0; ch < n; ++ch)
        {
            const auto old = (double) delay.getSample (ch, delayPos);
            delay.setSample (ch, delayPos, channels[ch][i]);
            auto out = old;

            if (slowGain.has_value() && fastGain.has_value())
            {
                const auto gain = std::pow (*slowGain, 1.0 - blend) * std::pow (*fastGain, blend);
                out = juce::jlimit (-top, top, old * emphasisGains[(size_t) delayPos] * input * gain);

                if (ch == 0)
                    reductionDb = 20.0 * std::log10 (juce::jmax (1.0e-9, gain));
            }

            channels[ch][i] = (float) out;
        }

        emphasisGains[(size_t) delayPos] = lift;
        delayPos = (delayPos + 1) % length;
    }
}

AudioEdits::Snapshot apply (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings)
{
    if (! isValid (settings) || sampleRate <= 0.0)
        return nullptr;

    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (start == end || end - start > std::numeric_limits<int>::max() / 4)
        return nullptr;

    const auto prime = (int) juce::jmin (start, (juce::int64) std::ceil (sampleRate));
    const auto latency = latencyFrames (settings.character, sampleRate);

    return AudioEdits::processRange (s, start - prime, end, [&settings, sampleRate, prime, latency] (juce::AudioBuffer<float>& b)
    {
        const auto channels = b.getNumChannels(), frames = b.getNumSamples();
        Core core;
        core.prepare (channels, sampleRate);
        core.set (settings);

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
