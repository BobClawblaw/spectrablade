#include "Session.h"
#include "AudioFileIO.h"
#include "SampleRateConverter.h"

//==============================================================================
juce::int64 SessionState::getLength() const
{
    juce::int64 length = 0;

    for (auto& t : tracks)
        for (auto& c : t.clips)
            length = juce::jmax (length, c.getEnd());

    return length;
}

bool SessionState::anySolo() const
{
    return std::any_of (tracks.begin(), tracks.end(), [] (const SessionTrack& t) { return t.solo; });
}

SessionTrack& SessionState::addTrack()
{
    SessionTrack t;
    t.id = nextId++;
    t.name = "Track " + juce::String ((int) tracks.size() + 1);
    tracks.push_back (std::move (t));
    return tracks.back();
}

std::optional<std::pair<int, int>> SessionState::findClip (juce::uint32 clipId) const
{
    for (size_t t = 0; t < tracks.size(); ++t)
        for (size_t c = 0; c < tracks[t].clips.size(); ++c)
            if (tracks[t].clips[c].id == clipId)
                return std::pair<int, int> { (int) t, (int) c };

    return std::nullopt;
}

//==============================================================================
namespace SessionEdits
{
namespace
{
    SessionClip* find (SessionState& s, juce::uint32 clipId)
    {
        const auto where = s.findClip (clipId);
        return where.has_value() ? &s.tracks[(size_t) where->first].clips[(size_t) where->second] : nullptr;
    }

    void limitFades (SessionClip& c)
    {
        c.fadeIn = juce::jlimit ((juce::int64) 0, c.length, c.fadeIn);
        c.fadeOut = juce::jlimit ((juce::int64) 0, c.length - c.fadeIn, c.fadeOut);
    }
}

juce::uint32 addClip (SessionState& s, int track, const juce::File& source, AudioDocument::Snapshot audio, juce::int64 position)
{
    if (track < 0 || track >= (int) s.tracks.size())
    {
        s.addTrack();
        track = (int) s.tracks.size() - 1;
    }

    SessionClip c;
    c.id = s.nextId++;
    c.name = source.getFileNameWithoutExtension();
    c.source = source;
    c.length = audio != nullptr ? audio->getNumFrames() : 0;
    c.audio = std::move (audio);
    c.position = juce::jmax ((juce::int64) 0, position);
    s.tracks[(size_t) track].clips.push_back (std::move (c));
    return s.tracks[(size_t) track].clips.back().id;
}

bool moveClip (SessionState& s, juce::uint32 clipId, int toTrack, juce::int64 position)
{
    const auto where = s.findClip (clipId);

    if (! where.has_value() || toTrack < 0)
        return false;

    auto& from = s.tracks[(size_t) where->first].clips;
    auto clip = from[(size_t) where->second];
    clip.position = juce::jmax ((juce::int64) 0, position);

    if (toTrack == where->first)
    {
        from[(size_t) where->second] = std::move (clip);
        return true;
    }

    from.erase (from.begin() + where->second);

    if (toTrack >= (int) s.tracks.size())
    {
        s.addTrack();
        toTrack = (int) s.tracks.size() - 1;
    }

    s.tracks[(size_t) toTrack].clips.push_back (std::move (clip));
    return true;
}

bool trimClip (SessionState& s, juce::uint32 clipId, juce::int64 start, juce::int64 end)
{
    auto* c = find (s, clipId);

    if (c == nullptr || c->audio == nullptr)
        return false;

    // Session frame of audio frame 0; the clip can show any part of the audio from there on.
    const auto audioStart = c->position - c->offset;
    start = juce::jlimit (juce::jmax ((juce::int64) 0, audioStart), audioStart + c->audio->getNumFrames() - 1, start);
    end = juce::jlimit (start + 1, audioStart + c->audio->getNumFrames(), end);

    c->offset = start - audioStart;
    c->position = start;
    c->length = end - start;
    limitFades (*c);
    return true;
}

bool splitClip (SessionState& s, juce::uint32 clipId, juce::int64 frame)
{
    const auto where = s.findClip (clipId);

    if (! where.has_value())
        return false;

    auto& clips = s.tracks[(size_t) where->first].clips;
    auto first = clips[(size_t) where->second];

    if (frame <= first.position || frame >= first.getEnd())
        return false;

    auto second = first;
    second.id = s.nextId++;
    first.length = frame - first.position;
    first.fadeOut = 0;
    second.offset += frame - second.position;
    second.length -= frame - second.position;
    second.position = frame;
    second.fadeIn = 0;
    limitFades (first);
    limitFades (second);

    clips[(size_t) where->second] = std::move (first);
    clips.push_back (std::move (second));
    return true;
}

bool removeClip (SessionState& s, juce::uint32 clipId)
{
    const auto where = s.findClip (clipId);

    if (! where.has_value())
        return false;

    auto& clips = s.tracks[(size_t) where->first].clips;
    clips.erase (clips.begin() + where->second);
    return true;
}

bool removeTrack (SessionState& s, int track)
{
    if (! juce::isPositiveAndBelow (track, (int) s.tracks.size()))
        return false;

    s.tracks.erase (s.tracks.begin() + track);
    return true;
}
}

