#include "DialogueContour.h"

namespace DialogueContour
{
bool isValid (const Settings& s)
{
    return s.semitones >= -12.0f && s.semitones <= 12.0f && s.endingSeconds >= 0.1f && s.endingSeconds <= 1.5f && s.curve >= -1.0f && s.curve <= 1.0f;
}

std::vector<double> shiftsFor (const PitchCorrection::Track& track, juce::int64 start, juce::int64 end, double sampleRate, const Settings& s)
{
    const auto frames = (int) track.hz.size();
    std::vector<double> shift ((size_t) frames, 0.0);

    if (frames == 0 || track.hop <= 0 || ! isValid (s))
        return shift;

    // The last pitched frame inside the range, and the ending's first frame: back from it by the
    // length asked for, or to the start of its run of pitched frames (a gap of over 100 ms ends a run).
    const auto frameOf = [&track] (juce::int64 frame) { return (int) ((frame - track.first) / track.hop); };
    const auto rangeFirst = juce::jlimit (0, frames - 1, frameOf (start)), rangeLast = juce::jlimit (0, frames - 1, frameOf (end - 1));
    auto last = -1;

    for (int f = rangeLast; f >= rangeFirst; --f)
        if (track.hz[(size_t) f] > 0.0f)
        {
            last = f;
            break;
        }

    if (last < 0)
        return shift;

    const auto endingFrames = juce::jmax (1, (int) std::lround (s.endingSeconds * sampleRate / track.hop));
    const auto gapFrames = juce::jmax (1, (int) std::lround (0.1 * sampleRate / track.hop));
    auto first = last;
    auto unpitched = 0;

    while (first > rangeFirst && last - first < endingFrames)
    {
        if (track.hz[(size_t) first - 1] > 0.0f)
            unpitched = 0;
        else if (++unpitched > gapFrames)
            break;

        --first;
    }

    while (first < last && track.hz[(size_t) first] <= 0.0f)
        ++first;

    // The ramp over the ending, bent by the curve: t to a power from a half (early) to two (late).
    const auto power = std::pow (2.0, (double) s.curve);
    const auto span = juce::jmax (1, last - first);

    for (int f = first; f <= last; ++f)
        if (track.hz[(size_t) f] > 0.0f)
            shift[(size_t) f] = 100.0 * s.semitones * std::pow ((double) (f - first) / span, power);

    return shift;
}

AudioDocument::Snapshot apply (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate, const Settings& s,
                               const std::function<bool (float)>& progress)
{
    if (! isValid (s) || sampleRate <= 0.0)
        return nullptr;

    start = juce::jlimit ((juce::int64) 0, snapshot.getNumFrames(), start);
    end = juce::jlimit (start, snapshot.getNumFrames(), end);

    if (start == end)
        return nullptr;

    const auto track = PitchCorrection::analyse (snapshot, start, end, sampleRate, [&] (float p) { return progress == nullptr || progress (0.5f * p); });

    if (track.hz.empty())
        return nullptr;

    auto shift = shiftsFor (track, start, end, sampleRate, s);
    auto any = false;

    for (auto v : shift)
        any = any || ! juce::exactlyEqual (v, 0.0);

    if (! any && ! juce::exactlyEqual (s.semitones, 0.0f))
        return nullptr;

    return PitchCorrection::shiftBy (snapshot, start, end, sampleRate, track, std::move (shift), [&] (float p) { return progress == nullptr || progress (0.5f + 0.5f * p); });
}
}
