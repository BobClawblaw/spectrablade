#include "Reverb.h"

namespace RoomReverb
{
namespace
{
    constexpr double maxPreDelayMs = 200.0, maxSize = 1.5;

    // The lines' lengths at 100 %, in ms: spread out and sharing no simple ratios, so their echoes
    // don't pile up on each other.
    constexpr std::array<double, Core::numLines> lineMs { 29.7, 33.3, 37.9, 41.3, 47.9, 53.1, 61.7, 71.9 };

    // The diffusers: all-passes of these lengths in ms, each with this coefficient.
    constexpr std::array<double, 4> diffuserMs { 4.77, 3.59, 12.73, 9.31 };
    constexpr double diffusion = 0.6;

    // How the sound goes into each line, and how the lines make the left and right sides: rows of a
    // Hadamard matrix, so the two sides are uncorrelated mixes of the same lines.
    constexpr std::array<double, Core::numLines> inSigns { 1, -1, 1, -1, 1, -1, 1, -1 };
    constexpr std::array<double, Core::numLines> leftSigns { 1, 1, 1, 1, 1, 1, 1, 1 };
    constexpr std::array<double, Core::numLines> rightSigns { 1, 1, -1, -1, 1, 1, -1, -1 };

    bool inRange (double v, double low, double high)    { return std::isfinite (v) && v >= low && v <= high; }

