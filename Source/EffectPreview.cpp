#include "EffectPreview.h"

std::optional<EffectPreview::Request> EffectPreview::ofStep (const Sequences::Step& step, double sampleRate, bool wholeRange)
{
    if (Sequences::problem (step, sampleRate).isNotEmpty())
        return std::nullopt;

    juce::String key = step.kind;

    for (const auto& [id, value] : step.values)
        key << " " << id << "=" << juce::String (value, 9);

    return Request { key,
                     [step] (const AudioDocument::Snapshot& audio, juce::int64 start, juce::int64 end, double rate, const std::function<bool (float)>& progress)
                     {
                         const auto r = Sequences::apply (step, audio, start, end, rate, {}, nullptr, progress);
                         return Rendered { r.snapshot, r.start, r.end, r.error };
                     },
                     wholeRange };
}

juce::Range<juce::int64> EffectPreview::partOf (juce::Range<juce::int64> range, double sampleRate, bool wholeRange)
{
    if (wholeRange)
        return range;

    const auto most = (juce::int64) std::llround (longestSeconds * sampleRate);
    return range.withLength (juce::jmin (range.getLength(), most));
}

EffectPreview::EffectPreview() = default;

EffectPreview::~EffectPreview()
{
    cancel();
    pool.removeAllJobs (true, 60000);
}

void EffectPreview::render (const Request& request, AudioDocument::Snapshot audio, juce::Range<juce::int64> range, double sampleRate,
                            std::function<void (const Rendered&)> done)
{
    const auto mine = ++state->newest;
    const auto part = partOf (range, sampleRate, request.wholeRange);

    pool.removeAllJobs (false, 0);   // drops one waiting; one under way stops at its next progress check

    pool.addJob ([s = state, mine, process = request.render, whole = std::move (audio), part, sampleRate, then = std::move (done)]
    {
        const auto current = [s, mine] { return s->newest.load() == mine; };

        if (! current())
            return;

        auto result = process (whole, part.getStart(), part.getEnd(), sampleRate, [current] (float) { return current(); });

        juce::MessageManager::callAsync ([s, mine, rendered = std::move (result), then]
        {
            if (s->newest.load() != mine)
                return;

            s->answered = mine;
            then (rendered);
        });
    });
}

void EffectPreview::cancel()
{
    state->answered = ++state->newest;
    pool.removeAllJobs (false, 0);
}
