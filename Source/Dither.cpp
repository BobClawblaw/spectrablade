#include "Dither.h"

namespace Dither
{
const juce::StringArray& shapingNames()
{
    static const juce::StringArray names { "None", "Light", "Strong" };
    return names;
}

bool isValid (const Settings& s)
{
    return s.bits == 16 || s.bits == 24;
}

double stepFor (int bits)
{
    return std::pow (2.0, 1 - bits);
}

AudioEdits::Snapshot apply (const AudioSnapshot& s, juce::int64 start, juce::int64 end, const Settings& settings)
{
    if (! isValid (settings))
        return nullptr;

    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (start == end || end - start > std::numeric_limits<int>::max() / 2)
        return nullptr;

    return AudioEdits::processRange (s, start, end, [&settings] (juce::AudioBuffer<float>& b)
    {
        const auto step = stepFor (settings.bits);
        const auto top = 1.0 - step;   // the largest value the depth holds
        const auto order = settings.shaping == Shaping::none ? 0 : settings.shaping == Shaping::light ? 2 : 3;
        const double weights[3] = { order == 2 ? 2.0 : 3.0, order == 2 ? -1.0 : -3.0, 1.0 };   // 1 - (weights) = (1 - z^-1) to the order

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            juce::Random random (0x5d17 + ch);   // the same noise for the same audio
            auto* x = b.getWritePointer (ch);
            double errors[3] = { 0.0, 0.0, 0.0 };   // the last rounding errors, newest first

            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                // The sample less the fed-back errors, plus triangular dither of a step either way.
                auto v = (double) x[i];

                for (int k = 0; k < order; ++k)
                    v -= weights[k] * errors[k];

                const auto dither = (random.nextDouble() + random.nextDouble() - 1.0) * step;
                const auto wanted = v + dither;
                const auto rounded = juce::jlimit (-1.0, top, std::round (wanted / step) * step);

                // The error shaped is all the rounding and the dither added.
                errors[2] = errors[1];
                errors[1] = errors[0];
                errors[0] = rounded - v;
                x[i] = (float) rounded;
            }
        }
    });
}
}