    int frames (double ms, double rate)                  { return juce::jmax (1, (int) std::lround (ms / 1000.0 * rate)); }
}

bool isValid (const Settings& s)
{
    return inRange (s.preDelayMs, 0.0, maxPreDelayMs) && inRange (s.decaySeconds, 0.1, 20.0) && inRange (s.sizePercent, 50.0, 150.0)
        && inRange (s.dampingPercent, 0.0, 100.0) && inRange (s.widthPercent, 0.0, 100.0) && inRange (s.mixPercent, 0.0, 100.0);
}

double tailSeconds (const Settings& s)
{
    return s.preDelayMs / 1000.0 + s.decaySeconds;
}

void Core::prepare (double sampleRate)
{
    rate = sampleRate;
    preDelay.assign ((size_t) frames (maxPreDelayMs, rate) + 1, 0.0f);

    for (size_t i = 0; i < diffusers.size(); ++i)
        diffusers[i].line.assign ((size_t) frames (diffuserMs[i], rate), 0.0f);

    for (size_t i = 0; i < lines.size(); ++i)
        lines[i].assign ((size_t) frames (lineMs[i] * maxSize, rate), 0.0f);

    reset();
    set (settings);
}

void Core::reset()
{
    std::fill (preDelay.begin(), preDelay.end(), 0.0f);
    preWrite = 0;

    for (auto& d : diffusers)
    {
        std::fill (d.line.begin(), d.line.end(), 0.0f);
        d.pos = 0;
    }

    for (auto& l : lines)
        std::fill (l.begin(), l.end(), 0.0f);

    writes.fill (0);
    lows.fill (0.0);
}

void Core::set (const Settings& s)
{
    settings = s;
    preFrames = juce::jlimit (0, (int) preDelay.size() - 1, (int) std::lround (s.preDelayMs / 1000.0 * rate));
    damping = s.dampingPercent / 100.0 * 0.9;

    for (size_t i = 0; i < lines.size(); ++i)
    {
        lengths[i] = juce::jlimit (1, juce::jmax (1, (int) lines[i].size()), frames (lineMs[i] * s.sizePercent / 100.0, rate));
        gains[i] = std::pow (10.0, -3.0 * lengths[i] / (s.decaySeconds * rate));   // -60 dB over the decay time
        writes[i] %= juce::jmax (1, lengths[i]);
    }
}

void Core::process (float* const* channels, int numChannels, int numFrames) noexcept
{
    if (numChannels <= 0 || lines[0].empty())
        return;

    const auto wet = settings.mixPercent / 100.0, dry = 1.0 - wet;
    const auto width = settings.widthPercent / 100.0;
    const auto scale = 1.0 / std::sqrt ((double) numLines);
    const auto preLength = (int) preDelay.size();

    for (int i = 0; i < numFrames; ++i)
    {
        // In: the channels' average, through the pre-delay and the diffusers.
        double in = 0.0;

        for (int ch = 0; ch < numChannels; ++ch)
            in += channels[ch][i];

        in /= numChannels;
        preDelay[(size_t) preWrite] = (float) in;
        auto read = preWrite - preFrames;
        read += read < 0 ? preLength : 0;
        auto u = (double) preDelay[(size_t) read];
        preWrite = preWrite + 1 == preLength ? 0 : preWrite + 1;

        for (auto& d : diffusers)
        {
            // An all-pass: delayed + c * (in - c * delayed) form, which keeps every frequency's level.
            const auto delayed = (double) d.line[(size_t) d.pos];
            const auto v = u + diffusion * delayed;
            d.line[(size_t) d.pos] = (float) v;
            u = delayed - diffusion * v;
            d.pos = d.pos + 1 == (int) d.line.size() ? 0 : d.pos + 1;
        }

        // The lines' outputs, damped; mixed by the Householder matrix (I - 2/N), which loses nothing.
        std::array<double, numLines> out {};
        double sum = 0.0, left = 0.0, right = 0.0;

        for (int k = 0; k < numLines; ++k)
        {
            out[(size_t) k] = (double) lines[(size_t) k][(size_t) writes[(size_t) k]];
            lows[(size_t) k] = (1.0 - damping) * out[(size_t) k] + damping * lows[(size_t) k];
            sum += lows[(size_t) k];
            left += leftSigns[(size_t) k] * out[(size_t) k];
            right += rightSigns[(size_t) k] * out[(size_t) k];
        }

        for (int k = 0; k < numLines; ++k)
        {
            const auto fed = lows[(size_t) k] - 2.0 / numLines * sum;
            auto& w = writes[(size_t) k];
            lines[(size_t) k][(size_t) w] = (float) (gains[(size_t) k] * fed + inSigns[(size_t) k] * scale * u);
            w = w + 1 >= lengths[(size_t) k] ? 0 : w + 1;
        }

        // Out: the two sides, narrowed toward their middle by the width.
        left *= scale;
        right *= scale;
        const auto mid = 0.5 * (left + right), side = 0.5 * (left - right) * width;
        const auto sides = std::array<double, 2> { mid + side, mid - side };

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto reverb = numChannels == 1 ? mid : sides[(size_t) (ch % 2)];
            channels[ch][i] = (float) (dry * channels[ch][i] + wet * reverb);
        }
    }
}

AudioEdits::Snapshot apply (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings)
{
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (! isValid (settings) || sampleRate <= 0.0 || start == end)
        return nullptr;

    const auto tailEnd = juce::jmin (s.getNumFrames(), end + (juce::int64) std::ceil (tailSeconds (settings) * sampleRate));

    if (tailEnd - start > std::numeric_limits<int>::max())
        return nullptr;

    const auto inside = (int) (end - start);

    return AudioEdits::processRange (s, start, tailEnd, [&settings, sampleRate, inside] (juce::AudioBuffer<float>& b)
    {
        Core core;
        core.prepare (sampleRate);
        core.set (settings);
        core.process (b.getArrayOfWritePointers(), b.getNumChannels(), inside);

        // The tail: silence in, the reverb added on top of what is there.
        const auto rest = b.getNumSamples() - inside;

        if (rest <= 0)
            return;

        juce::AudioBuffer<float> tail (b.getNumChannels(), rest);
        tail.clear();
        auto settingsOut = settings;
        settingsOut.mixPercent = 100.0;
        core.set (settingsOut);
        core.process (tail.getArrayOfWritePointers(), tail.getNumChannels(), rest);
        const auto wet = (float) (settings.mixPercent / 100.0);

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            b.addFrom (ch, inside, tail, ch, 0, rest, wet);
    });
}
}
