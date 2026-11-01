#include "Session.h"
#include "AudioEdits.h"
#include "AudioFileIO.h"
#include "Metronome.h"
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

namespace
{
    /** prefix + n, counting up from n past names already in use. */
    juce::String freeName (const SessionState& s, const juce::String& prefix, int n)
    {
        const auto taken = [&] (const juce::String& name)
        {
            return std::any_of (s.tracks.begin(), s.tracks.end(), [&] (auto& t) { return t.name == name; });
        };

        while (taken (prefix + juce::String (n)))
            ++n;

        return prefix + juce::String (n);
    }
}

SessionTrack& SessionState::addTrack()
{
    SessionTrack t;
    t.id = nextId++;
    t.name = freeName (*this, "Track ", (int) std::count_if (tracks.begin(), tracks.end(), [] (auto& x) { return ! x.isBus; }) + 1);
    tracks.push_back (std::move (t));
    return tracks.back();
}

SessionTrack& SessionState::addBus()
{
    SessionTrack t;
    t.id = nextId++;
    t.isBus = true;
    t.name = freeName (*this, "Bus ", (int) std::count_if (tracks.begin(), tracks.end(), [] (auto& x) { return x.isBus; }) + 1);
    tracks.push_back (std::move (t));
    return tracks.back();
}

const SessionTrack* SessionState::findTrack (juce::uint32 trackId) const
{
    for (auto& t : tracks)
        if (t.id == trackId)
            return &t;

    return nullptr;
}

