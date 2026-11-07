#pragma once

#include <JuceHeader.h>
#include "AudioDocument.h"
#include "Sequences.h"

/** Previewing an effect that can't run live: its settings processed in the background on part of
    the audio, for the player to play in place of the document (DocumentSource::setStandIn).

    Only the newest request counts: one waiting is dropped when another comes, and one under way
    is stopped at its next progress check; only the newest result reaches its callback. */
class EffectPreview
{
public:
    /** The most a preview processes from the start of the range, unless it needs the whole range. */
    static constexpr double longestSeconds = 30.0;

    struct Rendered
    {
        AudioDocument::Snapshot snapshot;    // the whole audio, the range processed; null: left as it was
        juce::int64 start = 0, end = 0;      // where the processed range is now (a stretch moves its end)
        juce::String error;
    };

    /** Processes [start, end) of the audio; progress gets 0 to 1 and returns false to stop. */
    using Render = std::function<Rendered (const AudioDocument::Snapshot&, juce::int64 start, juce::int64 end, double sampleRate,
                                           const std::function<bool (float)>& progress)>;

    struct Request
    {
        juce::String key;          // the settings, so the same ones aren't processed again
        Render render;
        bool wholeRange = false;   // processes all of the range (the result depends on all of it)
    };

    /** A sequence step as a request (wholeRange as given), or nothing when the step can't run
        with these settings (Sequences::problem). */
    static std::optional<Request> ofStep (const Sequences::Step&, double sampleRate, bool wholeRange = false);

    /** What a request processes of a range: the range's first longestSeconds, or all of it. */
    static juce::Range<juce::int64> partOf (juce::Range<juce::int64> range, double sampleRate, bool wholeRange);

    EffectPreview();
    ~EffectPreview();   // waits for a render under way (stopped at its next progress check)

    /** Processes the request on part of the range (partOf) on the preview's own thread, then calls
        done on the message thread, unless a newer request or cancel() came first. */
    void render (const Request&, AudioDocument::Snapshot, juce::Range<juce::int64> range, double sampleRate,
                 std::function<void (const Rendered&)> done);

    /** Drops the request waiting or under way: its callback isn't called. */
    void cancel();

    /** A request is waiting or under way. */
    bool isBusy() const noexcept     { return state->answered.load() != state->newest.load(); }

private:
    struct State
    {
        std::atomic<juce::uint32> newest { 0 }, answered { 0 };   // the newest request, and the last one answered or dropped
    };

    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withThreadName ("Effect Preview").withNumberOfThreads (1) };
    std::shared_ptr<State> state = std::make_shared<State>();   // outlives the preview in callbacks on their way

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectPreview)
};