//==============================================================================
Session::Session()
{
    reset ({}, {});
}

Session::State Session::getState() const
{
    const juce::SpinLock::ScopedLockType sl (lock);
    return current;
}

void Session::setCurrent (const State& state)
{
    // The rate is set before the state, so a reader that sees the new state sees its rate too.
    sampleRate = state->sampleRate;
    State previous;

    {
        const juce::SpinLock::ScopedLockType sl (lock);
        previous = std::exchange (current, state);
    }

    ++version;

    // Never let the last reference to a state (and its clips' audio) drop while the audio
    // thread may hold it.
    if (previous != nullptr && std::find (pendingRelease.begin(), pendingRelease.end(), previous) == pendingRelease.end())
        pendingRelease.push_back (std::move (previous));

    sendChangeMessage();
}

void Session::reset (SessionState initial, const juce::File& f)
{
    for (auto& e : history)
        if (std::find (pendingRelease.begin(), pendingRelease.end(), e.state) == pendingRelease.end())
            pendingRelease.push_back (e.state);

    history.clear();
    history.push_back ({ std::make_shared<const SessionState> (std::move (initial)), {} });
    position = savedPosition = 0;
    effectsChanged = false;
    file = f;
    setCurrent (history.back().state);
}

void Session::apply (SessionState next, const juce::String& description, bool merge)
{
    auto state = std::make_shared<const SessionState> (std::move (next));
    history.erase (history.begin() + position + 1, history.end());

    if (savedPosition > position)
        savedPosition = -1;   // the saved state was undone and is now gone

    if (merge && position > 0 && history.back().description == description && savedPosition != position)
    {
        history.back().state = state;
    }
    else
    {
        history.push_back ({ state, description });
        ++position;
    }

    setCurrent (state);
}

juce::String Session::getUndoDescription() const
{
    return canUndo() ? history[(size_t) position].description : juce::String();
}

juce::String Session::getRedoDescription() const
{
    return canRedo() ? history[(size_t) position + 1].description : juce::String();
}

void Session::undo()
{
    if (canUndo())
        setCurrent (history[(size_t) --position].state);
}

void Session::redo()
{
    if (canRedo())
        setCurrent (history[(size_t) ++position].state);
}

void Session::markEffectsChanged()
{
    effectsChanged = true;
    sendChangeMessage();
}

void Session::markSaved (const juce::File& savedAs)
{
    savedPosition = position;
    effectsChanged = false;
    file = savedAs;
    sendChangeMessage();
}

void Session::releaseUnusedStates()
{
    pendingRelease.erase (std::remove_if (pendingRelease.begin(), pendingRelease.end(),
                                          [] (const State& s) { return s == nullptr || s.use_count() == 1; }),
                          pendingRelease.end());
}