const SessionSend* SessionTrack::findSend (juce::uint32 busId) const
{
    for (auto& send : sends)
        if (send.busId == busId)
            return &send;

    return nullptr;
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
namespace
{
    /** The value on the line from a to b at frame (a.frame <= frame < b.frame). */
    float between (const AutomationPoint& a, const AutomationPoint& b, juce::int64 frame)
    {
        return a.value + (b.value - a.value) * (float) ((double) (frame - a.frame) / (double) (b.frame - a.frame));
    }

    /** The value at frame, given the index of the first point after it. */
    float valueWith (const std::vector<AutomationPoint>& points, size_t next, juce::int64 frame)
    {
        if (next == 0)
            return points.front().value;

        if (next == points.size())
            return points.back().value;

        return between (points[next - 1], points[next], frame);
    }

    size_t firstAfter (const std::vector<AutomationPoint>& points, juce::int64 frame)
    {
        return (size_t) (std::upper_bound (points.begin(), points.end(), frame,
                                           [] (juce::int64 f, const AutomationPoint& p) { return f < p.frame; })
                         - points.begin());
    }
}

float Envelope::valueAt (juce::int64 frame) const
{
    return points.empty() ? 0.0f : valueWith (points, firstAfter (points, frame), frame);
}

void Envelope::fill (juce::int64 start, float* out, int n) const
{
    if (points.empty())
    {
        std::fill (out, out + n, 0.0f);
        return;
    }

    auto next = firstAfter (points, start);

    for (int i = 0; i < n; ++i)
    {
        const auto frame = start + i;

        while (next < points.size() && points[next].frame <= frame)
            ++next;

        out[i] = valueWith (points, next, frame);
    }
}

int Envelope::add (juce::int64 frame, float value)
{
    frame = juce::jmax ((juce::int64) 0, frame);
    const auto at = firstAfter (points, frame);
    points.insert (points.begin() + (std::ptrdiff_t) at, { frame, value });
    return (int) at;
}

int Envelope::move (int index, juce::int64 frame, float value)
{
    if (! juce::isPositiveAndBelow (index, (int) points.size()))
        return index;

    const auto i = (size_t) index;
    const auto lowest = i > 0 ? points[i - 1].frame : (juce::int64) 0;
    const auto highest = i + 1 < points.size() ? points[i + 1].frame : std::numeric_limits<juce::int64>::max();
    points[i] = { juce::jlimit (lowest, highest, frame), value };
    return index;
}

void Envelope::remove (int index)
{
    if (juce::isPositiveAndBelow (index, (int) points.size()))
        points.erase (points.begin() + index);
}

juce::Range<float> automationRange (AutomationParameter p)
{
    switch (p)
    {
        case AutomationParameter::volume: return { -60.0f, 12.0f };
        case AutomationParameter::pan:    return { -1.0f, 1.0f };
        case AutomationParameter::effect: return { 0.0f, 1.0f };
    }

    return {};
}

const Envelope& SessionTrack::envelope (const AutomationTarget& t) const
{
    static const Envelope none;

    switch (t.parameter)
    {
        case AutomationParameter::volume: return volumeEnvelope;
        case AutomationParameter::pan:    return panEnvelope;
        case AutomationParameter::effect:
            for (auto& e : effectEnvelopes)
                if (e.slotId == t.slotId && e.parameterId == t.parameterId)
                    return e.envelope;

            return none;
    }

    return none;
}

float SessionTrack::valueAt (const AutomationTarget& t, juce::int64 frame) const
{
    const auto& e = envelope (t);

    if (! e.isEmpty())
        return e.valueAt (frame);

    return t.parameter == AutomationParameter::volume ? gainDb : t.parameter == AutomationParameter::pan ? pan : 0.0f;
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
    if (track < 0 || track >= (int) s.tracks.size() || s.tracks[(size_t) track].isBus)
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

    if (toTrack < (int) s.tracks.size() && s.tracks[(size_t) toTrack].isBus)
        toTrack = where->first;

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

bool chooseTake (SessionState& s, juce::uint32 clipId, int index)
{
    auto* c = find (s, clipId);

    if (c == nullptr || ! juce::isPositiveAndBelow (index, (int) c->takes.size()) || index == c->activeTake)
        return false;

    const auto& from = c->takes[(size_t) c->activeTake];
    const auto& to = c->takes[(size_t) index];
    const auto offset = c->offset + to.shift - from.shift;
    const auto available = to.audio != nullptr ? to.audio->getNumFrames() : 0;

    // Keep the clip inside the take's audio.
    const auto start = c->position + juce::jmax ((juce::int64) 0, -offset);
    const auto end = juce::jmin (c->getEnd(), c->position - offset + available);

    if (end <= start)
        return false;

    c->source = to.source;
    c->audio = to.audio;
    c->offset = offset + (start - c->position);
    c->length = end - start;
    c->position = start;
    c->activeTake = index;
    limitFades (*c);
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

bool crossfade (SessionState& s, juce::uint32 clipId)
{
    const auto where = s.findClip (clipId);

    if (! where.has_value())
        return false;

    auto& clips = s.tracks[(size_t) where->first].clips;
    auto& clip = clips[(size_t) where->second];
    auto changed = false;

    for (auto& other : clips)
    {
        if (other.id == clip.id)
            continue;

        if (other.position < clip.position && other.getEnd() > clip.position && other.getEnd() < clip.getEnd())
        {
            clip.fadeIn = other.fadeOut = other.getEnd() - clip.position;
            limitFades (other);
            changed = true;
        }
        else if (other.position > clip.position && other.position < clip.getEnd() && other.getEnd() > clip.getEnd())
        {
            clip.fadeOut = other.fadeIn = clip.getEnd() - other.position;
            limitFades (other);
            changed = true;
        }
    }

    limitFades (clip);
    return changed;
}

bool setClipGain (SessionState& s, juce::uint32 clipId, float gainDb)
{
    auto* c = find (s, clipId);

    if (c == nullptr)
        return false;

    c->gainDb = juce::jlimit (SessionClip::minGainDb, SessionClip::maxGainDb, gainDb);
    return true;
}

bool normalizeClip (SessionState& s, juce::uint32 clipId, float peakDb)
{
    auto* c = find (s, clipId);

    if (c == nullptr || c->audio == nullptr)
        return false;

    auto peak = 0.0f;

    for (int ch = 0; ch < c->audio->getNumChannels(); ++ch)
    {
        const auto range = c->audio->getPeak (ch, c->offset, c->offset + c->length);
        peak = juce::jmax (peak, std::abs (range.getStart()), std::abs (range.getEnd()));
    }

    if (peak <= 0.0f)
        return false;

    return setClipGain (s, clipId, peakDb - juce::Decibels::gainToDecibels (peak, -1000.0f));
}

bool setClipFades (SessionState& s, juce::uint32 clipId, juce::int64 fadeIn, juce::int64 fadeOut)
{
    auto* c = find (s, clipId);

    if (c == nullptr)
        return false;

    c->fadeIn = fadeIn;
    c->fadeOut = fadeOut;
    limitFades (*c);
    return true;
}

juce::uint32 placeClip (SessionState& s, int track, const SessionClip& clip, juce::int64 position)
{
    if (track < 0 || track >= (int) s.tracks.size() || s.tracks[(size_t) track].isBus)
    {
        s.addTrack();
        track = (int) s.tracks.size() - 1;
    }

    auto copy = clip;
    copy.id = s.nextId++;
    copy.position = juce::jmax ((juce::int64) 0, position);
    clearRange (s, track, copy.position, copy.getEnd());
    s.tracks[(size_t) track].clips.push_back (std::move (copy));
    return s.tracks[(size_t) track].clips.back().id;
}

int repeatClip (SessionState& s, juce::uint32 clipId, juce::int64 until)
{
    const auto where = s.findClip (clipId);

    if (! where.has_value())
        return 0;

    const auto clip = s.tracks[(size_t) where->first].clips[(size_t) where->second];
    auto copies = 0;

    for (auto at = clip.getEnd(); at < until && clip.length > 0; at += clip.length)
    {
        const auto id = placeClip (s, where->first, clip, at);

        if (at + clip.length > until)
            trimClip (s, id, at, until);

        ++copies;
    }

    return copies;
}

AudioDocument::Snapshot reversedAudio (const SessionClip& c)
{
    if (c.audio == nullptr || c.length <= 0)
        return nullptr;

    const AudioSnapshot part (c.audio->getNumChannels(), c.audio->slice (c.offset, c.offset + c.length));
    return AudioEdits::reverse (part, 0, part.getNumFrames());
}

bool reverseClip (SessionState& s, juce::uint32 clipId, const juce::File& file, AudioDocument::Snapshot reversed)
{
    auto* c = find (s, clipId);

    if (c == nullptr || reversed == nullptr)
        return false;

    c->source = file;
    c->length = reversed->getNumFrames();
    c->audio = std::move (reversed);
    c->offset = 0;
    std::swap (c->fadeIn, c->fadeOut);
    c->takes.clear();
    c->activeTake = 0;
    limitFades (*c);
    return true;
}

bool clearRange (SessionState& s, int track, juce::int64 start, juce::int64 end)
{
    if (! juce::isPositiveAndBelow (track, (int) s.tracks.size()) || end <= start)
        return false;

    auto& clips = s.tracks[(size_t) track].clips;
    std::vector<SessionClip> kept;
    auto changed = false;

    for (auto& c : clips)
    {
        if (end <= c.position || start >= c.getEnd())
        {
            kept.push_back (c);
            continue;
        }

        changed = true;

        if (start > c.position)
        {
            auto left = c;
            left.length = start - c.position;
            left.fadeOut = 0;
            limitFades (left);
            kept.push_back (std::move (left));
        }

        if (end < c.getEnd())
        {
            auto right = c;
            right.id = start > c.position ? s.nextId++ : c.id;
            right.offset += end - c.position;
            right.length = c.getEnd() - end;
            right.position = end;
            right.fadeIn = 0;
            limitFades (right);
            kept.push_back (std::move (right));
        }
    }

    clips = std::move (kept);
    return changed;
}

juce::uint32 recordClip (SessionState& s, int track, const juce::File& source, AudioDocument::Snapshot audio, juce::int64 position)
{
    if (audio != nullptr && juce::isPositiveAndBelow (track, (int) s.tracks.size()) && ! s.tracks[(size_t) track].isBus)
        clearRange (s, track, position, position + audio->getNumFrames());

    return addClip (s, track, source, std::move (audio), position);
}

juce::uint32 recordTakes (SessionState& s, int track, const juce::File& source, AudioDocument::Snapshot audio,
                          juce::int64 position, juce::int64 length, const std::vector<juce::int64>& offsets)
{
    if (audio == nullptr || offsets.empty() || length <= 0)
        return 0;

    if (juce::isPositiveAndBelow (track, (int) s.tracks.size()) && ! s.tracks[(size_t) track].isBus)
        clearRange (s, track, position, position + length);

    const auto id = addClip (s, track, source, audio, position);
    auto* c = find (s, id);

    for (auto o : offsets)
        c->takes.push_back ({ source, audio, o - offsets.front() });

    c->activeTake = (int) offsets.size() - 1;
    c->offset = offsets.back();
    c->length = juce::jlimit ((juce::int64) 0, audio->getNumFrames() - c->offset, length);
    return id;
}

void followGroup (SessionState& s, const SessionTrack& before)
{
    const auto* changed = s.findTrack (before.id);

    if (changed == nullptr || changed->group == 0)
        return;

    const auto delta = changed->gainDb - before.gainDb;
    const auto mute = changed->mute, solo = changed->solo;
    const auto group = changed->group;
    const auto muteChanged = mute != before.mute, soloChanged = solo != before.solo;

    for (auto& t : s.tracks)
    {
        if (t.id == before.id || t.group != group)
            continue;

        if (! juce::exactlyEqual (delta, 0.0f) && t.volumeEnvelope.isEmpty())
            t.gainDb = juce::jlimit (-60.0f, 12.0f, t.gainDb + delta);

        if (muteChanged)
            t.mute = mute;

        if (soloChanged)
            t.solo = solo;
    }
}

bool moveTrack (SessionState& s, int from, int to)
{
    const auto count = (int) s.tracks.size();

    if (! juce::isPositiveAndBelow (from, count) || ! juce::isPositiveAndBelow (to, count) || from == to)
        return false;

    const auto first = s.tracks.begin();

    if (from < to)
        std::rotate (first + from, first + from + 1, first + to + 1);
    else
        std::rotate (first + to, first + from, first + from + 1);

    return true;
}

bool removeTrack (SessionState& s, int track)
{
    if (! juce::isPositiveAndBelow (track, (int) s.tracks.size()))
        return false;

    const auto removed = s.tracks[(size_t) track].id;
    s.tracks.erase (s.tracks.begin() + track);

    for (auto& t : s.tracks)
        t.sends.erase (std::remove_if (t.sends.begin(), t.sends.end(), [removed] (auto& send) { return send.busId == removed; }),
                       t.sends.end());

    return true;
}

bool setSend (SessionState& s, juce::uint32 trackId, const SessionSend& send)
{
    const auto* bus = s.findTrack (send.busId);
    auto track = std::find_if (s.tracks.begin(), s.tracks.end(), [trackId] (auto& t) { return t.id == trackId; });

    if (bus == nullptr || ! bus->isBus || track == s.tracks.end() || track->isBus)
        return false;

    for (auto& existing : track->sends)
    {
        if (existing.busId == send.busId)
        {
            existing = send;
            return true;
        }
    }

    track->sends.push_back (send);
    return true;
}

bool removeSend (SessionState& s, juce::uint32 trackId, juce::uint32 busId)
{
    for (auto& t : s.tracks)
    {
        if (t.id != trackId)
            continue;

        const auto before = t.sends.size();
        t.sends.erase (std::remove_if (t.sends.begin(), t.sends.end(), [busId] (auto& send) { return send.busId == busId; }),
                       t.sends.end());
        return t.sends.size() != before;
    }

    return false;
}

bool setEnvelope (SessionState& s, juce::uint32 trackId, const AutomationTarget& target, Envelope e)
{
    const auto range = automationRange (target.parameter);

    for (auto& point : e.points)
    {
        point.frame = juce::jmax ((juce::int64) 0, point.frame);
        point.value = range.clipValue (point.value);
    }

    std::stable_sort (e.points.begin(), e.points.end(), [] (auto& a, auto& b) { return a.frame < b.frame; });

    for (auto& t : s.tracks)
    {
        if (t.id == trackId)
        {
            if (target.parameter == AutomationParameter::volume)
            {
                t.volumeEnvelope = std::move (e);
            }
            else if (target.parameter == AutomationParameter::pan)
            {
                t.panEnvelope = std::move (e);
            }
            else
            {
                auto& all = t.effectEnvelopes;
                const auto same = [&target] (const EffectEnvelope& x) { return x.slotId == target.slotId && x.parameterId == target.parameterId; };
                all.erase (std::remove_if (all.begin(), all.end(), same), all.end());

                if (! e.isEmpty())
                    all.push_back ({ target.slotId, target.parameterId, std::move (e) });
            }

            return true;
        }
    }

    return false;
}

bool writeAutomation (SessionState& s, juce::uint32 trackId, const AutomationTarget& p,
                      const std::vector<AutomationPoint>& pass, juce::int64 returnFrames)
{
    const auto* found = s.findTrack (trackId);

    if (found == nullptr || pass.empty())
        return false;

    const auto old = *found;
    const auto start = pass.front().frame, back = pass.back().frame + juce::jmax ((juce::int64) 0, returnFrames);

    // An effect parameter with no envelope holds the value the pass started from.
    const auto oldValue = [&] (juce::int64 frame)
    {
        return p.parameter == AutomationParameter::effect && old.envelope (p).isEmpty() ? pass.front().value : old.valueAt (p, frame);
    };

    Envelope e;

    for (auto& point : old.envelope (p).points)
        if (point.frame < start)
            e.points.push_back (point);

    e.points.push_back ({ start, oldValue (start) });
    e.points.insert (e.points.end(), pass.begin(), pass.end());
    e.points.push_back ({ back, oldValue (back) });

    for (auto& point : old.envelope (p).points)
        if (point.frame > back)
            e.points.push_back (point);

    return setEnvelope (s, trackId, p, std::move (e));
}
}

//==============================================================================
namespace SessionSnap
{
namespace
{
    std::optional<juce::int64> nearest (const SessionState& s, juce::int64 frame, juce::int64 tolerance, const Targets& targets)
    {
        std::optional<juce::int64> best;
        auto bestDistance = tolerance;

        const auto consider = [&] (juce::int64 target)
        {
            const auto distance = std::abs (target - frame);

            if (distance <= bestDistance && (! best.has_value() || distance < std::abs (*best - frame)))
            {
                best = target;
                bestDistance = distance;
            }
        };

        consider (0);

        for (auto f : targets.extra)
            consider (f);

        for (auto& t : s.tracks)
        {
            for (auto& c : t.clips)
            {
                if (c.id != targets.ignoreClip
                    && std::find (targets.ignoreClips.begin(), targets.ignoreClips.end(), c.id) == targets.ignoreClips.end())
                {
                    consider (c.position);
                    consider (c.getEnd());
                }
            }
        }

        for (size_t i = 0; i < s.markers.size(); ++i)
        {
            if ((int) i != targets.ignoreMarker)
            {
                consider (s.markers[i].start);
                consider (s.markers[i].getEnd());
            }
        }

        if (targets.gridBeats > 0)
        {
            // The grid beats either side of the frame.
            const auto beat = Metronome::beatAt (frame, s.tempo, s.sampleRate);
            const auto below = (beat >= 0 ? beat : beat - targets.gridBeats + 1) / targets.gridBeats * targets.gridBeats;

            for (auto b : { below, below + targets.gridBeats })
                if (b >= 0)
                    consider (Metronome::beatFrame (b, s.tempo, s.sampleRate));
        }
        else if (targets.gridStep > 0)
        {
            consider (juce::jmax ((juce::int64) 0, (juce::int64) std::llround ((double) frame / (double) targets.gridStep) * targets.gridStep));
        }

        return best;
    }
}

juce::int64 snap (const SessionState& s, juce::int64 frame, juce::int64 tolerance, const Targets& targets)
{
    return nearest (s, frame, tolerance, targets).value_or (frame);
}

juce::int64 snapSpan (const SessionState& s, juce::Range<juce::int64> span, juce::int64 tolerance, const Targets& targets)
{
    const auto start = nearest (s, span.getStart(), tolerance, targets);
    const auto end = nearest (s, span.getEnd(), tolerance, targets);

    if (start.has_value() && (! end.has_value() || std::abs (*start - span.getStart()) <= std::abs (*end - span.getEnd())))
        return *start - span.getStart();

    return end.has_value() ? *end - span.getEnd() : 0;
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
    root.setAttribute ("tempo", state.tempo);
    root.setAttribute ("beatsPerBar", state.beatsPerBar);

    // Sends name their bus by its place in the file, since ids are given out afresh on loading.
    std::map<juce::uint32, int> indexOf;

    for (size_t i = 0; i < state.tracks.size(); ++i)
        indexOf[state.tracks[i].id] = (int) i;

    for (auto& t : state.tracks)
    {
        auto* track = root.createNewChildElement ("TRACK");
        track->setAttribute ("name", t.name);

        if (t.isBus)
            track->setAttribute ("bus", true);

        for (auto& send : t.sends)
        {
            auto* sendXml = track->createNewChildElement ("SEND");
            sendXml->setAttribute ("bus", indexOf[send.busId]);
            sendXml->setAttribute ("gainDb", send.gainDb);
            sendXml->setAttribute ("preFader", send.preFader);
        }
        track->setAttribute ("gainDb", t.gainDb);
        track->setAttribute ("pan", t.pan);
        track->setAttribute ("mute", t.mute);
        track->setAttribute ("solo", t.solo);

        if (t.group != 0)
            track->setAttribute ("group", t.group);

        if (t.colour >= 0)
            track->setAttribute ("colour", t.colour);

        if (! t.input.isDefault())
        {
            track->setAttribute ("inputFirst", t.input.first);
            track->setAttribute ("inputChannels", t.input.channels);
        }

        const auto writeEnvelope = [track] (const Envelope& envelope) -> juce::XmlElement*
        {
            if (envelope.isEmpty())
                return nullptr;

            auto* envelopeXml = track->createNewChildElement ("ENVELOPE");

            for (auto& point : envelope.points)
            {
                auto* pointXml = envelopeXml->createNewChildElement ("POINT");
                pointXml->setAttribute ("frame", juce::String (point.frame));
                pointXml->setAttribute ("value", point.value);
            }

            return envelopeXml;
        };

        if (auto* xml = writeEnvelope (t.volumeEnvelope))
            xml->setAttribute ("parameter", "volume");

        if (auto* xml = writeEnvelope (t.panEnvelope))
            xml->setAttribute ("parameter", "pan");

        for (auto& e : t.effectEnvelopes)
        {
            if (auto* xml = writeEnvelope (e.envelope))
            {
                xml->setAttribute ("parameter", "effect");
                xml->setAttribute ("slot", (int) e.slotId);
                xml->setAttribute ("id", e.parameterId);
            }
        }

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

            if (! c.takes.empty())
            {
                clip->setAttribute ("activeTake", c.activeTake);

                for (auto& take : c.takes)
                {
                    auto* takeXml = clip->createNewChildElement ("TAKE");
                    takeXml->setAttribute ("file", take.source.getRelativePathFrom (target.getParentDirectory()));
                    takeXml->setAttribute ("absolute", take.source.getFullPathName());
                    takeXml->setAttribute ("shift", juce::String (take.shift));
                }
            }
        }
    }

    for (auto& m : state.markers)
    {
        auto* marker = root.createNewChildElement ("MARKER");
        marker->setAttribute ("name", m.name);
        marker->setAttribute ("start", juce::String (m.start));

        if (m.isRegion())
            marker->setAttribute ("length", juce::String (m.length));
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
    state.tempo = juce::jlimit (20.0, 400.0, root->getDoubleAttribute ("tempo", 120.0));
    state.beatsPerBar = juce::jlimit (1, 16, root->getIntAttribute ("beatsPerBar", 4));
    std::map<juce::String, AudioDocument::Snapshot> audio;   // by full path: each file read once

    for (auto* trackXml : root->getChildWithTagNameIterator ("TRACK"))
    {
        auto& track = trackXml->getBoolAttribute ("bus") ? state.addBus() : state.addTrack();
        track.name = trackXml->getStringAttribute ("name", track.name);

        for (auto* sendXml : trackXml->getChildWithTagNameIterator ("SEND"))
        {
            SessionSend send;
            send.busId = (juce::uint32) sendXml->getIntAttribute ("bus", -1);   // an index until every track is read
            send.gainDb = (float) sendXml->getDoubleAttribute ("gainDb");
            send.preFader = sendXml->getBoolAttribute ("preFader");
            track.sends.push_back (send);
        }
        track.gainDb = (float) trackXml->getDoubleAttribute ("gainDb");
        track.pan = (float) trackXml->getDoubleAttribute ("pan");
        track.mute = trackXml->getBoolAttribute ("mute");
        track.solo = trackXml->getBoolAttribute ("solo");
        track.group = juce::jlimit (0, SessionTrack::maxGroups, trackXml->getIntAttribute ("group"));
        track.colour = juce::jlimit (-1, 7, trackXml->getIntAttribute ("colour", -1));

        if (const auto channels = trackXml->getIntAttribute ("inputChannels"); ! track.isBus && (channels == 1 || channels == 2))
            track.input = { juce::jmax (0, trackXml->getIntAttribute ("inputFirst")), channels };

        for (auto* envelopeXml : trackXml->getChildWithTagNameIterator ("ENVELOPE"))
        {
            const auto name = envelopeXml->getStringAttribute ("parameter");
            AutomationTarget target;

            if (name == "volume")
                target = AutomationParameter::volume;
            else if (name == "pan")
                target = AutomationParameter::pan;
            else if (name == "effect")
                target = AutomationTarget::effect ((juce::uint32) envelopeXml->getIntAttribute ("slot"), envelopeXml->getStringAttribute ("id"));
            else
                continue;

            Envelope envelope;

            for (auto* pointXml : envelopeXml->getChildWithTagNameIterator ("POINT"))
                envelope.points.push_back ({ pointXml->getStringAttribute ("frame").getLargeIntValue(),
                                             (float) pointXml->getDoubleAttribute ("value") });

            SessionEdits::setEnvelope (state, track.id, target, std::move (envelope));
        }

        if (auto* effects = trackXml->getChildByName ("EFFECTS"))
            result.effects[track.id] = std::make_shared<juce::XmlElement> (*effects);

        for (auto* clipXml : trackXml->getChildWithTagNameIterator ("CLIP"))
        {
            // The relative path first, so a session moved with its audio still finds it.
            const auto locate = [&] (const juce::XmlElement& xml)
            {
                auto source = file.getParentDirectory().getChildFile (xml.getStringAttribute ("file"));

                if (! source.existsAsFile())
                    source = juce::File (xml.getStringAttribute ("absolute"));

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

                return std::pair { source, audio[key] };
            };

            const auto [source, clipAudio] = locate (*clipXml);

            if (clipAudio == nullptr)
                continue;

            SessionClip clip;
            clip.id = state.nextId++;
            clip.name = clipXml->getStringAttribute ("name", source.getFileNameWithoutExtension());
            clip.source = source;
            clip.audio = clipAudio;
            clip.position = juce::jmax ((juce::int64) 0, clipXml->getStringAttribute ("position").getLargeIntValue());
            clip.offset = juce::jlimit ((juce::int64) 0, clip.audio->getNumFrames(), clipXml->getStringAttribute ("offset").getLargeIntValue());
            clip.length = juce::jlimit ((juce::int64) 0, clip.audio->getNumFrames() - clip.offset,
                                        clipXml->getStringAttribute ("length").getLargeIntValue());
            clip.gainDb = (float) clipXml->getDoubleAttribute ("gainDb");
            clip.fadeIn = juce::jlimit ((juce::int64) 0, clip.length, clipXml->getStringAttribute ("fadeIn").getLargeIntValue());
            clip.fadeOut = juce::jlimit ((juce::int64) 0, clip.length, clipXml->getStringAttribute ("fadeOut").getLargeIntValue());

            // Takes, kept only when every one's audio is there; otherwise the clip plays the one it had.
            std::vector<SessionClip::Take> takes;

            for (auto* takeXml : clipXml->getChildWithTagNameIterator ("TAKE"))
            {
                const auto [takeSource, takeAudio] = locate (*takeXml);
                takes.push_back ({ takeSource, takeAudio, takeXml->getStringAttribute ("shift").getLargeIntValue() });
            }

            const auto active = clipXml->getIntAttribute ("activeTake");

            if (juce::isPositiveAndBelow (active, (int) takes.size())
                && std::all_of (takes.begin(), takes.end(), [] (auto& t) { return t.audio != nullptr; })
                && takes[(size_t) active].audio == clip.audio)
            {
                clip.takes = std::move (takes);
                clip.activeTake = active;
            }

            track.clips.push_back (std::move (clip));
        }
    }

    for (auto* markerXml : root->getChildWithTagNameIterator ("MARKER"))
    {
        Marker m;
        m.name = markerXml->getStringAttribute ("name");
        m.start = juce::jmax ((juce::int64) 0, markerXml->getStringAttribute ("start").getLargeIntValue());
        m.length = juce::jmax ((juce::int64) 0, markerXml->getStringAttribute ("length").getLargeIntValue());
        state.markers.push_back (m);
    }

    MarkerEdits::sort (state.markers);

    // Indices to ids; sends to anything but a bus are dropped.
    const auto pending = state.tracks;

    for (size_t i = 0; i < pending.size(); ++i)
    {
        state.tracks[i].sends.clear();

        for (auto send : pending[i].sends)
        {
            if (send.busId < pending.size())
            {
                send.busId = pending[send.busId].id;
                SessionEdits::setSend (state, state.tracks[i].id, send);
            }
        }
    }

    return result;
}
}
