#include "Exciter.h"

namespace Exciter
{
const juce::StringArray& curveNames()
{
    static const juce::StringArray names { "Tube", "Tape", "Transistor" };
    return names;
}

double shape (Curve curve, double x) noexcept
{
    switch (curve)
    {
        case Curve::tube:
            // The negative half rounds sooner (into two thirds of the swing), the slope 1 at zero
            // on both: the unevenness is what makes the even harmonics.
            return x >= 0.0 ? std::tanh (x) : std::tanh (1.5 * x) / 1.5;

        case Curve::tape:
            return std::tanh (x);

        case Curve::transistor:
            // A cubic to a flat top at 2/3 of the way in: harder, and the top is reached.
            if (x >= 1.0)
                return 2.0 / 3.0;

            if (x <= -1.0)
                return -2.0 / 3.0;

            return x - x * x * x / 3.0;
    }

    return x;
}

bool isValid (const Settings& s, double sampleRate)
{
    if (! Multiband::isValid (Multiband::Width { s.lowCrossover, s.highCrossover, { 100.0, 100.0, 100.0 } }, sampleRate) || std::abs (s.outputDb) > 24.0)
        return false;

    for (const auto& b : s.bands)
        if (b.driveDb < 0.0 || b.driveDb > 36.0 || b.mixPercent < 0.0 || b.mixPercent > 100.0)
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
    blocker = std::exp (-juce::MathConstants<double>::twoPi * 5.0 / rate);
    set (settings);
    reset();
}

void Core::reset() noexcept
{
    for (auto& ch : channels)
    {
        for (auto* c : { &ch.first, &ch.second, &ch.lowAlign })
            c->reset();

        for (auto& s : ch.shapers)
            s.reset();

        for (auto& d : ch.dry)
            d.fill (0.0);

        ch.blockIn.fill (0.0);
        ch.blockOut.fill (0.0);
    }

    dryPos = 0;
}

void Core::set (const Settings& s)
{
    settings = s;

    for (auto& ch : channels)
    {
        ch.first.set (s.lowCrossover, rate);
        ch.second.set (s.highCrossover, rate);
        ch.lowAlign.set (s.highCrossover, rate);
    }

    for (size_t b = 0; b < 3; ++b)
    {
        drive[b] = std::pow (10.0, (s.bands[b].driveDb - headroomDb) / 20.0);
        wet[b] = s.bands[b].mixPercent / 100.0;
    }

    output = std::pow (10.0, s.outputDb / 20.0);
}

void Core::process (float* const* data, int numChannels, int numFrames) noexcept
{
    const auto n = juce::jmin (numChannels, (int) channels.size());

    for (int i = 0; i < numFrames; ++i)
    {
        for (int c = 0; c < n; ++c)
        {
            auto& ch = channels[(size_t) c];
            double low = 0.0, rest = 0.0, middle = 0.0, high = 0.0;
            ch.first.split (data[c][i], low, rest);
            ch.second.split (rest, middle, high);
            low = ch.lowAlign.allPass (low);
            const std::array<double, 3> parts { low, middle, high };
            auto sum = 0.0;

            for (size_t b = 0; b < 3; ++b)
            {
                const auto curve = settings.bands[b].curve;
                const auto g = drive[b];
                auto shaped = ch.shapers[b].tick (parts[b], [curve, g] (double x) { return shape (curve, x * g) / g; });

                // The offset off, and the band as it was from the same moment.
                const auto blocked = shaped - ch.blockIn[b] + blocker * ch.blockOut[b];
                ch.blockIn[b] = shaped;
                ch.blockOut[b] = blocked;
                shaped = blocked;

                auto& delayed = ch.dry[b][(size_t) dryPos];
                const auto was = delayed;
                delayed = parts[b];
                sum += wet[b] * shaped + (1.0 - wet[b]) * was;
            }

            data[c][i] = (float) (sum * output);
        }

        if (++dryPos == Oversampler4::latency)
            dryPos = 0;
    }
}

AudioEdits::Snapshot apply (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings)
{
    if (! isValid (settings, sampleRate))
        return nullptr;

    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (start == end || end - start > std::numeric_limits<int>::max() / 4)
        return nullptr;

    const auto prime = (int) juce::jmin (start, (juce::int64) std::ceil (sampleRate));

    return AudioEdits::processRange (s, start - prime, end, [&settings, sampleRate, prime] (juce::AudioBuffer<float>& b)
    {
        const auto channels = b.getNumChannels(), frames = b.getNumSamples();
        Core core;
        core.prepare (channels, sampleRate);
        core.set (settings);

        juce::AudioBuffer<float> work (channels, frames + Core::latency);
        work.clear();

        for (int ch = 0; ch < channels; ++ch)
            work.copyFrom (ch, 0, b, ch, 0, frames);

        core.process (work.getArrayOfWritePointers(), channels, work.getNumSamples());

        for (int ch = 0; ch < channels; ++ch)
            b.copyFrom (ch, prime, work, ch, Core::latency + prime, frames - prime);
    });
}
}
