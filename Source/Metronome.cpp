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

namespace
{
    juce::int64 floorDiv (juce::int64 a, juce::int64 b)
    {
        return a >= 0 ? a / b : -((-a + b - 1) / b);
    }
}

template <typename Before>
TempoMap::Segment TempoMap::find (Before before) const
{
    Segment s { 0, 0, 0.0, beatLength (startTempo, rate), juce::jmax (1, startBeats) };

    for (auto& c : changes)
    {
        if (c.bar <= s.bar)
            continue;

        const auto beats = (c.bar - s.bar) * s.beatsPerBar;
        const Segment next { c.bar, s.beat + beats, s.frame + (double) beats * s.beatLength, beatLength (c.tempo, rate), juce::jmax (1, c.beatsPerBar) };

        if (before (next))
            break;

        s = next;
    }

    return s;
}

juce::int64 TempoMap::beatFrame (juce::int64 beat) const
{
    const auto s = find ([beat] (const Segment& next) { return beat < next.beat; });
    return (juce::int64) std::llround (s.frame + (double) (beat - s.beat) * s.beatLength);
}

juce::int64 TempoMap::beatAt (juce::int64 frame) const
{
    const auto s = find ([frame] (const Segment& next) { return frame < startOf (next); });
    auto beat = s.beat + (juce::int64) std::floor (((double) frame - s.frame) / s.beatLength);

    // The rounding of beat starts can put the frame on either side of the estimate.
    while (beatFrame (beat + 1) <= frame)
        ++beat;

    while (beatFrame (beat) > frame)
        --beat;

    return beat;
}

std::pair<juce::int64, int> TempoMap::barAndBeat (juce::int64 beat) const
{
    const auto s = find ([beat] (const Segment& next) { return beat < next.beat; });
    const auto bars = floorDiv (beat - s.beat, s.beatsPerBar);
    return { s.bar + bars, (int) (beat - s.beat - bars * s.beatsPerBar) };
}

juce::int64 TempoMap::barStartBeat (juce::int64 bar) const
{
    const auto s = find ([bar] (const Segment& next) { return bar < next.bar; });
    return s.beat + (bar - s.bar) * s.beatsPerBar;
}

double TempoMap::tempoAt (juce::int64 frame) const
{
    const auto s = find ([frame] (const Segment& next) { return frame < startOf (next); });
    return 60.0 * rate / s.beatLength;
}

int TempoMap::beatsPerBarAt (juce::int64 frame) const
{
    return find ([frame] (const Segment& next) { return frame < startOf (next); }).beatsPerBar;
}

double TempoMap::shortestBeat() const
{
    auto shortest = beatLength (startTempo, rate);

    for (auto& c : changes)
        if (c.bar > 0)
            shortest = juce::jmin (shortest, beatLength (c.tempo, rate));

    return shortest;
}

double TempoMap::shortestBar() const
{
    auto shortest = beatLength (startTempo, rate) * juce::jmax (1, startBeats);

    for (auto& c : changes)
        if (c.bar > 0)
            shortest = juce::jmin (shortest, beatLength (c.tempo, rate) * juce::jmax (1, c.beatsPerBar));

    return shortest;
}

std::pair<juce::int64, juce::int64> TempoMap::linesAround (juce::int64 frame, Grid grid) const
{
    if (grid.bars <= 0)
    {
        const auto beat = beatAt (frame);
        return { beatFrame (beat), beatFrame (beat + 1) };
    }

    const auto below = floorDiv (barAt (frame), grid.bars) * grid.bars;
    return { barFrame (below), barFrame (below + grid.bars) };
}

void TempoMap::forEachLine (juce::int64 first, juce::int64 last, Grid grid,
                            const std::function<void (juce::int64, juce::int64)>& line) const
{
    if (grid.bars <= 0)
    {
        for (auto beat = beatAt (first); beatFrame (beat) <= last; ++beat)
            if (beatFrame (beat) >= first)
                line (beatFrame (beat), beat);

        return;
    }

    for (auto bar = floorDiv (barAt (first), grid.bars) * grid.bars; barFrame (bar) <= last; bar += grid.bars)
        if (barFrame (bar) >= first)
            line (barFrame (bar), barStartBeat (bar));
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
