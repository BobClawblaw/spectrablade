#include "Session.h"
#include "Surround.h"

#include <numeric>
#include <set>

#include "AudioEdits.h"
#include "AudioFileIO.h"
#include "Metronome.h"
#include "SampleRateConverter.h"
#include "TimeStretch.h"
#include "TrackColours.h"

//==============================================================================
juce::int64 SessionState::getLength() const
{
    juce::int64 length = 0;

    for (auto& t : tracks)
    {
        for (auto& c : t.clips)
            length = juce::jmax (length, c.getEnd());

        for (auto& c : t.noteClips)
            length = juce::jmax (length, c.getEnd());
    }

    return length;
}

bool SessionState::anySolo() const
{
    return std::any_of (tracks.begin(), tracks.end(), [] (const SessionTrack& t) { return t.solo; });
}

bool SessionState::isMuted (const SessionTrack& t) const
{
    const auto* folder = t.folderId != 0 ? findTrack (t.folderId) : nullptr;
    return t.mute || (folder != nullptr && folder->mute);
}

bool SessionState::isSoloed (const SessionTrack& t) const
{
    const auto* folder = t.folderId != 0 ? findTrack (t.folderId) : nullptr;
    return t.solo || (folder != nullptr && folder->solo);
}

bool SessionState::isSoloSafe (const SessionTrack& t) const
{
    const auto* folder = t.folderId != 0 ? findTrack (t.folderId) : nullptr;
    return t.soloSafe || (folder != nullptr && folder->soloSafe);
}

float SessionState::folderGain (const SessionTrack& t) const
{
    const auto* folder = t.folderId != 0 ? findTrack (t.folderId) : nullptr;
    return folder != nullptr && ! juce::exactlyEqual (folder->gainDb, 0.0f) ? juce::Decibels::decibelsToGain (folder->gainDb, -150.0f) : 1.0f;
}

const SessionTrack* SessionState::outputBusOf (const SessionTrack& t) const
{
    const auto* bus = t.isFolder || t.outputBusId == 0 || t.outputBusId == t.id ? nullptr : findTrack (t.outputBusId);
    return bus != nullptr && bus->isBus ? bus : nullptr;
}

juce::uint32 SessionTrack::sidechainKey (juce::uint32 slotId) const
{
    for (auto& s : sidechains)
        if (s.slotId == slotId)
            return s.keyTrackId;

    return 0;
}

bool SessionState::listensTo (const SessionTrack& track, juce::uint32 otherId) const
{
    // Depth first through the keys; no longer than the session, so a loop can't hang it.
    std::vector<std::pair<const SessionTrack*, size_t>> stack { { &track, 0 } };

    while (! stack.empty() && stack.size() <= tracks.size() + 1)
    {
        auto [t, depth] = stack.back();
        stack.pop_back();
        std::vector<juce::uint32> keys;

        if (t->ducking.isOn())
            keys.push_back (t->ducking.keyTrackId);

        for (auto& s : t->sidechains)
            keys.push_back (s.keyTrackId);

        for (const auto k : keys)
        {
            if (k == otherId)
                return true;

            if (const auto* key = findTrack (k); key != nullptr && depth < tracks.size())
                stack.push_back ({ key, depth + 1 });
        }
    }

    return false;
}

bool SessionState::feeds (const SessionTrack& from, juce::uint32 busId) const
{
    // Down a chain of outputs from a bus: no longer than the session, so a loop can't hang it.
    const auto reaches = [this, busId] (const SessionTrack* bus)
    {
        for (size_t steps = 0; bus != nullptr && steps <= tracks.size(); ++steps, bus = outputBusOf (*bus))
            if (bus->id == busId)
                return true;

        return false;
    };

    if (reaches (outputBusOf (from)))
        return true;

    if (from.isBus)
        return false;

    return std::any_of (from.sends.begin(), from.sends.end(), [&] (auto& send)
    {
        const auto* bus = findTrack (send.busId);
        return bus != nullptr && bus->isBus && reaches (bus);
    });
}

bool SessionState::isShown (const SessionTrack& t) const
{
    const auto* folder = t.folderId != 0 ? findTrack (t.folderId) : nullptr;
    return ! t.hidden && (folder == nullptr || ! folder->collapsed);
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
    t.name = freeName (*this, "Track ", (int) std::count_if (tracks.begin(), tracks.end(), [] (auto& x) { return ! x.isBus && ! x.isFolder; }) + 1);
    tracks.push_back (std::move (t));
    return tracks.back();
}

SessionTrack& SessionState::addInstrumentTrack()
{
    auto& t = addTrack();
    t.isInstrument = true;
    t.musical = true;
    return t;
}

SessionTrack& SessionState::addFolder()
{
    SessionTrack t;
    t.id = nextId++;
    t.isFolder = true;
    t.name = freeName (*this, "Folder ", (int) std::count_if (tracks.begin(), tracks.end(), [] (auto& x) { return x.isFolder; }) + 1);
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

juce::int64 SessionTrack::clipsEnd() const noexcept
{
    juce::int64 end = 0;

    for (auto& c : clips)
        end = juce::jmax (end, c.getEnd());

    for (auto& c : noteClips)
        end = juce::jmax (end, c.getEnd());

    return end;
}

std::optional<std::pair<int, int>> SessionState::findNoteClip (juce::uint32 clipId) const
{
    for (size_t t = 0; t < tracks.size(); ++t)
        for (size_t c = 0; c < tracks[t].noteClips.size(); ++c)
            if (tracks[t].noteClips[c].id == clipId)
                return std::pair<int, int> { (int) t, (int) c };

    return std::nullopt;
}

//==============================================================================
juce::String ControllerLane::nameOf (int controller)
{
    switch (controller)
    {
        case pitchBend: return "Pitch bend";
        case 1:  return "Mod wheel (CC 1)";
        case 2:  return "Breath (CC 2)";
        case 7:  return "Volume (CC 7)";
        case 10: return "Pan (CC 10)";
        case 11: return "Expression (CC 11)";
        case 64: return "Sustain (CC 64)";
        case 71: return "Resonance (CC 71)";
        case 74: return "Cutoff (CC 74)";
        default: return "CC " + juce::String (controller);
    }
}

const ControllerLane* NoteClip::findLane (int controller) const noexcept
{
    for (auto& l : lanes)
        if (l.controller == controller)
            return &l;

    return nullptr;
}

std::optional<float> NoteClip::laneValueAt (const ControllerLane& lane, juce::int64 frame) const noexcept
{
    if (lane.envelope.isEmpty() || frame < position || frame >= getEnd())
        return std::nullopt;

    const auto period = juce::jmax ((juce::int64) 1, getPeriod());
    return juce::jlimit (0.0f, 1.0f, lane.envelope.valueAt ((frame - getOrigin()) % period));
}

juce::MidiMessage NoteClip::laneMessage (int controller, float value)
{
    const auto v = juce::jlimit (0.0f, 1.0f, value);

    if (controller == ControllerLane::pitchBend)
        return juce::MidiMessage::pitchWheel (1, juce::jlimit (0, 16383, (int) std::lround (v * 16383.0f)));

    return juce::MidiMessage::controllerEvent (1, juce::jlimit (0, 127, controller), juce::jlimit (0, 127, (int) std::lround (v * 127.0f)));
}

void NoteClip::addMidi (juce::int64 from, int count, juce::MidiBuffer& midi) const
{
    const auto to = from + count, end = getEnd(), origin = getOrigin();

    // A note cut by the clip's end turns off at the end frame itself, so a block starting there still counts.
    if (count <= 0 || to <= position || from > end)
        return;

    // Each pass of the notes (one, or every loopLength frames from the origin): its note-offs
    // first, then its note-ons, so at one frame a note taking over from another is turned off
    // before it is on. A note starting before the clip isn't played.
    const auto period = juce::jmax ((juce::int64) 1, getPeriod());
    const auto firstPass = juce::jmax ((juce::int64) 0, (from - origin) / period - 1);

    // The lanes first, so a note starting at a frame hears the controller's value there: at the
    // clip's start, at each pass's start, and at each step where the rounded value changed.
    for (auto& lane : lanes)
    {
        if (lane.envelope.isEmpty())
            continue;

        const auto steps = (juce::int64) automationStepFrames;
        const auto rounded = [&] (juce::int64 frame) -> int
        {
            const auto v = laneValueAt (lane, frame);
            return v.has_value() ? (int) std::lround (*v * (lane.controller == ControllerLane::pitchBend ? 16383.0f : 127.0f)) : -1;
        };

        // The latest of the clip's start and the pass starts at or before a frame.
        const auto anchorBefore = [&] (juce::int64 frame) { return juce::jmax (position, origin + ((frame - origin) / period) * period); };
        const auto send = [&] (juce::int64 frame) { midi.addEvent (laneMessage (lane.controller, *laneValueAt (lane, frame)), (int) (frame - from)); };

        if (position >= from && position < to)
            send (position);

        for (auto pass = origin + (firstPass + 1) * period; pass < juce::jmin (to, end); pass += period)
            if (pass > position && pass >= from)
                send (pass);

        for (auto f = ((juce::jmax (from, position + 1) + steps - 1) / steps) * steps; f < juce::jmin (to, end); f += steps)
        {
            const auto anchor = anchorBefore (f);

            if (f == anchor)
                continue;   // sent above

            if (rounded (f) != rounded (juce::jmax (f - steps, anchor)))
                send (f);
        }
    }

    for (auto pass = firstPass; origin + pass * period < juce::jmin (to, end); ++pass)
    {
        const auto passStart = origin + pass * period;

        for (auto& n : notes)
        {
            const auto on = passStart + n.start;
            const auto off = juce::jmin (on + juce::jmax ((juce::int64) 1, n.length), end);

            if (n.start >= period || on < position || on >= end || off <= on)
                continue;

            if (off >= from && off < to)
                midi.addEvent (juce::MidiMessage::noteOff (1, juce::jlimit (0, 127, n.pitch)), (int) (off - from));
        }

        for (auto& n : notes)
        {
            const auto on = passStart + n.start;

            if (n.start < period && on >= position && on < end && on >= from && on < to)
                midi.addEvent (juce::MidiMessage::noteOn (1, juce::jlimit (0, 127, n.pitch), juce::jlimit (0.0f, 1.0f, n.velocity)), (int) (on - from));
        }
    }
}

juce::Range<juce::int64> NoteClip::notesRange() const noexcept
{
    juce::Range<juce::int64> r;
    auto first = true;

    for (auto& n : notes)
    {
        const auto whole = offset + length;
        const juce::Range<juce::int64> span (juce::jlimit ((juce::int64) 0, whole, n.start), juce::jlimit ((juce::int64) 0, whole, n.start + n.length));
        r = first ? span : r.getUnionWith (span);
        first = false;
    }

    return r;
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

namespace FadeShapes
{
float level (FadeShape shape, juce::int64 frame, juce::int64 length)
{
    if (frame >= length)
        return 1.0f;

    const auto t = (float) frame / (float) length;

    switch (shape)
    {
        case FadeShape::linear:     return t;
        case FadeShape::equalPower: return std::sin (juce::MathConstants<float>::halfPi * t);
        case FadeShape::slow:       return t * t;
        case FadeShape::fast:       return 1.0f - (1.0f - t) * (1.0f - t);
        case FadeShape::sCurve:
        default:                    return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) frame / (float) length);
    }
}

juce::String displayName (FadeShape shape)
{
    switch (shape)
    {
        case FadeShape::linear:     return "Linear";
        case FadeShape::equalPower: return "Equal Power";
        case FadeShape::slow:       return "Slow Start";
        case FadeShape::fast:       return "Fast Start";
        case FadeShape::sCurve:
        default:                    return "S-Curve";
    }
}

juce::String id (FadeShape shape)
{
    switch (shape)
    {
        case FadeShape::linear:     return "linear";
        case FadeShape::equalPower: return "equalPower";
        case FadeShape::slow:       return "slow";
        case FadeShape::fast:       return "fast";
        case FadeShape::sCurve:
        default:                    return "sCurve";
    }
}

FadeShape fromId (const juce::String& name)
{
    for (auto shape : all)
        if (id (shape) == name)
            return shape;

    return FadeShape::sCurve;
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
        case AutomationParameter::surroundX:
        case AutomationParameter::surroundY:     return { -1.0f, 1.0f };
        case AutomationParameter::surroundWidth: return { 0.0f, 1.0f };
        case AutomationParameter::lfe:           return { -60.0f, SurroundPan::maxLfeDb };
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
        case AutomationParameter::surroundX:
        case AutomationParameter::surroundY:
        case AutomationParameter::surroundWidth:
        case AutomationParameter::lfe:
            return surroundEnvelopes[(size_t) ((int) t.parameter - (int) AutomationParameter::surroundX)];
    }

    return none;
}

