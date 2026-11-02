#include "Modulation.h"

namespace Modulation
{
namespace
{
    constexpr double maxDelayMs = 100.0;   // the shortest delay plus the depth, at most

    bool rangeOk (const AudioSnapshot& s, juce::int64& start, juce::int64& end)
    {
        start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
        end = juce::jlimit (start, s.getNumFrames(), end);
        return start < end && end - start <= std::numeric_limits<int>::max();
    }

    /** The sweep for a channel: 0 at the top of the cycle, 1 halfway through. */
    double sweep (double phase, int channel, double spreadDegrees) noexcept
    {
        const auto p = phase + (channel % 2 == 1 ? spreadDegrees / 360.0 : 0.0);
        return 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * p);
    }

    bool inRange (double v, double low, double high)    { return std::isfinite (v) && v >= low && v <= high; }
}

Delay chorusDefaults()
{
    return {};
}

Delay flangerDefaults()
{
    return { 0.25, 1.0, 3.0, 0.6, 50.0, 90.0 };
}

bool isValid (const Delay& d)
{
    return inRange (d.rateHz, 0.01, 10.0) && inRange (d.delayMs, 0.0, 50.0) && inRange (d.depthMs, 0.0, 50.0)
        && inRange (d.feedback, -0.95, 0.95) && inRange (d.mixPercent, 0.0, 100.0) && inRange (d.spreadDegrees, 0.0, 180.0);
}

bool isValid (const Phaser& p)
{
    return inRange (p.rateHz, 0.01, 10.0) && inRange (p.lowHz, 20.0, 20000.0) && inRange (p.highHz, p.lowHz, 20000.0)
        && p.stages >= 2 && p.stages <= 12 && p.stages % 2 == 0 && inRange (p.feedback, -0.95, 0.95)
        && inRange (p.mixPercent, 0.0, 100.0) && inRange (p.spreadDegrees, 0.0, 180.0);
}

//==============================================================================
void DelayCore::prepare (double sampleRate, int numChannels)
{
    rate = sampleRate;
    const auto length = (size_t) std::ceil (maxDelayMs / 1000.0 * sampleRate) + 3;
    lines.assign ((size_t) juce::jmax (1, numChannels), std::vector<float> (length, 0.0f));
    reset();
}

void DelayCore::reset()
{
    for (auto& l : lines)
        std::fill (l.begin(), l.end(), 0.0f);

    write = 0;
    phase = 0.0;
}

void DelayCore::process (float* const* channels, int numChannels, int numFrames) noexcept
{
    if (lines.empty())
        return;

    numChannels = juce::jmin (numChannels, (int) lines.size());
    const auto length = (int) lines[0].size();
    const auto step = settings.rateHz / rate;
    const auto wet = settings.mixPercent / 100.0, dry = 1.0 - wet;
    const auto shortest = settings.delayMs / 1000.0 * rate, depth = settings.depthMs / 1000.0 * rate;
    const auto longest = (double) (length - 2);

    for (int i = 0; i < numFrames; ++i)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& line = lines[(size_t) ch];

            // The delay in frames: at least one, so it reads only what has been written.
            const auto d = juce::jlimit (1.0, longest, shortest + depth * sweep (phase, ch, settings.spreadDegrees));
            const auto whole = (int) d;
            const auto frac = d - whole;
            auto a = write - whole, b = a - 1;
            a += a < 0 ? length : 0;
            b += b < 0 ? length : 0;
            const auto tap = (double) line[(size_t) a] + frac * ((double) line[(size_t) b] - (double) line[(size_t) a]);

            const auto x = (double) channels[ch][i];
            line[(size_t) write] = (float) (x + settings.feedback * tap);
            channels[ch][i] = (float) (dry * x + wet * tap);
        }

        write = write + 1 == length ? 0 : write + 1;
        phase += step;
        phase -= std::floor (phase);
    }
}

//==============================================================================
void PhaserCore::prepare (double sampleRate, int numChannels)
{
    rate = sampleRate;
    state.assign ((size_t) juce::jmax (1, numChannels), {});
    last.assign (state.size(), 0.0);
    reset();
}

void PhaserCore::reset()
{
    for (auto& s : state)
        s.fill (0.0);

    std::fill (last.begin(), last.end(), 0.0);
    phase = 0.0;
}

void PhaserCore::process (float* const* channels, int numChannels, int numFrames) noexcept
{
    numChannels = juce::jmin (numChannels, (int) state.size());
    const auto step = settings.rateHz / rate;
    const auto wet = settings.mixPercent / 100.0, dry = 1.0 - wet;
    const auto top = 0.45 * rate;
    const auto low = juce::jmin (settings.lowHz, top), high = juce::jmin (settings.highHz, top);
    const auto stages = juce::jlimit (2, maxStages, settings.stages);

    for (int i = 0; i < numFrames; ++i)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            // The all-passes' frequency, swept evenly in octaves from low to high and back.
            const auto f = low * std::pow (high / low, sweep (phase, ch, settings.spreadDegrees));
            const auto t = std::tan (juce::MathConstants<double>::pi * f / rate);
            const auto c = (t - 1.0) / (t + 1.0);

            const auto x = (double) channels[ch][i];
            auto y = x + settings.feedback * last[(size_t) ch];
            auto& z = state[(size_t) ch];

            for (int k = 0; k < stages; ++k)
            {
                // A first-order all-pass, (c + z^-1) / (1 + c z^-1).
                const auto out = c * y + z[(size_t) k];
                z[(size_t) k] = y - c * out;
                y = out;
            }

            last[(size_t) ch] = y;
            channels[ch][i] = (float) (dry * x + wet * y);
        }

        phase += step;
        phase -= std::floor (phase);
    }
}

//==============================================================================
AudioEdits::Snapshot sweptDelay (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Delay& d)
{
    if (! isValid (d) || sampleRate <= 0.0 || ! rangeOk (s, start, end))
        return nullptr;

    return AudioEdits::processRange (s, start, end, [&d, sampleRate] (juce::AudioBuffer<float>& b)
    {
        DelayCore core;
        core.prepare (sampleRate, b.getNumChannels());
        core.set (d);
        core.process (b.getArrayOfWritePointers(), b.getNumChannels(), b.getNumSamples());
    });
}

AudioEdits::Snapshot phaser (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Phaser& p)
{
    if (! isValid (p) || sampleRate <= 0.0 || ! rangeOk (s, start, end))
        return nullptr;

    return AudioEdits::processRange (s, start, end, [&p, sampleRate] (juce::AudioBuffer<float>& b)
    {
        PhaserCore core;
        core.prepare (sampleRate, b.getNumChannels());
        core.set (p);
        core.process (b.getArrayOfWritePointers(), b.getNumChannels(), b.getNumSamples());
    });
}
}
