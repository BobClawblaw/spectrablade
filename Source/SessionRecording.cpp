#include "SessionRecording.h"

namespace SessionRecording
{
Result land (SessionState& state, const std::vector<std::pair<juce::uint32, Recorder*>>& tracks,
             const std::map<Recorder*, Recorder::Take>& takes, juce::int64 punchIn, juce::int64 punchOut,
             int roundTripLatency, bool looped, const Save& save)
{
    struct Saved
    {
        juce::File file;
        AudioDocument::Snapshot audio;
        std::vector<Recorder::Placement> passes;
    };

    std::map<Recorder*, Saved> saved;
    Result result;
    auto next = state;

    for (auto& [trackId, recorder] : tracks)
    {
        const auto found = takes.find (recorder);

        if (found == takes.end())
            continue;

        auto track = -1;

        for (int t = 0; t < (int) next.tracks.size(); ++t)
            if (trackId != 0 && next.tracks[(size_t) t].id == trackId)
                track = t;

        if (saved.count (recorder) == 0)
        {
            const auto& take = found->second;
            auto& entry = saved[recorder];
            entry.passes = looped ? Recorder::placeLoop (take, punchIn, punchOut, roundTripLatency)
                                  : std::vector<Recorder::Placement> { Recorder::place (take, punchIn, punchOut, roundTripLatency) };

            if (entry.passes.empty() || entry.passes.front().takeEnd <= entry.passes.front().takeStart)
                continue;

            // From the first pass's start to the last one's end: every pass, in one file.
            const AudioSnapshot takeAudio (take.clip.numChannels, take.clip.segments);
            const auto clip = AudioEdits::copy (takeAudio, entry.passes.front().takeStart, entry.passes.back().takeEnd, take.clip.sampleRate);
            entry.audio = std::make_shared<const AudioSnapshot> (clip.numChannels, clip.segments);
            entry.file = save (entry.audio, track >= 0 ? next.tracks[(size_t) track].name : juce::String ("Take"), result.error);

            if (result.error.isNotEmpty())
                return { 0, 0, result.error };
        }

        const auto& entry = saved[recorder];

        if (entry.audio == nullptr)
            continue;

        const auto& first = entry.passes.front();

        if (looped && entry.passes.size() > 1)
        {
            std::vector<juce::int64> offsets;

            for (auto& pass : entry.passes)
                offsets.push_back (pass.takeStart - first.takeStart);

            SessionEdits::recordTakes (next, track, entry.file, entry.audio, first.destStart, punchOut - punchIn, offsets);
        }
        else
        {
            SessionEdits::recordClip (next, track, entry.file, entry.audio, first.destStart);
        }

        ++result.landed;
        result.mostPasses = juce::jmax (result.mostPasses, (int) entry.passes.size());
    }

    state = std::move (next);
    return result;
}

juce::int64 Rolls::startFrame (juce::int64 punchIn, double rate) const
{
    return juce::jmax ((juce::int64) 0, punchIn - (juce::int64) std::llround (juce::jlimit (0.0, longest, preroll) * rate));
}

bool Rolls::isDone (juce::int64 frameBack, juce::int64 punchOut, double rate) const
{
    return punchOut >= 0 && frameBack >= punchOut + (juce::int64) std::llround (juce::jlimit (0.0, longest, postroll) * rate);
}

juce::String describe (const Result& result, bool looped)
{
    const auto asTakes = looped && result.mostPasses > 1;

    if (result.landed > 1)
        return juce::String (asTakes ? "Loop Record " : "Record ") + juce::String (result.landed) + " Tracks";

    return asTakes ? "Loop Record (" + juce::String (result.mostPasses) + " takes)" : juce::String ("Record");
}
}