SurroundPan SessionTrack::surroundPanAt (juce::int64 frame) const
{
    auto p = surroundPan;
    const auto at = [&] (int i, float setting) { return surroundEnvelopes[(size_t) i].isEmpty() ? setting : surroundEnvelopes[(size_t) i].valueAt (frame); };
    p.x = at (0, p.x);
    p.y = at (1, p.y);
    p.width = at (2, p.width);

    if (! surroundEnvelopes[3].isEmpty())
    {
        const auto db = surroundEnvelopes[3].valueAt (frame);
        p.lfeDb = db <= -60.0f ? SurroundPan::lfeOff : db;
    }

    return p;
}

float SessionTrack::valueAt (const AutomationTarget& t, juce::int64 frame) const
{
    const auto& e = envelope (t);

    if (! e.isEmpty())
        return e.valueAt (frame);

    switch (t.parameter)
    {
        case AutomationParameter::volume:        return gainDb;
        case AutomationParameter::pan:           return pan;
        case AutomationParameter::surroundX:     return surroundPan.x;
        case AutomationParameter::surroundY:     return surroundPan.y;
        case AutomationParameter::surroundWidth: return surroundPan.width;
        case AutomationParameter::lfe:           return juce::jmax (-60.0f, surroundPan.lfeDb);
        case AutomationParameter::effect:        break;
    }

    return 0.0f;
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

    // A clip the edits may change: there, not locked, and not on a frozen track.
    SessionClip* findUnlocked (SessionState& s, juce::uint32 clipId)
    {
        const auto where = s.findClip (clipId);

        if (! where.has_value() || s.tracks[(size_t) where->first].isFrozen())
            return nullptr;

        auto* c = &s.tracks[(size_t) where->first].clips[(size_t) where->second];
        return ! c->locked ? c : nullptr;
    }

    void limitFades (SessionClip& c)
    {
        c.fadeIn = juce::jlimit ((juce::int64) 0, c.length, c.fadeIn);
        c.fadeOut = juce::jlimit ((juce::int64) 0, c.length - c.fadeIn, c.fadeOut);
    }
}