//==============================================================================
namespace SessionFile
{
juce::String save (const SessionState& state, const juce::File& target,
                   const std::function<std::unique_ptr<juce::XmlElement> (juce::uint32 trackId)>& trackEffects)
{
    juce::XmlElement root ("SPECTRABLADE_SESSION");
    root.setAttribute ("version", 1);
    root.setAttribute ("sampleRate", state.sampleRate);

    for (auto& t : state.tracks)
    {
        auto* track = root.createNewChildElement ("TRACK");
        track->setAttribute ("name", t.name);
        track->setAttribute ("gainDb", t.gainDb);
        track->setAttribute ("pan", t.pan);
        track->setAttribute ("mute", t.mute);
        track->setAttribute ("solo", t.solo);

        if (trackEffects != nullptr)
            if (auto effects = trackEffects (t.id))
                track->addChildElement (effects.release());

        for (auto& c : t.clips)
        {
            auto* clip = track->createNewChildElement ("CLIP");
            clip->setAttribute ("name", c.name);
            clip->setAttribute ("file", c.source.getRelativePathFrom (target.getParentDirectory()));
            clip->setAttribute ("absolute", c.source.getFullPathName());
            clip->setAttribute ("position", juce::String (c.position));
            clip->setAttribute ("offset", juce::String (c.offset));
            clip->setAttribute ("length", juce::String (c.length));
            clip->setAttribute ("gainDb", c.gainDb);
            clip->setAttribute ("fadeIn", juce::String (c.fadeIn));
            clip->setAttribute ("fadeOut", juce::String (c.fadeOut));
        }
    }

    juce::TemporaryFile temp (target);

    if (! root.writeTo (temp.getFile()))
        return "Could not write " + temp.getFile().getFullPathName();

    if (! temp.overwriteTargetFileWithTemporary())
        return "Could not replace " + target.getFullPathName();

    return {};
}

Loaded load (const juce::File& file, juce::AudioFormatManager& formats)
{
    Loaded result;
    const auto root = juce::XmlDocument::parse (file);

    if (root == nullptr || ! root->hasTagName ("SPECTRABLADE_SESSION"))
    {
        result.error = file.getFileName() + " is not a SpectraBlade session.";
        return result;
    }

    auto& state = result.state;
    state.sampleRate = root->getDoubleAttribute ("sampleRate", 48000.0);
    std::map<juce::String, AudioDocument::Snapshot> audio;   // by full path: each file read once

    for (auto* trackXml : root->getChildWithTagNameIterator ("TRACK"))
    {
        auto& track = state.addTrack();
        track.name = trackXml->getStringAttribute ("name", track.name);
        track.gainDb = (float) trackXml->getDoubleAttribute ("gainDb");
        track.pan = (float) trackXml->getDoubleAttribute ("pan");
        track.mute = trackXml->getBoolAttribute ("mute");
        track.solo = trackXml->getBoolAttribute ("solo");

        if (auto* effects = trackXml->getChildByName ("EFFECTS"))
            result.effects[track.id] = std::make_shared<juce::XmlElement> (*effects);

        for (auto* clipXml : trackXml->getChildWithTagNameIterator ("CLIP"))
        {
            // The relative path first, so a session moved with its audio still finds it.
            auto source = file.getParentDirectory().getChildFile (clipXml->getStringAttribute ("file"));

            if (! source.existsAsFile())
                source = juce::File (clipXml->getStringAttribute ("absolute"));

            const auto key = source.getFullPathName();

            if (audio.find (key) == audio.end())
            {
                auto loaded = AudioFileIO::load (source, formats);

                if (loaded.snapshot != nullptr && loaded.error.isEmpty() && ! juce::exactlyEqual (loaded.sampleRate, state.sampleRate))
                    loaded.snapshot = SampleRateConverter::convert (*loaded.snapshot, loaded.sampleRate, state.sampleRate);

                audio[key] = loaded.error.isEmpty() ? loaded.snapshot : nullptr;

                if (audio[key] == nullptr)
                    result.missing.addIfNotAlreadyThere (source.getFullPathName());
            }

            if (audio[key] == nullptr)
                continue;

            SessionClip clip;
            clip.id = state.nextId++;
            clip.name = clipXml->getStringAttribute ("name", source.getFileNameWithoutExtension());
            clip.source = source;
            clip.audio = audio[key];
            clip.position = juce::jmax ((juce::int64) 0, clipXml->getStringAttribute ("position").getLargeIntValue());
            clip.offset = juce::jlimit ((juce::int64) 0, clip.audio->getNumFrames(), clipXml->getStringAttribute ("offset").getLargeIntValue());
            clip.length = juce::jlimit ((juce::int64) 0, clip.audio->getNumFrames() - clip.offset,
                                        clipXml->getStringAttribute ("length").getLargeIntValue());
            clip.gainDb = (float) clipXml->getDoubleAttribute ("gainDb");
            clip.fadeIn = juce::jlimit ((juce::int64) 0, clip.length, clipXml->getStringAttribute ("fadeIn").getLargeIntValue());
            clip.fadeOut = juce::jlimit ((juce::int64) 0, clip.length, clipXml->getStringAttribute ("fadeOut").getLargeIntValue());
            track.clips.push_back (std::move (clip));
        }
    }

    return result;
}
}
