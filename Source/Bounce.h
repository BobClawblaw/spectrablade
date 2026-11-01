#pragma once

#include "Session.h"

/** Bouncing a track: rendering it as it goes to the mix, through its effects, volume, pan and
    automation, into a file that replaces it on a track of its own. */
namespace Bounce
{
    /** The session with only this track in it, sends removed, unmuted and unsoloed: mixed, it
        gives what the track adds to the mix itself (not through buses). */
    SessionState isolate (const SessionState&, juce::uint32 trackId);

    /** Puts a bounce into the session: a new track right after the source, named after it, with
        the audio as one clip at position (the file is the clip's source) and the source's sends,
        which now carry the bounced audio; the source is muted.
        Returns the new track's id, or 0 when the source track is no longer there. */
    juce::uint32 land (SessionState&, juce::uint32 trackId, const juce::File&, AudioDocument::Snapshot, juce::int64 position);
}