juce::uint32 addClip (SessionState& s, int track, const juce::File& source, AudioDocument::Snapshot audio, juce::int64 position)
{
    if (track < 0 || track >= (int) s.tracks.size() || ! s.tracks[(size_t) track].takesAudioClips())
    {
        // Six- or eight-channel audio makes a 5.1 or 7.1 track, in a surround master.
        auto& added = s.addTrack();
        added.fiveOne = s.surround && audio != nullptr && audio->getNumChannels() == Surround::fiveOneChannels;
        added.sevenOne = s.surround && audio != nullptr && audio->getNumChannels() == Surround::sevenOneChannels;
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

bool setTrackKind (SessionState& s, juce::uint32 trackId, bool instrument)
{
    const auto t = std::find_if (s.tracks.begin(), s.tracks.end(), [trackId] (auto& x) { return x.id == trackId; });

    if (t == s.tracks.end() || ! t->takesClips() || t->isWide() || t->isInstrument == instrument
        || (instrument ? ! t->clips.empty() : ! t->noteClips.empty()))
        return false;

    t->isInstrument = instrument;

    if (instrument)
        t->musical = true;

    return true;
}

bool setTrackMusical (SessionState& s, juce::uint32 trackId, bool musical)
{
    const auto t = std::find_if (s.tracks.begin(), s.tracks.end(), [trackId] (auto& x) { return x.id == trackId; });

    if (t == s.tracks.end() || t->isBus || t->isFolder || t->musical == musical)
        return false;

    t->musical = musical;
    return true;
}

int separateClipKinds (SessionState& s)
{
    int added = 0;

    for (size_t i = 0; i < s.tracks.size(); ++i)
    {
        auto& t = s.tracks[i];
        const auto wrongAudio = t.isInstrument && ! t.clips.empty();
        const auto wrongNotes = ! t.isInstrument && ! t.isBus && ! t.isFolder && ! t.noteClips.empty();

        if (! wrongAudio && ! wrongNotes)
            continue;

        SessionTrack moved;
        moved.id = s.nextId++;
        moved.name = t.name;
        moved.isInstrument = wrongNotes;
        moved.folderId = t.folderId;
        moved.colour = t.colour;

        if (wrongAudio)
            std::swap (moved.clips, t.clips);

        if (wrongNotes)
            std::swap (moved.noteClips, t.noteClips);

        s.tracks.insert (s.tracks.begin() + (long) i + 1, std::move (moved));
        ++added;
        ++i;
    }

    return added;
}

bool moveClip (SessionState& s, juce::uint32 clipId, int toTrack, juce::int64 position)
{
    const auto where = s.findClip (clipId);

    if (! where.has_value() || toTrack < 0 || findUnlocked (s, clipId) == nullptr)
        return false;

    auto& from = s.tracks[(size_t) where->first].clips;
    auto clip = from[(size_t) where->second];
    clip.position = juce::jmax ((juce::int64) 0, position);

    if (toTrack < (int) s.tracks.size() && ! s.tracks[(size_t) toTrack].takesAudioClips())
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
    auto* c = findUnlocked (s, clipId);

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

    if (! where.has_value() || findUnlocked (s, clipId) == nullptr)
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
    auto* c = findUnlocked (s, clipId);

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

    // The same moment is to.shift - from.shift frames further into the new take's audio.
    for (auto& p : c->gainEnvelope.points)
        p.frame += to.shift - from.shift;

    limitFades (*c);
    return true;
}

bool removeClip (SessionState& s, juce::uint32 clipId)
{
    const auto where = s.findClip (clipId);

    if (! where.has_value() || findUnlocked (s, clipId) == nullptr)
        return false;

    auto& clips = s.tracks[(size_t) where->first].clips;
    clips.erase (clips.begin() + where->second);
    return true;
}

bool setClipColour (SessionState& s, juce::uint32 clipId, int colour)
{
    auto* c = find (s, clipId);

    if (! juce::isPositiveAndBelow (colour, (int) TrackColours::palette.size()))
        colour = -1;

    if (c == nullptr || c->colour == colour)
        return false;

    c->colour = colour;
    return true;
}

bool setClipGainEnvelope (SessionState& s, juce::uint32 clipId, Envelope envelope)
{
    auto* c = findUnlocked (s, clipId);

    if (c == nullptr)
        return false;

    for (auto& p : envelope.points)
        p.value = juce::jlimit (SessionClip::minGainDb, SessionClip::maxGainDb, p.value);

    c->gainEnvelope = std::move (envelope);
    return true;
}

juce::uint32 groupClips (SessionState& s, const std::vector<juce::uint32>& clipIds)
{
    std::vector<SessionClip*> clips;

    for (auto id : clipIds)
        if (auto* c = find (s, id); c != nullptr && std::find (clips.begin(), clips.end(), c) == clips.end())
            clips.push_back (c);

    if (clips.size() < 2)
        return 0;

    const auto groupId = s.nextId++;

    for (auto* c : clips)
        c->groupId = groupId;

    return groupId;
}

bool ungroupClips (SessionState& s, const std::vector<juce::uint32>& clipIds)
{
    std::set<juce::uint32> groups;

    for (auto id : clipIds)
        if (const auto* c = find (s, id); c != nullptr && c->groupId != 0)
            groups.insert (c->groupId);

    for (auto& t : s.tracks)
        for (auto& c : t.clips)
            if (groups.count (c.groupId) > 0)
                c.groupId = 0;

    return ! groups.empty();
}

std::vector<juce::uint32> clipGroup (const SessionState& s, juce::uint32 clipId)
{
    const auto where = s.findClip (clipId);

    if (! where.has_value())
        return {};

    const auto groupId = s.tracks[(size_t) where->first].clips[(size_t) where->second].groupId;

    if (groupId == 0)
        return { clipId };

    std::vector<juce::uint32> ids;

    for (auto& t : s.tracks)
        for (auto& c : t.clips)
            if (c.groupId == groupId)
                ids.push_back (c.id);

    return ids;
}

bool setClipMuted (SessionState& s, juce::uint32 clipId, bool muted)
{
    auto* c = find (s, clipId);
    const auto where = s.findClip (clipId);

    // A frozen track plays its frozen audio, so muting its clips would change nothing heard.
    if (c == nullptr || c->muted == muted || s.tracks[(size_t) where->first].isFrozen())
        return false;

    c->muted = muted;
    return true;
}

juce::int64 zeroCrossingNear (const SessionClip& c, juce::int64 frame, juce::int64 within)
{
    if (c.audio == nullptr || within <= 0)
        return frame;

    // Session frames [first, last] whose audio frame and the one before it exist.
    const auto audioStart = c.position - c.offset;
    const auto first = juce::jmax (frame - within, audioStart + 1);
    const auto last = juce::jmin (frame + within, audioStart + c.audio->getNumFrames() - 1);

    if (last < first)
        return frame;

    const auto count = (int) (last - first + 2);
    juce::AudioBuffer<float> b (c.audio->getNumChannels(), count);
    c.audio->read (b, 0, first - 1 - audioStart, count);
    std::vector<float> sum ((size_t) count, 0.0f);

    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        for (int i = 0; i < count; ++i)
            sum[(size_t) i] += b.getSample (ch, i);

    std::optional<juce::int64> best;

    for (int i = 1; i < count; ++i)
    {
        const auto before = sum[(size_t) i - 1], at = sum[(size_t) i];

        if (juce::exactlyEqual (at, 0.0f) || (before < 0.0f) != (at < 0.0f))
        {
            const auto candidate = first + i - 1;

            if (! best.has_value() || std::abs (candidate - frame) < std::abs (*best - frame))
                best = candidate;
        }
    }

    return best.value_or (frame);
}

bool setClipLocked (SessionState& s, juce::uint32 clipId, bool locked)
{
    auto* c = find (s, clipId);

    if (c == nullptr || c->locked == locked)
        return false;

    c->locked = locked;
    return true;
}

bool crossfade (SessionState& s, juce::uint32 clipId)
{
    const auto where = s.findClip (clipId);

    if (! where.has_value() || findUnlocked (s, clipId) == nullptr)
        return false;

    auto& clips = s.tracks[(size_t) where->first].clips;
    auto& clip = clips[(size_t) where->second];
    auto changed = false;

    for (auto& other : clips)
    {
        if (other.id == clip.id || other.locked)
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
    auto* c = findUnlocked (s, clipId);

    if (c == nullptr)
        return false;

    c->gainDb = juce::jlimit (SessionClip::minGainDb, SessionClip::maxGainDb, gainDb);
    return true;
}

bool normalizeClip (SessionState& s, juce::uint32 clipId, float peakDb)
{
    auto* c = findUnlocked (s, clipId);

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
    auto* c = findUnlocked (s, clipId);

    if (c == nullptr)
        return false;

    c->fadeIn = fadeIn;
    c->fadeOut = fadeOut;
    limitFades (*c);
    return true;
}

juce::uint32 placeClip (SessionState& s, int track, const SessionClip& clip, juce::int64 position)
{
    if (track < 0 || track >= (int) s.tracks.size() || ! s.tracks[(size_t) track].takesAudioClips())
    {
        s.addTrack();
        track = (int) s.tracks.size() - 1;
    }

    auto copy = clip;
    copy.id = s.nextId++;
    copy.locked = false;
    copy.groupId = 0;
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

}

namespace
{
    /** An envelope's line across [start, end), from 0: its value at start, its points inside,
        and its value at the last frame. Empty for an empty envelope. */
    Envelope lineAcross (const Envelope& e, juce::int64 start, juce::int64 end)
    {
        if (e.isEmpty())
            return {};

        Envelope out;
        out.points.push_back ({ 0, e.valueAt (start) });

        for (auto& p : e.points)
            if (p.frame > start && p.frame < end - 1)
                out.points.push_back ({ p.frame - start, p.value });

        if (end - 1 > start)
            out.points.push_back ({ end - 1 - start, e.valueAt (end - 1) });

        return out;
    }
}

namespace SessionEdits
{
namespace
{
    /** Whether processed takes fit a clip: none (its takes go), or one for each, every other
        take as long as the processed audio. */
    bool takesMatch (const SessionClip& c, const std::vector<SessionClip::Take>& takes, juce::int64 frames)
    {
        if (takes.empty())
            return true;

        if (takes.size() != c.takes.size())
            return false;

        for (size_t i = 0; i < takes.size(); ++i)
            if ((int) i != c.activeTake && (takes[i].audio == nullptr || takes[i].audio->getNumFrames() != frames))
                return false;

        return true;
    }

    /** After the clip's audio is replaced by processed audio (saved as file): its takes, each
        processed alike from the part the clip played so all start at 0; or none. */
    void keepTakes (SessionClip& c, const juce::File& file, std::vector<SessionClip::Take> takes)
    {
        if (takes.empty())
        {
            c.takes.clear();
            c.activeTake = 0;
            return;
        }

        takes[(size_t) c.activeTake] = { file, c.audio, 0 };

        for (auto& t : takes)
            t.shift = 0;

        c.takes = std::move (takes);
    }
}

bool reverseClip (SessionState& s, juce::uint32 clipId, const juce::File& file, AudioDocument::Snapshot reversed,
                  std::vector<SessionClip::Take> takes)
{
    auto* c = findUnlocked (s, clipId);

    if (c == nullptr || reversed == nullptr || ! takesMatch (*c, takes, reversed->getNumFrames()))
        return false;

    // The line across the part heard, back to front, as the new audio is.
    auto line = lineAcross (c->gainEnvelope, c->offset, c->offset + c->length);
    std::reverse (line.points.begin(), line.points.end());

    for (auto& p : line.points)
        p.frame = c->length - 1 - p.frame;

    c->gainEnvelope = std::move (line);
    c->source = file;
    c->length = reversed->getNumFrames();
    c->audio = std::move (reversed);
    c->offset = 0;
    std::swap (c->fadeIn, c->fadeOut);
    keepTakes (*c, file, std::move (takes));
    limitFades (*c);
    return true;
}

AudioDocument::Snapshot stretchedAudio (const SessionClip& c, double ratio, double sampleRate, const std::function<bool (float)>& progress)
{
    if (c.audio == nullptr || c.length <= 0)
        return nullptr;

    juce::AudioBuffer<float> part (c.audio->getNumChannels(), (int) c.length);
    c.audio->read (part, 0, c.offset, (int) c.length);
    auto stretched = TimeStretch::stretch (part, ratio, sampleRate, progress);
    return stretched.getNumSamples() > 0 ? AudioEdits::fromBuffer (std::move (stretched)) : nullptr;
}

AudioDocument::Snapshot pitchedAudio (const SessionClip& c, double semitones, double sampleRate, const std::function<bool (float)>& progress)
{
    if (c.audio == nullptr || c.length <= 0)
        return nullptr;

    juce::AudioBuffer<float> part (c.audio->getNumChannels(), (int) c.length);
    c.audio->read (part, 0, c.offset, (int) c.length);
    auto shifted = TimeStretch::shiftPitch (part, semitones, sampleRate, progress);
    return shifted.getNumSamples() > 0 ? AudioEdits::fromBuffer (std::move (shifted)) : nullptr;
}

juce::Range<juce::int64> pitchRange (const SessionClip& c, juce::Range<juce::int64> timeSelection)
{
    const juce::Range<juce::int64> played (c.position, c.getEnd());
    const auto under = played.getIntersectionWith (timeSelection);
    const auto in = under.isEmpty() ? played : under;
    return in - c.position + c.offset;
}

AudioDocument::Snapshot playedPart (const SessionClip& c, const AudioDocument::Snapshot& audio)
{
    if (audio == nullptr || c.length <= 0 || c.offset + c.length > audio->getNumFrames())
        return nullptr;

    juce::AudioBuffer<float> part (audio->getNumChannels(), (int) c.length);
    audio->read (part, 0, c.offset, (int) c.length);
    return AudioEdits::fromBuffer (std::move (part));
}

SessionClip takeAsPlayed (const SessionClip& c, int take)
{
    auto copy = c;
    copy.takes.clear();
    copy.activeTake = 0;

    if (! juce::isPositiveAndBelow (take, (int) c.takes.size()) || take == c.activeTake || c.takes[(size_t) take].audio == nullptr || c.length <= 0)
        return copy;

    const auto& from = c.takes[(size_t) c.activeTake];
    const auto& to = c.takes[(size_t) take];
    juce::AudioBuffer<float> part (to.audio->getNumChannels(), (int) c.length);
    to.audio->read (part, 0, c.offset + to.shift - from.shift, (int) c.length);
    copy.source = to.source;
    copy.audio = AudioEdits::fromBuffer (std::move (part));
    copy.offset = 0;
    return copy;
}

bool replaceActiveTake (SessionState& s, juce::uint32 clipId, const juce::File& file, AudioDocument::Snapshot audio)
{
    auto* c = findUnlocked (s, clipId);

    if (c == nullptr || audio == nullptr || c->length <= 0 || audio->getNumFrames() != c->length)
        return false;

    if (c->takes.empty())
        return stretchClip (s, clipId, file, std::move (audio));

    // Each take read from where it was against the clip, now that the active one starts at 0.
    const auto activeShift = c->takes[(size_t) c->activeTake].shift;

    for (auto& t : c->takes)
        t.shift = t.shift - activeShift + c->offset;

    auto& active = c->takes[(size_t) c->activeTake];
    active.source = file;
    active.audio = audio;
    active.shift = 0;

    for (auto& p : c->gainEnvelope.points)
        p.frame -= c->offset;

    c->source = file;
    c->audio = std::move (audio);
    c->offset = 0;
    return true;
}

bool stretchClip (SessionState& s, juce::uint32 clipId, const juce::File& file, AudioDocument::Snapshot stretched, bool keepEnd,
                  std::vector<SessionClip::Take> takes)
{
    auto* c = findUnlocked (s, clipId);

    if (c == nullptr || stretched == nullptr || c->length <= 0 || (keepEnd && stretched->getNumFrames() > c->getEnd()))
        return false;

    if (! takesMatch (*c, takes, stretched->getNumFrames()))
        return false;

    if (keepEnd)
        c->position = c->getEnd() - stretched->getNumFrames();

    const auto scale = (double) stretched->getNumFrames() / (double) c->length;
    auto line = lineAcross (c->gainEnvelope, c->offset, c->offset + c->length);

    for (auto& p : line.points)
        p.frame = (juce::int64) std::llround ((double) p.frame * scale);

    c->gainEnvelope = std::move (line);
    c->source = file;
    c->length = stretched->getNumFrames();
    c->audio = std::move (stretched);
    c->offset = 0;
    c->fadeIn = (juce::int64) std::llround ((double) c->fadeIn * scale);
    c->fadeOut = (juce::int64) std::llround ((double) c->fadeOut * scale);

    keepTakes (*c, file, std::move (takes));
    limitFades (*c);
    return true;
}

int fitRegionsToClips (SessionState& s)
{
    auto changed = 0;

    for (auto& m : s.markers)
    {
        if (! m.isRegion())
            continue;

        const juce::Range<juce::int64> region (m.start, m.getEnd());
        juce::Range<juce::int64> span;
        auto any = false;

        for (auto& t : s.tracks)
            for (auto& c : t.clips)
                if (const auto overlap = region.getIntersectionWith ({ c.position, c.getEnd() }); ! overlap.isEmpty())
                {
                    span = any ? span.getUnionWith (overlap) : overlap;
                    any = true;
                }

        if (any && span != region)
        {
            m.start = span.getStart();
            m.length = span.getLength();
            ++changed;
        }
    }

    MarkerEdits::sort (s.markers);
    return changed;
}

int regionsOverClips (SessionState& s)
{
    std::vector<juce::Range<juce::int64>> spans;

    for (auto& t : s.tracks)
        for (auto& c : t.clips)
            if (c.length > 0)
                spans.emplace_back (c.position, c.getEnd());

    std::sort (spans.begin(), spans.end(), [] (auto a, auto b) { return a.getStart() < b.getStart(); });
    std::vector<juce::Range<juce::int64>> groups;

    for (auto span : spans)
    {
        if (! groups.empty() && span.getStart() <= groups.back().getEnd())
            groups.back() = groups.back().getUnionWith (span);
        else
            groups.push_back (span);
    }

    auto added = 0;

    for (auto group : groups)
    {
        if (std::any_of (s.markers.begin(), s.markers.end(), [group] (const Marker& m)
                         { return m.isRegion() && m.start == group.getStart() && m.getEnd() == group.getEnd(); }))
            continue;

        s.markers.push_back ({ group.getStart(), group.getLength(), MarkerEdits::nextName (s.markers, true) });
        ++added;
    }

    MarkerEdits::sort (s.markers);
    return added;
}

bool setClipFadeShape (SessionState& s, juce::uint32 clipId, bool fadeOut, FadeShape shape)
{
    auto* c = findUnlocked (s, clipId);

    if (c == nullptr || (fadeOut ? c->fadeOutShape : c->fadeInShape) == shape)
        return false;

    (fadeOut ? c->fadeOutShape : c->fadeInShape) = shape;
    return true;
}

bool clearRange (SessionState& s, int track, juce::int64 start, juce::int64 end)
{
    if (! juce::isPositiveAndBelow (track, (int) s.tracks.size()) || end <= start || s.tracks[(size_t) track].isFrozen())
        return false;

    auto& clips = s.tracks[(size_t) track].clips;
    std::vector<SessionClip> kept;
    auto changed = false;

    for (auto& c : clips)
    {
        if (end <= c.position || start >= c.getEnd() || c.locked)
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
    if (audio != nullptr && juce::isPositiveAndBelow (track, (int) s.tracks.size()) && s.tracks[(size_t) track].takesAudioClips())
        clearRange (s, track, position, position + audio->getNumFrames());

    return addClip (s, track, source, std::move (audio), position);
}

juce::uint32 recordTakes (SessionState& s, int track, const juce::File& source, AudioDocument::Snapshot audio,
                          juce::int64 position, juce::int64 length, const std::vector<juce::int64>& offsets)
{
    if (audio == nullptr || offsets.empty() || length <= 0)
        return 0;

    if (juce::isPositiveAndBelow (track, (int) s.tracks.size()) && s.tracks[(size_t) track].takesAudioClips())
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

int closeGaps (SessionState& s, int track)
{
    if (! juce::isPositiveAndBelow (track, (int) s.tracks.size()) || s.tracks[(size_t) track].isFrozen())
        return 0;

    auto& clips = s.tracks[(size_t) track].clips;
    std::vector<size_t> order (clips.size());
    std::iota (order.begin(), order.end(), (size_t) 0);
    std::stable_sort (order.begin(), order.end(), [&clips] (auto a, auto b) { return clips[a].position < clips[b].position; });

    // A clip that overlapped the ones before it moves with them, by the same amount, keeping the
    // overlap; one after a gap starts where they now end.
    auto moved = 0;
    juce::int64 shift = 0;
    std::optional<juce::int64> oldEnd, newEnd;

    for (auto i : order)
    {
        auto& c = clips[i];
        const auto from = c.position;

        if (c.locked)
            shift = 0;
        else if (oldEnd.has_value() && from < *oldEnd)
            c.position = juce::jmax ((juce::int64) 0, from + shift);
        else if (newEnd.has_value() && from > *newEnd)
            c.position = *newEnd;

        if (! c.locked && ! (oldEnd.has_value() && from < *oldEnd))
            shift = c.position - from;

        moved += c.position != from ? 1 : 0;
        oldEnd = juce::jmax (oldEnd.value_or (0), from + c.length);
        newEnd = juce::jmax (newEnd.value_or (0), c.getEnd());
    }

    return moved;
}

int quantizeClips (SessionState& s, const std::vector<juce::uint32>& clipIds, const std::function<juce::int64 (juce::int64)>& nearestLine)
{
    auto moved = 0;

    for (auto id : clipIds)
    {
        const auto where = s.findClip (id);

        if (! where.has_value())
            continue;

        const auto from = s.tracks[(size_t) where->first].clips[(size_t) where->second].position;
        const auto to = juce::jmax ((juce::int64) 0, nearestLine (from));

        if (to != from && moveClip (s, id, where->first, to))
            ++moved;
    }

    return moved;
}

int quantizeNoteClips (SessionState& s, const std::vector<juce::uint32>& clipIds, const std::function<juce::int64 (juce::int64)>& nearestLine)
{
    auto moved = 0;

    for (auto id : clipIds)
    {
        const auto where = s.findNoteClip (id);

        if (! where.has_value())
            continue;

        const auto from = s.tracks[(size_t) where->first].noteClips[(size_t) where->second].position;
        const auto to = juce::jmax ((juce::int64) 0, nearestLine (from));

        if (to != from && moveNoteClip (s, id, where->first, to))
            ++moved;
    }

    return moved;
}

bool clearTime (SessionState& s, juce::int64 start, juce::int64 end)
{
    auto changed = false;

    for (int t = 0; t < (int) s.tracks.size(); ++t)
    {
        changed = clearRange (s, t, start, end) || changed;
        changed = clearNoteRange (s, t, start, end) || changed;
    }

    return changed;
}

namespace
{
    /** The envelope with [start, end) taken out: the line up to start and the line from end on
        are kept as they were, end now falling at start. */
    Envelope withoutTime (const Envelope& e, juce::int64 start, juce::int64 end)
    {
        if (e.points.empty() || e.points.back().frame < start)
            return e;

        Envelope out;

        for (auto& p : e.points)
            if (p.frame < start)
                out.points.push_back (p);

        if (! out.points.empty() && out.points.back().frame < start - 1)
            out.points.push_back ({ start - 1, e.valueAt (start - 1) });

        out.points.push_back ({ start, e.valueAt (end) });

        for (auto& p : e.points)
            if (p.frame > end)
                out.points.push_back ({ p.frame - (end - start), p.value });

        return out;
    }

    /** The envelope with `length` frames put in at `at`, holding its value there across them. */
    Envelope withTime (const Envelope& e, juce::int64 at, juce::int64 length)
    {
        if (e.points.empty() || e.points.back().frame < at)
            return e;

        Envelope out;
        const auto value = e.valueAt (at);
        const auto pointAt = std::any_of (e.points.begin(), e.points.end(), [at] (auto& p) { return p.frame == at; });

        for (auto& p : e.points)
            if (p.frame < at)
                out.points.push_back (p);

        if (! out.points.empty())
        {
            out.points.push_back ({ at, value });

            if (! pointAt)
                out.points.push_back ({ at + length, value });
        }

        for (auto& p : e.points)
            if (p.frame >= at)
                out.points.push_back ({ p.frame + length, p.value });

        return out;
    }

    /** Applies an envelope change to every envelope on every track; true when one changed. */
    bool changeEnvelopes (SessionState& s, const std::function<Envelope (const Envelope&)>& change)
    {
        auto changed = false;

        const auto update = [&] (Envelope& e)
        {
            auto next = change (e);
            changed = changed || next != e;
            e = std::move (next);
        };

        for (auto& track : s.tracks)
        {
            update (track.volumeEnvelope);
            update (track.panEnvelope);

            for (auto& e : track.surroundEnvelopes)
                update (e);

            for (auto& effect : track.effectEnvelopes)
                update (effect.envelope);
        }

        return changed;
    }

    /** Applies a marker change; true when the markers changed. */
    bool changeMarkers (SessionState& s, Markers next)
    {
        const auto changed = next != s.markers;
        s.markers = std::move (next);
        return changed;
    }
}

bool deleteTime (SessionState& s, juce::int64 start, juce::int64 end)
{
    if (start < 0 || end <= start)
        return false;

    auto changed = clearTime (s, start, end);

    for (auto& track : s.tracks)
        for (auto& c : track.clips)
            if (! c.locked && ! track.isFrozen() && c.position >= end)
            {
                c.position -= end - start;
                changed = true;
            }

    // Note clips: cleared by clearTime; the ones after the time move back.
    for (auto& track : s.tracks)
        for (auto& c : track.noteClips)
            if (! track.isFrozen() && c.position >= end)
            {
                c.position -= end - start;
                changed = true;
            }

    changed = changeEnvelopes (s, [=] (const Envelope& e) { return withoutTime (e, start, end); }) || changed;
    return changeMarkers (s, MarkerEdits::afterReplace (s.markers, start, end, 0)) || changed;
}

bool insertTime (SessionState& s, juce::int64 at, juce::int64 length)
{
    if (at < 0 || length <= 0)
        return false;

    auto changed = false;
    std::vector<juce::uint32> across;

    for (auto& track : s.tracks)
        for (auto& c : track.clips)
            if (! c.locked && ! track.isFrozen() && c.position < at && at < c.getEnd())
                across.push_back (c.id);

    for (auto id : across)
        changed = splitClip (s, id, at) || changed;

    for (auto& track : s.tracks)
        for (auto& c : track.clips)
            if (! c.locked && ! track.isFrozen() && c.position >= at)
            {
                c.position += length;
                changed = true;
            }

    // Note clips across `at` are split there; from `at` on they move later.
    std::vector<juce::uint32> notesAcross;

    for (auto& track : s.tracks)
        for (auto& c : track.noteClips)
            if (! track.isFrozen() && c.position < at && at < c.getEnd())
                notesAcross.push_back (c.id);

    for (auto id : notesAcross)
        changed = splitNoteClip (s, id, at) || changed;

    for (auto& track : s.tracks)
        for (auto& c : track.noteClips)
            if (! track.isFrozen() && c.position >= at)
            {
                c.position += length;
                changed = true;
            }

    changed = changeEnvelopes (s, [=] (const Envelope& e) { return withTime (e, at, length); }) || changed;
    return changeMarkers (s, MarkerEdits::afterReplace (s.markers, at, at, length)) || changed;
}

namespace
{
    /** `before` (an envelope as it was, or with no points, the constant `setting`) with `length`
        frames put in at `at` holding `line` (from lineAcross). The line before `at` and after
        the new time keep their shape. */
    Envelope withLine (const Envelope& before, const Envelope& line, juce::int64 at, juce::int64 length, float setting)
    {
        const auto base = before.isEmpty() ? Envelope { { { 0, setting } } } : before;
        Envelope out;

        for (auto& p : base.points)
            if (p.frame < at)
                out.points.push_back (p);

        if (at > 0 && (out.points.empty() || out.points.back().frame < at - 1))
            out.points.push_back ({ at - 1, base.valueAt (at - 1) });

        for (auto& p : line.points)
            out.points.push_back ({ p.frame + at, p.value });

        out.points.push_back ({ at + length, base.valueAt (at) });

        for (auto& p : base.points)
            if (p.frame > at)
                out.points.push_back ({ p.frame + length, p.value });

        return out;
    }
}

SessionTime copyTime (const SessionState& s, juce::int64 start, juce::int64 end)
{
    SessionTime t;

    if (start < 0 || end <= start)
        return t;

    t.length = end - start;
    t.sampleRate = s.sampleRate;
    t.markers = MarkerEdits::afterCrop (s.markers, start, end);

    for (auto& track : s.tracks)
    {
        auto& copy = t.tracks.emplace_back();

        for (auto& c : track.clips)
        {
            if (c.getEnd() <= start || c.position >= end)
                continue;

            auto cut = c;
            const auto from = juce::jmax (c.position, start), to = juce::jmin (c.getEnd(), end);
            cut.offset += from - c.position;
            cut.position = from - start;
            cut.length = to - from;
            cut.fadeIn = from > c.position ? 0 : cut.fadeIn;
            cut.fadeOut = to < c.getEnd() ? 0 : cut.fadeOut;
            cut.locked = false;
            limitFades (cut);
            copy.clips.push_back (std::move (cut));
        }

        for (auto& c : track.noteClips)
        {
            if (c.getEnd() <= start || c.position >= end)
                continue;

            auto cut = c;
            const auto from = juce::jmax (c.position, start), to = juce::jmin (c.getEnd(), end);
            cut.offset += from - c.position;
            cut.position = from - start;
            cut.length = to - from;
            copy.noteClips.push_back (std::move (cut));
        }

        copy.volume = lineAcross (track.volumeEnvelope, start, end);
        copy.pan = lineAcross (track.panEnvelope, start, end);

        for (size_t e = 0; e < copy.surround.size(); ++e)
            copy.surround[e] = lineAcross (track.surroundEnvelopes[e], start, end);

        for (auto& e : track.effectEnvelopes)
            copy.effects.push_back ({ e.slotId, e.parameterId, lineAcross (e.envelope, start, end) });
    }

    return t;
}

bool pasteTime (SessionState& s, const SessionTime& t, juce::int64 at)
{
    if (t.length <= 0 || at < 0 || ! juce::exactlyEqual (t.sampleRate, s.sampleRate))
        return false;

    const auto before = s.tracks;
    insertTime (s, at, t.length);

    for (size_t i = 0; i < juce::jmin (s.tracks.size(), t.tracks.size()); ++i)
    {
        auto& track = s.tracks[i];
        const auto& copied = t.tracks[i];

        for (auto c : ! track.takesAudioClips() ? std::vector<SessionClip>() : copied.clips)
        {
            c.id = s.nextId++;
            c.groupId = 0;
            c.position += at;
            track.clips.push_back (std::move (c));
        }

        for (auto c : ! track.takesNoteClips() ? std::vector<NoteClip>() : copied.noteClips)
        {
            c.id = s.nextId++;
            c.position += at;
            track.noteClips.push_back (std::move (c));
        }

        if (! copied.volume.isEmpty())
            track.volumeEnvelope = withLine (before[i].volumeEnvelope, copied.volume, at, t.length, before[i].gainDb);

        if (! copied.pan.isEmpty())
            track.panEnvelope = withLine (before[i].panEnvelope, copied.pan, at, t.length, before[i].pan);

        for (size_t e = 0; e < copied.surround.size(); ++e)
            if (! copied.surround[e].isEmpty())
                track.surroundEnvelopes[e] = withLine (before[i].surroundEnvelopes[e], copied.surround[e], at, t.length,
                                                       before[i].valueAt (surroundParameters[e], 0));

        for (size_t e = 0; e < track.effectEnvelopes.size(); ++e)
        {
            auto& effect = track.effectEnvelopes[e];
            const auto line = std::find_if (copied.effects.begin(), copied.effects.end(), [&effect] (auto& c)
                                            { return c.slotId == effect.slotId && c.parameterId == effect.parameterId; });

            if (line != copied.effects.end())
                effect.envelope = withLine (before[i].effectEnvelopes[e].envelope, line->envelope, at, t.length, 0.0f);
        }
    }

    for (auto m : t.markers)
    {
        m.start += at;
        s.markers.push_back (std::move (m));
    }

    MarkerEdits::sort (s.markers);
    return true;
}

namespace
{
    /** Where a musical track's clip is in beats, to put it back there once the tempo has changed. */
    struct MusicalPlace
    {
        juce::uint32 id = 0;
        bool notes = false;
        double start = 0.0, end = 0.0;     // beats
    };

    std::vector<MusicalPlace> musicalPlaces (const SessionState& s)
    {
        std::vector<MusicalPlace> places;
        const auto map = s.tempoMap();

        for (auto& t : s.tracks)
        {
            if (! t.musical)
                continue;

            for (auto& c : t.clips)
                places.push_back ({ c.id, false, map.beatPosition (c.position), 0.0 });

            for (auto& c : t.noteClips)
                places.push_back ({ c.id, true, map.beatPosition (c.position), map.beatPosition (c.getEnd()) });
        }

        return places;
    }

    /** Each clip back on its beat under the tempo as it is now: an audio clip moved (a locked one
        left), a note clip scaled to its bars' new length and moved. */
    void keepMusicalPlaces (SessionState& s, const std::vector<MusicalPlace>& places)
    {
        const auto map = s.tempoMap();

        for (auto& p : places)
        {
            const auto start = map.frameAtBeatPosition (p.start);

            if (! p.notes)
            {
                if (const auto where = s.findClip (p.id))
                    moveClip (s, p.id, where->first, start);

                continue;
            }

            const auto where = s.findNoteClip (p.id);

            if (! where.has_value())
                continue;

            const auto length = juce::jmax ((juce::int64) 1, map.frameAtBeatPosition (p.end) - start);
            const auto was = s.tracks[(size_t) where->first].noteClips[(size_t) where->second].length;

            if (length != was)
                stretchNoteClip (s, p.id, (double) length / (double) was, false);

            moveNoteClip (s, p.id, where->first, start);
        }
    }
}

bool setTempo (SessionState& s, double tempo, int beatsPerBar)
{
    if (tempo < 20.0 || tempo > 400.0 || beatsPerBar < 1 || beatsPerBar > 16 || (juce::exactlyEqual (tempo, s.tempo) && beatsPerBar == s.beatsPerBar))
        return false;

    const auto places = musicalPlaces (s);
    s.tempo = tempo;
    s.beatsPerBar = beatsPerBar;
    keepMusicalPlaces (s, places);
    return true;
}

bool setTempoChange (SessionState& s, Metronome::TempoChange change)
{
    if (change.bar < 1 || change.tempo < 20.0 || change.tempo > 400.0 || change.beatsPerBar < 1 || change.beatsPerBar > 16)
        return false;

    auto& changes = s.tempoChanges;
    const auto at = std::lower_bound (changes.begin(), changes.end(), change.bar, [] (auto& c, juce::int64 bar) { return c.bar < bar; });
    const auto replacing = at != changes.end() && at->bar == change.bar;

    if (replacing && *at == change)
        return false;

    const auto places = musicalPlaces (s);

    if (replacing)
        *at = change;
    else
        changes.insert (at, change);

    keepMusicalPlaces (s, places);
    return true;
}

bool removeTempoChange (SessionState& s, juce::int64 bar)
{
    auto& changes = s.tempoChanges;
    const auto before = changes.size();
    const auto places = musicalPlaces (s);
    changes.erase (std::remove_if (changes.begin(), changes.end(), [bar] (auto& c) { return c.bar == bar; }), changes.end());

    if (changes.size() == before)
        return false;

    keepMusicalPlaces (s, places);
    return true;
}

bool freezeTrack (SessionState& s, juce::uint32 trackId, const juce::File& file, AudioDocument::Snapshot audio)
{
    const auto track = std::find_if (s.tracks.begin(), s.tracks.end(), [trackId] (auto& t) { return t.id == trackId; });

    if (track == s.tracks.end() || track->isBus || track->isFolder || audio == nullptr)
        return false;

    track->frozenAudio = std::move (audio);
    track->frozenFile = file;
    return true;
}

bool unfreezeTrack (SessionState& s, juce::uint32 trackId)
{
    const auto track = std::find_if (s.tracks.begin(), s.tracks.end(), [trackId] (auto& t) { return t.id == trackId; });

    if (track == s.tracks.end() || ! track->isFrozen())
        return false;

    track->frozenAudio = nullptr;
    track->frozenFile = juce::File();
    return true;
}

void addKeys (const SessionState& from, SessionState& alone)
{
    for (size_t i = 0; i < alone.tracks.size(); ++i)   // grows as keys come in, whose keys come in too
    {
        std::vector<juce::uint32> keys;

        if (alone.tracks[i].ducking.isOn())
            keys.push_back (alone.tracks[i].ducking.keyTrackId);

        for (auto& chain : alone.tracks[i].sidechains)
            keys.push_back (chain.keyTrackId);

        for (const auto k : keys)
        {
            const auto* key = from.findTrack (k);

            if (key == nullptr || key->isBus || key->isFolder || alone.findTrack (k) != nullptr)
                continue;

            auto copy = *key;
            copy.sends.clear();
            copy.outputBusId = 0;
            copy.deviceOutput = 0;
            copy.solo = false;

            if (const auto* folder = key->folderId != 0 ? from.findTrack (key->folderId) : nullptr)
            {
                copy.mute = copy.mute || folder->mute;
                copy.gainDb += folder->gainDb;
            }

            copy.folderId = 0;
            alone.tracks.push_back (std::move (copy));
        }
    }
}

SessionState frozenSource (const SessionState& s, juce::uint32 trackId)
{
    auto alone = s;
    alone.tracks.clear();
    // The track's own sound: stereo, which its surround pan places afterwards, or a 5.1 or 7.1
    // track's six or eight channels.
    const auto* found = s.findTrack (trackId);
    alone.surround = found != nullptr && found->isWide();
    alone.sevenOne = found != nullptr && found->sevenOne;

    if (const auto* track = s.findTrack (trackId))
    {
        auto t = *track;
        t.sends.clear();
        t.outputBusId = 0;   // applied after the frozen audio, as its volume is
        t.deviceOutput = -1;
        t.ducking = {};
        t.mute = t.solo = false;
        t.invertPolarity = false;   // applied after the frozen audio, as after live effects
        t.gainDb = 0.0f;
        t.volumeEnvelope = {};
        t.frozenAudio = nullptr;
        t.frozenFile = juce::File();
        alone.tracks.push_back (std::move (t));
    }

    addKeys (s, alone);   // what its effects' sidechains listen to
    return alone;
}

juce::uint32 duplicateTrack (SessionState& s, int track)
{
    if (! juce::isPositiveAndBelow (track, (int) s.tracks.size()))
        return 0;

    auto copy = s.tracks[(size_t) track];
    copy.id = s.nextId++;
    copy.name = copy.name + " Copy";

    for (auto& c : copy.clips)
    {
        c.id = s.nextId++;
        c.locked = false;
        c.groupId = 0;
    }

    for (auto& c : copy.noteClips)
        c.id = s.nextId++;

    s.tracks.insert (s.tracks.begin() + track + 1, std::move (copy));
    return s.tracks[(size_t) track + 1].id;
}

bool removeTrack (SessionState& s, int track)
{
    if (! juce::isPositiveAndBelow (track, (int) s.tracks.size()))
        return false;

    const auto removed = s.tracks[(size_t) track].id;
    s.tracks.erase (s.tracks.begin() + track);

    for (auto& t : s.tracks)
    {
        t.sends.erase (std::remove_if (t.sends.begin(), t.sends.end(), [removed] (auto& send) { return send.busId == removed; }),
                       t.sends.end());

        if (t.folderId == removed)
            t.folderId = 0;

        if (t.outputBusId == removed)
            t.outputBusId = 0;

        if (t.ducking.keyTrackId == removed)
            t.ducking.keyTrackId = 0;

        t.sidechains.erase (std::remove_if (t.sidechains.begin(), t.sidechains.end(), [removed] (auto& c) { return c.keyTrackId == removed; }),
                            t.sidechains.end());
    }

    return true;
}

bool setTrackFolder (SessionState& s, juce::uint32 trackId, juce::uint32 folderId)
{
    const auto track = std::find_if (s.tracks.begin(), s.tracks.end(), [trackId] (auto& t) { return t.id == trackId; });
    const auto* folder = folderId != 0 ? s.findTrack (folderId) : nullptr;

    if (track == s.tracks.end() || track->isFolder || track->folderId == folderId || (folderId != 0 && (folder == nullptr || ! folder->isFolder)))
        return false;

    track->folderId = folderId;
    return true;
}

bool setTrackOutput (SessionState& s, juce::uint32 trackId, juce::uint32 busId)
{
    const auto* bus = busId != 0 ? s.findTrack (busId) : nullptr;
    auto track = std::find_if (s.tracks.begin(), s.tracks.end(), [trackId] (auto& t) { return t.id == trackId; });

    if (track == s.tracks.end() || track->isFolder || (track->outputBusId == busId && track->deviceOutput < 0)
        || (busId != 0 && (bus == nullptr || ! bus->isBus)))
        return false;

    // A bus may not play into itself, nor into a bus whose output leads back to it.
    if (busId != 0 && track->isBus && (busId == trackId || s.feeds (*bus, trackId)))
        return false;

    track->outputBusId = busId;
    track->deviceOutput = -1;
    return true;
}

bool setTrackDeviceOutput (SessionState& s, juce::uint32 trackId, int firstChannel)
{
    auto track = std::find_if (s.tracks.begin(), s.tracks.end(), [trackId] (auto& t) { return t.id == trackId; });

    if (track == s.tracks.end() || track->isFolder || firstChannel < -1 || firstChannel >= SessionTrack::maxDeviceOutputs - 1
        || (track->deviceOutput == firstChannel && track->outputBusId == 0))
        return false;

    track->outputBusId = 0;
    track->deviceOutput = firstChannel;
    return true;
}

bool setTrackFiveOne (SessionState& s, juce::uint32 trackId, bool fiveOne)
{
    return setTrackChannels (s, trackId, fiveOne ? 6 : 2);
}

bool setTrackChannels (SessionState& s, juce::uint32 trackId, int channels)
{
    auto track = std::find_if (s.tracks.begin(), s.tracks.end(), [trackId] (auto& t) { return t.id == trackId; });

    if ((channels != 2 && channels != 6 && channels != 8) || track == s.tracks.end() || track->isBus || track->isFolder
        || track->isFrozen() || juce::jmax (2, track->wideChannels()) == channels)
        return false;

    track->fiveOne = channels == 6;
    track->sevenOne = channels == 8;
    return true;
}

bool setSurround (SessionState& s, bool fiveOne)
{
    return setMasterChannels (s, fiveOne ? 6 : 2);
}

bool setMasterChannels (SessionState& s, int channels)
{
    if ((channels != 2 && channels != 6 && channels != 8) || s.masterChannels() == channels)
        return false;

    s.surround = channels > 2;
    s.sevenOne = channels == 8;
    return true;
}

bool setSurroundPan (SessionState& s, juce::uint32 trackId, SurroundPan pan)
{
    auto track = std::find_if (s.tracks.begin(), s.tracks.end(), [trackId] (auto& t) { return t.id == trackId; });

    if (track == s.tracks.end() || track->isFolder)
        return false;

    pan.x = juce::jlimit (-1.0f, 1.0f, std::isfinite (pan.x) ? pan.x : 0.0f);
    pan.y = juce::jlimit (-1.0f, 1.0f, std::isfinite (pan.y) ? pan.y : 1.0f);
    pan.width = juce::jlimit (0.0f, 1.0f, std::isfinite (pan.width) ? pan.width : 1.0f);
    pan.lfeDb = ! std::isfinite (pan.lfeDb) || pan.lfeDb < -60.0f ? SurroundPan::lfeOff : juce::jmin (pan.lfeDb, SurroundPan::maxLfeDb);

    if (track->surroundPan == pan)
        return false;

    track->surroundPan = pan;
    return true;
}

bool setDucking (SessionState& s, juce::uint32 trackId, const Ducking& ducking)
{
    auto track = std::find_if (s.tracks.begin(), s.tracks.end(), [trackId] (auto& t) { return t.id == trackId; });

    if (track == s.tracks.end() || track->isBus || track->isFolder || ! ducking.isValid())
        return false;

    if (ducking.isOn())
    {
        const auto* key = s.findTrack (ducking.keyTrackId);

        if (key == nullptr || key->isBus || key->isFolder || key->id == trackId)
            return false;

        // No loops: the key may not listen to this track, through ducking or sidechains.
        if (s.listensTo (*key, trackId))
            return false;
    }

    const auto& d = track->ducking;

    if (d.keyTrackId == ducking.keyTrackId && juce::exactlyEqual (d.amountDb, ducking.amountDb) && juce::exactlyEqual (d.thresholdDb, ducking.thresholdDb)
        && juce::exactlyEqual (d.attackMs, ducking.attackMs) && juce::exactlyEqual (d.releaseMs, ducking.releaseMs))
        return false;

    track->ducking = ducking;
    return true;
}

bool setSidechain (SessionState& s, juce::uint32 trackId, juce::uint32 slotId, juce::uint32 keyTrackId)
{
    auto track = std::find_if (s.tracks.begin(), s.tracks.end(), [trackId] (auto& t) { return t.id == trackId; });

    if (track == s.tracks.end() || track->isFolder || slotId == 0 || track->sidechainKey (slotId) == keyTrackId)
        return false;

    if (keyTrackId != 0)
    {
        const auto* key = s.findTrack (keyTrackId);

        if (key == nullptr || key->isBus || key->isFolder || key->id == trackId || s.listensTo (*key, trackId))
            return false;
    }

    auto& list = track->sidechains;
    list.erase (std::remove_if (list.begin(), list.end(), [slotId] (auto& c) { return c.slotId == slotId; }), list.end());

    if (keyTrackId != 0)
        list.push_back ({ slotId, keyTrackId });

    return true;
}

bool setSend (SessionState& s, juce::uint32 trackId, const SessionSend& send)
{
    const auto* bus = s.findTrack (send.busId);
    auto track = std::find_if (s.tracks.begin(), s.tracks.end(), [trackId] (auto& t) { return t.id == trackId; });

    if (bus == nullptr || ! bus->isBus || track == s.tracks.end() || track->isBus || track->isFolder)
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
            else if (target.parameter != AutomationParameter::effect)
            {
                if (t.isFolder)
                    return false;

                t.surroundEnvelopes[(size_t) ((int) target.parameter - (int) AutomationParameter::surroundX)] = std::move (e);
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

NoteClip tidiedNoteClip (NoteClip c)
{
    c.name = c.name.trim().isEmpty() ? juce::String ("Notes") : c.name.trim();
    c.position = juce::jmax ((juce::int64) 0, c.position);
    c.offset = juce::jmax ((juce::int64) 0, c.offset);
    c.length = juce::jmax ((juce::int64) 1, c.length);
    c.loopLength = juce::jlimit ((juce::int64) 0, c.offset + c.length, c.loopLength);
    c.colour = juce::jlimit (-1, (int) TrackColours::palette.size() - 1, c.colour);

    for (auto& n : c.notes)
    {
        n.pitch = juce::jlimit (0, 127, n.pitch);
        n.start = juce::jmax ((juce::int64) 0, n.start);
        n.length = juce::jmax ((juce::int64) 1, n.length);
        n.velocity = n.velocity > 0.0f ? juce::jmin (1.0f, n.velocity) : 1.0f / 127.0f;
    }

    std::vector<ControllerLane> lanes;

    for (auto lane : c.lanes)
    {
        lane.controller = juce::jlimit (0, ControllerLane::pitchBend, lane.controller);

        if (lane.envelope.isEmpty() || std::any_of (lanes.begin(), lanes.end(), [&lane] (auto& l) { return l.controller == lane.controller; }))
            continue;

        for (auto& p : lane.envelope.points)
        {
            p.frame = juce::jmax ((juce::int64) 0, p.frame);
            p.value = juce::jlimit (0.0f, 1.0f, p.value);
        }

        std::stable_sort (lane.envelope.points.begin(), lane.envelope.points.end(), [] (auto& a, auto& b) { return a.frame < b.frame; });
        lanes.push_back (std::move (lane));
    }

    c.lanes = std::move (lanes);
    return c;
}

juce::uint32 addNoteClip (SessionState& s, juce::uint32 trackId, NoteClip clip)
{
    const auto track = std::find_if (s.tracks.begin(), s.tracks.end(), [trackId] (auto& t) { return t.id == trackId; });

    if (track == s.tracks.end() || ! track->takesNoteClips())
        return 0;

    clip = tidiedNoteClip (std::move (clip));
    clip.id = s.nextId++;
    track->noteClips.push_back (clip);
    return clip.id;
}

bool replaceNoteClip (SessionState& s, juce::uint32 clipId, const NoteClip& values)
{
    const auto where = s.findNoteClip (clipId);

    if (! where.has_value())
        return false;

    auto& c = s.tracks[(size_t) where->first].noteClips[(size_t) where->second];
    auto tidy = values;
    tidy.offset = c.offset;
    tidy = tidiedNoteClip (std::move (tidy));
    c.name = tidy.name;
    c.length = tidy.length;
    c.loopLength = tidy.loopLength;
    c.notes = std::move (tidy.notes);
    c.lanes = std::move (tidy.lanes);
    c.colour = tidy.colour;
    return true;
}

bool moveNoteClip (SessionState& s, juce::uint32 clipId, int toTrack, juce::int64 position)
{
    const auto where = s.findNoteClip (clipId);

    if (! where.has_value() || toTrack < 0)
        return false;

    auto& from = s.tracks[(size_t) where->first].noteClips;
    auto clip = from[(size_t) where->second];
    clip.position = juce::jmax ((juce::int64) 0, position);

    if (toTrack < (int) s.tracks.size() && ! s.tracks[(size_t) toTrack].takesNoteClips())
        toTrack = where->first;

    if (toTrack == where->first)
    {
        from[(size_t) where->second] = std::move (clip);
        return true;
    }

    from.erase (from.begin() + where->second);

    if (toTrack >= (int) s.tracks.size())
    {
        s.addInstrumentTrack();
        toTrack = (int) s.tracks.size() - 1;
    }

    s.tracks[(size_t) toTrack].noteClips.push_back (std::move (clip));
    return true;
}

bool trimNoteClip (SessionState& s, juce::uint32 clipId, juce::int64 start, juce::int64 end)
{
    const auto where = s.findNoteClip (clipId);

    if (! where.has_value())
        return false;

    auto& c = s.tracks[(size_t) where->first].noteClips[(size_t) where->second];
    start = juce::jmax (c.getOrigin(), juce::jmax ((juce::int64) 0, start));
    end = juce::jmax (start + 1, end);
    c.offset += start - c.position;
    c.position = start;
    c.length = end - start;
    c.loopLength = juce::jlimit ((juce::int64) 0, c.offset + c.length, c.loopLength);
    return true;
}

bool splitNoteClip (SessionState& s, juce::uint32 clipId, juce::int64 frame)
{
    const auto where = s.findNoteClip (clipId);

    if (! where.has_value())
        return false;

    auto& clips = s.tracks[(size_t) where->first].noteClips;
    auto& c = clips[(size_t) where->second];

    if (frame <= c.position || frame >= c.getEnd())
        return false;

    auto right = c;
    right.id = s.nextId++;
    right.offset += frame - c.position;
    right.position = frame;
    right.length = c.getEnd() - frame;
    c.length = frame - c.position;
    clips.push_back (std::move (right));
    return true;
}

juce::uint32 placeNoteClip (SessionState& s, int track, const NoteClip& clip, juce::int64 position)
{
    if (track < 0 || track >= (int) s.tracks.size() || ! s.tracks[(size_t) track].takesNoteClips())
    {
        s.addInstrumentTrack();
        track = (int) s.tracks.size() - 1;
    }

    auto copy = tidiedNoteClip (clip);
    copy.id = s.nextId++;
    copy.position = juce::jmax ((juce::int64) 0, position);
    s.tracks[(size_t) track].noteClips.push_back (copy);
    return copy.id;
}

bool stretchNoteClip (SessionState& s, juce::uint32 clipId, double ratio, bool keepEnd)
{
    const auto where = s.findNoteClip (clipId);

    if (! where.has_value() || ! std::isfinite (ratio) || ! (ratio > 0.0))
        return false;

    ratio = juce::jlimit (1.0 / 16.0, 16.0, ratio);
    const auto scale = [ratio] (juce::int64 frames) { return (juce::int64) std::llround ((double) frames * ratio); };
    auto& c = s.tracks[(size_t) where->first].noteClips[(size_t) where->second];
    const auto end = c.getEnd();
    c.offset = scale (c.offset);
    c.length = juce::jmax ((juce::int64) 1, scale (c.length));
    c.loopLength = scale (c.loopLength);

    for (auto& n : c.notes)
    {
        n.start = scale (n.start);
        n.length = juce::jmax ((juce::int64) 1, scale (n.length));
    }

    for (auto& lane : c.lanes)
        for (auto& p : lane.envelope.points)
            p.frame = scale (p.frame);

    if (keepEnd)
        c.position = juce::jmax ((juce::int64) 0, end - c.length);

    c = tidiedNoteClip (std::move (c));
    return true;
}

NoteClip flattenedNoteClip (const NoteClip& c)
{
    auto flat = c;
    flat.notes.clear();
    flat.lanes.clear();
    flat.offset = 0;
    flat.loopLength = 0;

    const auto laneFor = [&flat] (int controller) -> ControllerLane&
    {
        for (auto& lane : flat.lanes)
            if (lane.controller == controller)
                return lane;

        flat.lanes.push_back ({ controller, {} });
        return flat.lanes.back();
    };

    const auto end = c.getEnd();
    const auto period = juce::jmax ((juce::int64) 1, c.getPeriod());

    for (auto pass = c.getOrigin(); pass < end; pass += period)
    {
        for (auto n : c.notes)
        {
            const auto at = pass + n.start;

            if (at < c.position || at >= end)
                continue;

            n.start = at - c.position;
            n.length = juce::jmin (n.length, end - at);
            flat.notes.push_back (n);
        }

        for (auto& lane : c.lanes)
            for (auto& p : lane.envelope.points)
                if (const auto at = pass + p.frame; at >= c.position && at < end)
                    laneFor (lane.controller).envelope.add (at - c.position, p.value);

        if (c.loopLength <= 0)
            break;
    }

    // Each lane starts with its value at the clip's start, as the clip did.
    for (auto& lane : c.lanes)
        if (const auto value = c.laneValueAt (lane, c.position))
        {
            auto& target = laneFor (lane.controller);

            if (target.envelope.points.empty() || target.envelope.points.front().frame != 0)
                target.envelope.add (0, *value);
        }

    std::stable_sort (flat.notes.begin(), flat.notes.end(), [] (auto& a, auto& b) { return a.start < b.start; });
    return flat;
}

bool mergeNoteClips (SessionState& s, juce::uint32 clipId, juce::uint32 otherId)
{
    const auto a = s.findNoteClip (clipId), b = s.findNoteClip (otherId);

    if (! a.has_value() || ! b.has_value() || a->first != b->first || clipId == otherId)
        return false;

    auto& clips = s.tracks[(size_t) a->first].noteClips;
    const auto first = flattenedNoteClip (clips[(size_t) a->second]), second = flattenedNoteClip (clips[(size_t) b->second]);
    const auto& earlier = first.position <= second.position ? first : second;
    auto merged = earlier;
    merged.id = clipId;
    merged.muted = clips[(size_t) a->second].muted;
    merged.position = juce::jmin (first.position, second.position);
    merged.length = juce::jmax (first.getEnd(), second.getEnd()) - merged.position;
    merged.notes.clear();
    merged.lanes.clear();

    const auto laneFor = [&merged] (int controller) -> ControllerLane&
    {
        for (auto& lane : merged.lanes)
            if (lane.controller == controller)
                return lane;

        merged.lanes.push_back ({ controller, {} });
        return merged.lanes.back();
    };

    for (const auto* part : { &first, &second })
    {
        for (auto n : part->notes)
        {
            n.start += part->position - merged.position;
            merged.notes.push_back (n);
        }

        for (auto& lane : part->lanes)
            for (auto& p : lane.envelope.points)
                laneFor (lane.controller).envelope.add (p.frame + part->position - merged.position, p.value);
    }

    std::stable_sort (merged.notes.begin(), merged.notes.end(), [] (auto& x, auto& y) { return x.start < y.start; });
    clips[(size_t) a->second] = tidiedNoteClip (std::move (merged));
    clips.erase (clips.begin() + b->second);
    return true;
}

bool clearNoteRange (SessionState& s, int track, juce::int64 start, juce::int64 end)
{
    if (! juce::isPositiveAndBelow (track, (int) s.tracks.size()) || end <= start || s.tracks[(size_t) track].isFrozen())
        return false;

    auto& clips = s.tracks[(size_t) track].noteClips;
    std::vector<NoteClip> kept;
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
            kept.push_back (std::move (left));
        }

        if (end < c.getEnd())
        {
            auto right = c;
            right.id = start > c.position ? s.nextId++ : c.id;
            right.offset += end - c.position;
            right.length = c.getEnd() - end;
            right.position = end;
            kept.push_back (std::move (right));
        }
    }

    clips = std::move (kept);
    return changed;
}

bool removeNoteClip (SessionState& s, juce::uint32 clipId)
{
    const auto where = s.findNoteClip (clipId);

    if (! where.has_value())
        return false;

    auto& clips = s.tracks[(size_t) where->first].noteClips;
    clips.erase (clips.begin() + where->second);
    return true;
}

bool setNoteClipMuted (SessionState& s, juce::uint32 clipId, bool muted)
{
    const auto where = s.findNoteClip (clipId);

    if (! where.has_value())
        return false;

    auto& c = s.tracks[(size_t) where->first].noteClips[(size_t) where->second];

    if (c.muted == muted)
        return false;

    c.muted = muted;
    return true;
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

        if (targets.beatGrid.has_value())
        {
            // The grid lines either side of the frame.
            const auto [below, above] = s.tempoMap().linesAround (frame, *targets.beatGrid);

            for (auto line : { below, above })
                if (line >= 0)
                    consider (line);
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

juce::StringArray Session::getHistory() const
{
    juce::StringArray steps;

    for (auto& entry : history)
        steps.add (entry.description);

    return steps;
}

void Session::goTo (int step)
{
    if (juce::isPositiveAndBelow (step, (int) history.size()) && step != position)
    {
        position = step;
        setCurrent (history[(size_t) position].state);
    }
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

void Session::markUnsaved()
{
    savedPosition = -1;
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
void writeEnvelopes (const SessionTrack& t, juce::XmlElement& into)
{
    const auto writeEnvelope = [&into] (const Envelope& envelope) -> juce::XmlElement*
    {
        if (envelope.isEmpty())
            return nullptr;

        auto* envelopeXml = into.createNewChildElement ("ENVELOPE");

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

    for (size_t e = 0; e < t.surroundEnvelopes.size(); ++e)
        if (auto* xml = writeEnvelope (t.surroundEnvelopes[e]))
            xml->setAttribute ("parameter", juce::StringArray { "surroundX", "surroundY", "surroundWidth", "lfe" }[(int) e]);

    for (auto& e : t.effectEnvelopes)
    {
        if (auto* xml = writeEnvelope (e.envelope))
        {
            xml->setAttribute ("parameter", "effect");
            xml->setAttribute ("slot", (int) e.slotId);
            xml->setAttribute ("id", e.parameterId);
        }
    }
}

void readEnvelopes (SessionState& state, juce::uint32 trackId, const juce::XmlElement& from, double frameScale)
{
    for (auto* envelopeXml : from.getChildWithTagNameIterator ("ENVELOPE"))
    {
        const auto name = envelopeXml->getStringAttribute ("parameter");
        AutomationTarget target;

        if (name == "volume")
            target = AutomationParameter::volume;
        else if (name == "pan")
            target = AutomationParameter::pan;
        else if (const auto e = juce::StringArray { "surroundX", "surroundY", "surroundWidth", "lfe" }.indexOf (name); e >= 0)
            target = surroundParameters[(size_t) e];
        else if (name == "effect")
            target = AutomationTarget::effect ((juce::uint32) envelopeXml->getIntAttribute ("slot"), envelopeXml->getStringAttribute ("id"));
        else
            continue;

        Envelope envelope;

        for (auto* pointXml : envelopeXml->getChildWithTagNameIterator ("POINT"))
        {
            auto frame = pointXml->getStringAttribute ("frame").getLargeIntValue();

            if (! juce::exactlyEqual (frameScale, 1.0))
                frame = (juce::int64) std::llround ((double) frame * frameScale);

            envelope.points.push_back ({ frame, (float) pointXml->getDoubleAttribute ("value") });
        }

        SessionEdits::setEnvelope (state, trackId, target, std::move (envelope));
    }
}

juce::String save (const SessionState& state, const juce::File& target,
                   const std::function<std::unique_ptr<juce::XmlElement> (juce::uint32 trackId)>& trackEffects)
{
    juce::XmlElement root ("SPECTRABLADE_SESSION");
    root.setAttribute ("version", 1);
    root.setAttribute ("sampleRate", state.sampleRate);
    root.setAttribute ("tempo", state.tempo);
    root.setAttribute ("beatsPerBar", state.beatsPerBar);

    if (state.surround)
        root.setAttribute ("surround", true);

    if (state.surround && state.sevenOne)
        root.setAttribute ("sevenOne", true);

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

        if (t.isFolder)
            track->setAttribute ("folder", true);

        if (! t.isBus && ! t.isFolder)
        {
            track->setAttribute ("instrument", t.isInstrument);
            track->setAttribute ("musical", t.musical);
        }

        if (t.isFolder && t.collapsed)
            track->setAttribute ("collapsed", true);

        if (t.folderId != 0 && indexOf.count (t.folderId) > 0)
            track->setAttribute ("inFolder", indexOf[t.folderId]);

        if (t.outputBusId != 0 && indexOf.count (t.outputBusId) > 0)
            track->setAttribute ("output", indexOf[t.outputBusId]);
        else if (t.deviceOutput >= 0)
            track->setAttribute ("deviceOutput", t.deviceOutput);

        if (t.fiveOne)
            track->setAttribute ("fiveOne", true);

        if (t.sevenOne)
            track->setAttribute ("sevenOne", true);

        if (! t.isFolder && ! t.surroundPan.isDefault())
        {
            track->setAttribute ("surroundX", t.surroundPan.x);
            track->setAttribute ("surroundY", t.surroundPan.y);
            track->setAttribute ("surroundWidth", t.surroundPan.width);

            if (t.surroundPan.lfeDb > SurroundPan::lfeOff)
                track->setAttribute ("lfeDb", t.surroundPan.lfeDb);
        }

        if (t.ducking.isOn() && indexOf.count (t.ducking.keyTrackId) > 0)
        {
            auto* duck = track->createNewChildElement ("DUCKING");
            duck->setAttribute ("key", indexOf[t.ducking.keyTrackId]);
            duck->setAttribute ("amountDb", t.ducking.amountDb);
            duck->setAttribute ("thresholdDb", t.ducking.thresholdDb);
            duck->setAttribute ("attackMs", t.ducking.attackMs);
            duck->setAttribute ("releaseMs", t.ducking.releaseMs);
        }

        for (auto& chain : t.sidechains)
        {
            if (indexOf.count (chain.keyTrackId) > 0)
            {
                auto* side = track->createNewChildElement ("SIDECHAIN");
                side->setAttribute ("slot", (int) chain.slotId);
                side->setAttribute ("key", indexOf[chain.keyTrackId]);
            }
        }

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

        if (t.soloSafe)
            track->setAttribute ("soloSafe", true);

        if (t.invertPolarity)
            track->setAttribute ("invert", true);

        if (t.group != 0)
            track->setAttribute ("group", t.group);

        if (t.colour >= 0)
            track->setAttribute ("colour", t.colour);

        if (t.hidden)
            track->setAttribute ("hidden", true);

        if (t.note.isNotEmpty())
            track->setAttribute ("note", t.note);

        if (! t.input.isDefault())
        {
            track->setAttribute ("inputFirst", t.input.first);
            track->setAttribute ("inputChannels", t.input.channels);
        }

        SessionFile::writeEnvelopes (t, *track);

        if (trackEffects != nullptr)
            if (auto effects = trackEffects (t.id))
                track->addChildElement (effects.release());

        if (t.isFrozen())
        {
            auto* frozen = track->createNewChildElement ("FROZEN");
            frozen->setAttribute ("file", t.frozenFile.getRelativePathFrom (target.getParentDirectory()));
            frozen->setAttribute ("absolute", t.frozenFile.getFullPathName());
        }

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

            if (c.fadeInShape != FadeShape::sCurve)
                clip->setAttribute ("fadeInShape", FadeShapes::id (c.fadeInShape));

            if (c.fadeOutShape != FadeShape::sCurve)
                clip->setAttribute ("fadeOutShape", FadeShapes::id (c.fadeOutShape));

            if (c.locked)
                clip->setAttribute ("locked", true);

            if (c.muted)
                clip->setAttribute ("muted", true);

            if (c.colour >= 0)
                clip->setAttribute ("colour", c.colour);

            if (c.groupId != 0)
                clip->setAttribute ("group", (int) c.groupId);

            if (c.note.isNotEmpty())
                clip->setAttribute ("note", c.note);

            if (! c.gainEnvelope.isEmpty())
            {
                auto* gainXml = clip->createNewChildElement ("GAIN");

                for (auto& point : c.gainEnvelope.points)
                {
                    auto* pointXml = gainXml->createNewChildElement ("POINT");
                    pointXml->setAttribute ("frame", juce::String (point.frame));
                    pointXml->setAttribute ("value", point.value);
                }
            }

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

        for (auto& c : t.noteClips)
        {
            auto* clip = track->createNewChildElement ("NOTECLIP");
            clip->setAttribute ("name", c.name);
            clip->setAttribute ("position", juce::String (c.position));

            if (c.offset > 0)
                clip->setAttribute ("offset", juce::String (c.offset));

            clip->setAttribute ("length", juce::String (c.length));

            if (c.loopLength > 0)
                clip->setAttribute ("loop", juce::String (c.loopLength));

            if (c.muted)
                clip->setAttribute ("muted", true);

            if (c.colour >= 0)
                clip->setAttribute ("colour", c.colour);

            for (auto& n : c.notes)
            {
                auto* note = clip->createNewChildElement ("NOTE");
                note->setAttribute ("pitch", n.pitch);
                note->setAttribute ("start", juce::String (n.start));
                note->setAttribute ("length", juce::String (n.length));
                note->setAttribute ("velocity", n.velocity);
            }

            for (auto& lane : c.lanes)
            {
                auto* laneXml = clip->createNewChildElement ("LANE");
                laneXml->setAttribute ("controller", lane.controller);

                for (auto& point : lane.envelope.points)
                {
                    auto* pointXml = laneXml->createNewChildElement ("POINT");
                    pointXml->setAttribute ("frame", juce::String (point.frame));
                    pointXml->setAttribute ("value", point.value);
                }
            }
        }
    }

    for (auto& c : state.tempoChanges)
    {
        auto* change = root.createNewChildElement ("TEMPO");
        change->setAttribute ("bar", juce::String (c.bar));
        change->setAttribute ("tempo", c.tempo);
        change->setAttribute ("beatsPerBar", c.beatsPerBar);
    }

    if (state.notes.isNotEmpty())
        root.createNewChildElement ("NOTES")->addTextElement (state.notes);

    if (state.video != juce::File())
    {
        auto* video = root.createNewChildElement ("VIDEO");
        video->setAttribute ("file", state.video.getRelativePathFrom (target.getParentDirectory()));
        video->setAttribute ("absolute", state.video.getFullPathName());
        video->setAttribute ("start", state.videoStart);
    }

    for (auto& m : state.markers)
    {
        auto* marker = root.createNewChildElement ("MARKER");
        marker->setAttribute ("name", m.name);
        marker->setAttribute ("start", juce::String (m.start));

        if (m.isRegion())
            marker->setAttribute ("length", juce::String (m.length));

        if (m.colour >= 0)
            marker->setAttribute ("colour", m.colour);
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
    state.surround = root->getBoolAttribute ("surround");
    state.sevenOne = state.surround && root->getBoolAttribute ("sevenOne");
    std::map<juce::String, AudioDocument::Snapshot> audio;   // by full path: each file read once
    std::map<int, juce::uint32> groups;                      // a clip group's saved id, and its id now
    std::vector<std::pair<size_t, bool>> undecided;          // tracks saved before tracks had kinds: index, and whether an instrument heads the chain

    // Before kinds, an instrument sat first in any track's chain: that marks an instrument track.
    const auto hasInstrumentPlugin = [] (const juce::XmlElement& trackXml)
    {
        if (auto* effects = trackXml.getChildByName ("EFFECTS"))
            for (auto* slot : effects->getChildWithTagNameIterator ("SLOT"))
                if (auto* plugin = slot->getChildByName ("PLUGIN"); plugin != nullptr && plugin->getBoolAttribute ("isInstrument"))
                    return true;

        return false;
    };

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

    for (auto* trackXml : root->getChildWithTagNameIterator ("TRACK"))
    {
        auto& track = trackXml->getBoolAttribute ("bus") ? state.addBus() : trackXml->getBoolAttribute ("folder") ? state.addFolder() : state.addTrack();
        track.collapsed = track.isFolder && trackXml->getBoolAttribute ("collapsed");
        track.folderId = (juce::uint32) trackXml->getIntAttribute ("inFolder", -1);   // an index until every track is read
        track.outputBusId = (juce::uint32) trackXml->getIntAttribute ("output", -1);   // likewise

        if (const auto pair = trackXml->getIntAttribute ("deviceOutput", -1); ! track.isFolder && pair >= 0 && pair < SessionTrack::maxDeviceOutputs - 1)
            track.deviceOutput = pair;

        track.fiveOne = ! track.isBus && ! track.isFolder && trackXml->getBoolAttribute ("fiveOne");
        track.sevenOne = ! track.isBus && ! track.isFolder && ! track.fiveOne && trackXml->getBoolAttribute ("sevenOne");
        track.isInstrument = ! track.isBus && ! track.isFolder && trackXml->getBoolAttribute ("instrument");
        track.musical = ! track.isBus && ! track.isFolder && trackXml->getBoolAttribute ("musical");

        if (! track.isBus && ! track.isFolder && ! trackXml->hasAttribute ("instrument"))
            undecided.emplace_back (state.tracks.size() - 1, hasInstrumentPlugin (*trackXml));

        if (! track.isFolder)
            SessionEdits::setSurroundPan (state, track.id, { (float) trackXml->getDoubleAttribute ("surroundX", 0.0),
                                                            (float) trackXml->getDoubleAttribute ("surroundY", 1.0),
                                                            (float) trackXml->getDoubleAttribute ("surroundWidth", 1.0),
                                                            (float) trackXml->getDoubleAttribute ("lfeDb", SurroundPan::lfeOff) });

        if (const auto* duck = trackXml->getChildByName ("DUCKING"))
        {
            track.ducking.keyTrackId = (juce::uint32) duck->getIntAttribute ("key", -1);   // likewise
            track.ducking.amountDb = (float) duck->getDoubleAttribute ("amountDb", -12.0);
            track.ducking.thresholdDb = (float) duck->getDoubleAttribute ("thresholdDb", -40.0);
            track.ducking.attackMs = (float) duck->getDoubleAttribute ("attackMs", 10.0);
            track.ducking.releaseMs = (float) duck->getDoubleAttribute ("releaseMs", 300.0);
        }
        else
        {
            track.ducking.keyTrackId = (juce::uint32) -1;
        }

        for (auto* side : trackXml->getChildWithTagNameIterator ("SIDECHAIN"))   // keys as indices, likewise
            track.sidechains.push_back ({ (juce::uint32) side->getIntAttribute ("slot"), (juce::uint32) side->getIntAttribute ("key", -1) });
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
        track.soloSafe = trackXml->getBoolAttribute ("soloSafe");
        track.invertPolarity = trackXml->getBoolAttribute ("invert");
        track.group = juce::jlimit (0, SessionTrack::maxGroups, trackXml->getIntAttribute ("group"));
        track.colour = juce::jlimit (-1, 7, trackXml->getIntAttribute ("colour", -1));
        track.hidden = trackXml->getBoolAttribute ("hidden");
        track.note = trackXml->getStringAttribute ("note");

        if (const auto channels = trackXml->getIntAttribute ("inputChannels"); ! track.isBus && (channels == 1 || channels == 2))
            track.input = { juce::jmax (0, trackXml->getIntAttribute ("inputFirst")), channels };

        SessionFile::readEnvelopes (state, track.id, *trackXml);

        if (auto* effects = trackXml->getChildByName ("EFFECTS"))
            result.effects[track.id] = std::make_shared<juce::XmlElement> (*effects);

        // Frozen audio that cannot be found leaves the track unfrozen, playing its clips live again.
        if (auto* frozenXml = trackXml->getChildByName ("FROZEN"); frozenXml != nullptr && ! track.isBus)
            if (const auto [source, frozenAudio] = locate (*frozenXml); frozenAudio != nullptr)
            {
                track.frozenFile = source;
                track.frozenAudio = frozenAudio;
            }

        for (auto* clipXml : trackXml->getChildWithTagNameIterator ("CLIP"))
        {
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
            clip.fadeInShape = FadeShapes::fromId (clipXml->getStringAttribute ("fadeInShape"));
            clip.fadeOutShape = FadeShapes::fromId (clipXml->getStringAttribute ("fadeOutShape"));
            clip.locked = clipXml->getBoolAttribute ("locked");
            clip.muted = clipXml->getBoolAttribute ("muted");
            clip.colour = juce::jlimit (-1, (int) TrackColours::palette.size() - 1, clipXml->getIntAttribute ("colour", -1));
            clip.note = clipXml->getStringAttribute ("note");

            // Groups take fresh ids, as clips do, the same for every clip saved in one group.
            if (const auto group = clipXml->getIntAttribute ("group"); group != 0)
            {
                auto& id = groups[group];

                if (id == 0)
                    id = state.nextId++;

                clip.groupId = id;
            }

            if (auto* gainXml = clipXml->getChildByName ("GAIN"))
                for (auto* pointXml : gainXml->getChildWithTagNameIterator ("POINT"))
                    clip.gainEnvelope.points.push_back ({ pointXml->getStringAttribute ("frame").getLargeIntValue(),
                                                         juce::jlimit (SessionClip::minGainDb, SessionClip::maxGainDb,
                                                                       (float) pointXml->getDoubleAttribute ("value")) });

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

        for (auto* clipXml : trackXml->getChildWithTagNameIterator ("NOTECLIP"))
        {
            NoteClip clip;
            clip.name = clipXml->getStringAttribute ("name");
            clip.position = clipXml->getStringAttribute ("position").getLargeIntValue();
            clip.offset = clipXml->getStringAttribute ("offset").getLargeIntValue();
            clip.length = clipXml->getStringAttribute ("length").getLargeIntValue();
            clip.loopLength = clipXml->getStringAttribute ("loop").getLargeIntValue();
            clip.muted = clipXml->getBoolAttribute ("muted");
            clip.colour = clipXml->getIntAttribute ("colour", -1);

            for (auto* noteXml : clipXml->getChildWithTagNameIterator ("NOTE"))
                clip.notes.push_back ({ noteXml->getIntAttribute ("pitch"), noteXml->getStringAttribute ("start").getLargeIntValue(),
                                        noteXml->getStringAttribute ("length").getLargeIntValue(), (float) noteXml->getDoubleAttribute ("velocity") });

            for (auto* laneXml : clipXml->getChildWithTagNameIterator ("LANE"))
            {
                ControllerLane lane;
                lane.controller = laneXml->getIntAttribute ("controller", 1);

                for (auto* pointXml : laneXml->getChildWithTagNameIterator ("POINT"))
                    lane.envelope.points.push_back ({ pointXml->getStringAttribute ("frame").getLargeIntValue(), (float) pointXml->getDoubleAttribute ("value") });

                clip.lanes.push_back (std::move (lane));
            }

            if (! track.isBus)
            {
                clip = SessionEdits::tidiedNoteClip (std::move (clip));
                clip.id = state.nextId++;
                track.noteClips.push_back (std::move (clip));
            }
        }
    }

    if (auto* notesXml = root->getChildByName ("NOTES"))
        state.notes = notesXml->getAllSubText();

    // The relative path first, as for audio; a video not found is kept, for the panel to say so.
    if (auto* videoXml = root->getChildByName ("VIDEO"))
    {
        state.video = file.getParentDirectory().getChildFile (videoXml->getStringAttribute ("file"));

        if (! state.video.existsAsFile() && videoXml->getStringAttribute ("absolute").isNotEmpty())
            state.video = juce::File (videoXml->getStringAttribute ("absolute"));

        state.videoStart = juce::jmax (0.0, videoXml->getDoubleAttribute ("start"));
    }

    for (auto* markerXml : root->getChildWithTagNameIterator ("MARKER"))
    {
        Marker m;
        m.name = markerXml->getStringAttribute ("name");
        m.start = juce::jmax ((juce::int64) 0, markerXml->getStringAttribute ("start").getLargeIntValue());
        m.length = juce::jmax ((juce::int64) 0, markerXml->getStringAttribute ("length").getLargeIntValue());
        m.colour = juce::jlimit (-1, (int) TrackColours::palette.size() - 1, markerXml->getIntAttribute ("colour", -1));
        state.markers.push_back (m);
    }

    MarkerEdits::sort (state.markers);

    for (auto* changeXml : root->getChildWithTagNameIterator ("TEMPO"))
        SessionEdits::setTempoChange (state, { changeXml->getStringAttribute ("bar").getLargeIntValue(),
                                               juce::jlimit (20.0, 400.0, changeXml->getDoubleAttribute ("tempo", 120.0)),
                                               juce::jlimit (1, 16, changeXml->getIntAttribute ("beatsPerBar", 4)) });

    // A track saved before tracks had kinds is an instrument track when an instrument heads its
    // chain, or it has note clips and no audio clips; clips on the wrong kind of track go onto a
    // track of their own after it, once the indices below are resolved.
    for (const auto& [index, instrument] : undecided)
        state.tracks[index].isInstrument = instrument || (state.tracks[index].clips.empty() && ! state.tracks[index].noteClips.empty());

    // Indices to ids; sends and outputs to anything but a bus, and places in anything but a folder, are dropped.
    const auto pending = state.tracks;

    for (size_t i = 0; i < pending.size(); ++i)
    {
        state.tracks[i].sends.clear();
        state.tracks[i].folderId = 0;
        state.tracks[i].outputBusId = 0;
        state.tracks[i].ducking = {};
        state.tracks[i].sidechains.clear();

        if (pending[i].folderId < pending.size())
            SessionEdits::setTrackFolder (state, state.tracks[i].id, pending[pending[i].folderId].id);

        if (pending[i].outputBusId < pending.size())
            SessionEdits::setTrackOutput (state, state.tracks[i].id, pending[pending[i].outputBusId].id);

        for (auto send : pending[i].sends)
        {
            if (send.busId < pending.size())
            {
                send.busId = pending[send.busId].id;
                SessionEdits::setSend (state, state.tracks[i].id, send);
            }
        }
    }

    // Ducking once every track's has been cleared of indices, as the check for loops follows
    // keys through the other tracks; a loop or settings out of range are dropped.
    for (size_t i = 0; i < pending.size(); ++i)
    {
        if (auto ducking = pending[i].ducking; ducking.keyTrackId < pending.size())
        {
            ducking.keyTrackId = pending[ducking.keyTrackId].id;
            SessionEdits::setDucking (state, state.tracks[i].id, ducking);
        }
    }

    // Sidechains likewise, after the ducking, as the check for loops follows both.
    for (size_t i = 0; i < pending.size(); ++i)
        for (auto& chain : pending[i].sidechains)
            if (chain.keyTrackId < pending.size())
                SessionEdits::setSidechain (state, state.tracks[i].id, chain.slotId, pending[chain.keyTrackId].id);

    SessionEdits::separateClipKinds (state);
    return result;
}
}
