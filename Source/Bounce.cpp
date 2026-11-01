#include "Bounce.h"

namespace Bounce
{
SessionState isolate (const SessionState& s, juce::uint32 trackId)
{
    auto alone = s;
    alone.tracks.clear();

    if (const auto* track = s.findTrack (trackId))
    {
        auto t = *track;
        t.sends.clear();
        t.mute = false;
        t.solo = false;
        alone.tracks.push_back (std::move (t));
    }

    return alone;
}

juce::uint32 land (SessionState& s, juce::uint32 trackId, const juce::File& file, AudioDocument::Snapshot audio, juce::int64 position)
{
    const auto source = std::find_if (s.tracks.begin(), s.tracks.end(), [trackId] (auto& t) { return t.id == trackId; });

    if (source == s.tracks.end())
        return 0;

    const auto index = (int) std::distance (s.tracks.begin(), source);
    const auto name = source->name + " Bounce";
    const auto sends = source->sends;
    source->mute = true;

    auto& added = s.addTrack();
    added.name = name;
    const auto id = added.id;
    SessionEdits::addClip (s, (int) s.tracks.size() - 1, file, std::move (audio), position);

    for (auto& send : sends)
        SessionEdits::setSend (s, id, send);

    // From the end, where addTrack put it, to just after the source.
    std::rotate (s.tracks.begin() + index + 1, s.tracks.end() - 1, s.tracks.end());
    return id;
}

std::vector<juce::uint32> stemTracks (const SessionState& s)
{
    std::vector<juce::uint32> ids;

    for (auto& t : s.tracks)
        if (! t.isBus && ! t.clips.empty())
            ids.push_back (t.id);

    return ids;
}

juce::String stemFileName (const juce::String& sessionName, int number, const juce::String& trackName)
{
    return juce::File::createLegalFileName (sessionName + " - " + juce::String (number).paddedLeft ('0', 2) + " " + trackName) + ".wav";
}

std::vector<std::pair<juce::Range<juce::int64>, juce::File>> regionFiles (const SessionState& s, const juce::String& sessionName,
                                                                           const juce::File& folder)
{
    std::vector<std::pair<juce::Range<juce::int64>, juce::File>> files;

    for (auto& m : s.markers)
        if (m.isRegion())
            files.emplace_back (juce::Range<juce::int64> (m.start, m.getEnd()),
                                folder.getChildFile (stemFileName (sessionName, (int) files.size() + 1, m.name)));

    return files;
}
}
