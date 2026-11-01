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
        the audio as one clip at position (the file is the clip's source), the source's sends,
        which now carry the bounced audio, and the source's folder; the source is muted.
        Returns the new track's id, or 0 when the source track is no longer there. */
    juce::uint32 land (SessionState&, juce::uint32 trackId, const juce::File&, AudioDocument::Snapshot, juce::int64 position);

    /** The tracks (not buses) that have clips, in the session's order: what Export Stems writes. */
    std::vector<juce::uint32> stemTracks (const SessionState&);

    /** A folder and what is in it alone, as Export Folder Stems renders it: its tracks and buses
        as they are set (their own mutes too), sending only to buses in the folder, nothing
        soloed, and the folder's volume but not its mute. */
    SessionState isolateFolder (const SessionState&, juce::uint32 folderId);

    /** The folders with a track with clips in them, in the session's order: what Export Folder
        Stems writes. */
    std::vector<juce::uint32> stemFolders (const SessionState&);

    /** A stem's file name: "Song - 01 Guitar.wav", numbered by the track's place among the stems
        so names stay apart and sort in order, with characters files can't hold left out. */
    juce::String stemFileName (const juce::String& sessionName, int number, const juce::String& trackName);

    /** What Export Regions writes: each region of the session in time order, with its file in
        the folder, named as stems are ("Song - 01 Verse.wav"), numbered by its place among the
        regions. Point markers are left out. */
    std::vector<std::pair<juce::Range<juce::int64>, juce::File>> regionFiles (const SessionState&, const juce::String& sessionName,
                                                                               const juce::File& folder);

    /** The same for any markers, such as an open file's for the editor's Save Each Region As. */
    std::vector<std::pair<juce::Range<juce::int64>, juce::File>> regionFiles (const Markers&, const juce::String& name,
                                                                               const juce::File& folder);
}
