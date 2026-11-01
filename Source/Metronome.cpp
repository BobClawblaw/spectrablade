#include "Metronome.h"

namespace Metronome
{
namespace
{
    double beatLength (double tempo, double rate)
    {
        return 60.0 * rate / juce::jlimit (1.0, 1000.0, tempo);
    }
}

juce::int64 beatFrame (juce::int64 beat, double tempo, double rate)
{
    return (juce::int64) std::llround ((double) beat * beatLength (tempo, rate));
}

juce::int64 beatAt (juce::int64 frame, double tempo, double rate)
{
    auto beat = (juce::int64) std::floor ((double) frame / beatLength (tempo, rate));

    // The rounding of beat starts can put the frame on either side of the estimate.
    while (beatFrame (beat + 1, tempo, rate) <= frame)
        ++beat;

    while (beatFrame (beat, tempo, rate) > frame)
        --beat;

    return beat;
}

juce::int64 barFrames (double tempo, int beatsPerBar, double rate)
{
    return beatFrame (juce::jmax (1, beatsPerBar), tempo, rate);
}

float sample (juce::int64 offset, bool accent, double rate)
{
    const auto length = (juce::int64) std::llround (clickSeconds * rate);

    if (offset < 0 || offset >= length)
        return 0.0f;

    const auto frequency = accent ? 1760.0 : 1320.0;
    const auto level = accent ? 0.5 : 0.35;
    const auto fade = 1.0 - (double) offset / (double) length;
    return (float) (level * fade * fade * std::sin (juce::MathConstants<double>::twoPi * frequency * (double) offset / rate));
}
}
