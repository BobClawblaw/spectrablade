#pragma once

#include "Recorder.h"
#include "Session.h"

/** Puts what the recorders took onto session tracks, after recording several tracks at once. */
namespace SessionRecording
{
    /** Saves the audio a take lands with, named after its track; returns the file, or sets error. */
    using Save = std::function<juce::File (const AudioDocument::Snapshot&, const juce::String& trackName, juce::String& error)>;

    struct Result
    {
        int landed = 0;          // tracks that got a take
        int mostPasses = 0;      // the most passes any take had (loop recording)
        juce::String error;      // from saving; nothing is landed then
    };

    /** Each track (its id, or 0 for a new track) gets the take of the recorder it recorded from,
        placed over [punchIn, punchOut) with the round-trip latency: a clip, or with a loop's
        several complete passes, a clip with takes. Each recorder's take is saved once, so tracks
        recording from the same input share its file. */
    Result land (SessionState&, const std::vector<std::pair<juce::uint32, Recorder*>>& tracks,
                 const std::map<Recorder*, Recorder::Take>& takes, juce::int64 punchIn, juce::int64 punchOut,
                 int roundTripLatency, bool looped, const Save&);

    /** The undo step's name: "Record", "Loop Record (3 takes)", "Record 2 Tracks" or "Loop Record 2 Tracks". */
    juce::String describe (const Result&, bool looped);

    /** Pre-roll and post-roll: how long playback runs before the punch-in and after the punch-out. */
    struct Rolls
    {
        double preroll = 2.0, postroll = 0.25;   // seconds

        static constexpr double longest = 10.0;

        /** Where playback starts for a punch-in: the pre-roll before it, not before the start. */
        juce::int64 startFrame (juce::int64 punchIn, double rate) const;

        /** Whether recording should stop: the frame that has come back through the input is the
            post-roll past the punch-out. Never without a punch-out (punchOut < 0). */
        bool isDone (juce::int64 frameBack, juce::int64 punchOut, double rate) const;
    };
}
