#pragma once

#include "AudioDocument.h"

//==============================================================================
/** A clip: a stretch of an audio file placed on a track. The audio is shared, never copied. */
struct SessionClip
{
    juce::uint32 id = 0;
    juce::String name;
    juce::File source;                     // the file the audio came from
    AudioDocument::Snapshot audio;
    juce::int64 position = 0;              // session frame where the clip starts
    juce::int64 offset = 0;                // first frame of the audio it plays
    juce::int64 length = 0;                // frames it plays
    float gainDb = 0.0f;
    juce::int64 fadeIn = 0, fadeOut = 0;   // frames, raised-cosine

    juce::int64 getEnd() const noexcept    { return position + length; }
};

/** Part of a track's signal sent to a bus, from before its volume (pre-fader) or after it. */
struct SessionSend
{
    juce::uint32 busId = 0;
    float gainDb = 0.0f;
    bool preFader = false;
};

/** A point on an envelope: the value at a session frame. */
struct AutomationPoint
{
    juce::int64 frame = 0;
    float value = 0.0f;

    bool operator== (const AutomationPoint& o) const noexcept { return frame == o.frame && juce::exactlyEqual (value, o.value); }
};

/** How a track's volume or pan changes over time: points in time order, joined by straight
    lines. Before the first point the value is the first point's, after the last the last's.
    An envelope with no points leaves the track's own setting in charge. */
struct Envelope
{
    std::vector<AutomationPoint> points;

    bool isEmpty() const noexcept                 { return points.empty(); }
    float valueAt (juce::int64 frame) const;

    /** The values at frames start to start + n - 1; the same as valueAt at each, however the
        frames are split into blocks. */
    void fill (juce::int64 start, float* out, int n) const;

    /** Adds a point (after any others at the same frame, not before 0); returns its index. */
    int add (juce::int64 frame, float value);

    /** Moves a point, keeping it between its neighbours in time and not before 0. Returns its index. */
    int move (int index, juce::int64 frame, float value);

    void remove (int index);

    bool operator== (const Envelope& o) const noexcept { return points == o.points; }
};

/** What an envelope automates: a track's volume or pan, or a parameter of one of its effects. */
enum class AutomationParameter { volume, pan, effect };

/** An envelope's target. For an effect parameter: the effect's slot (its TrackEffects slot id,
    which stays with the effect when the chain is reordered) and the parameter's ID. */
struct AutomationTarget
{
    AutomationParameter parameter = AutomationParameter::volume;
    juce::uint32 slotId = 0;
    juce::String parameterId;

    AutomationTarget (AutomationParameter p = AutomationParameter::volume) : parameter (p) {}   // implicit, so volume and pan convert
    static AutomationTarget effect (juce::uint32 slot, const juce::String& id)
    {
        AutomationTarget t (AutomationParameter::effect);
        t.slotId = slot;
        t.parameterId = id;
        return t;
    }

    bool operator== (const AutomationTarget& o) const noexcept
    {
        return parameter == o.parameter && slotId == o.slotId && parameterId == o.parameterId;
    }
};

/** The values an envelope's points can take: volume in dB (the track fader's range), pan from
    -1 (left) to +1 (right), an effect parameter from 0 to 1 (its normalised range). */
juce::Range<float> automationRange (AutomationParameter);

/** An envelope on a parameter of one of a track's effects. */
struct EffectEnvelope
{
    juce::uint32 slotId = 0;
    juce::String parameterId;
    Envelope envelope;                     // 0 to 1

    bool operator== (const EffectEnvelope& o) const noexcept
    {
        return slotId == o.slotId && parameterId == o.parameterId && envelope == o.envelope;
    }
};

/** A track: clips (overlapping clips mix), a level, a pan, mute, solo and sends to buses.
    Volume and pan can each follow an envelope instead of their setting, and so can its
    effects' parameters. An envelope for an effect that has left the chain is kept but unused.

    A bus is a track with no clips that plays what other tracks send it, through its own effects,
    volume and pan. Buses send nowhere themselves. */
struct SessionTrack
{
    juce::uint32 id = 0;
    juce::String name;
    bool isBus = false;
    float gainDb = 0.0f;
    float pan = 0.0f;                      // -1 (left) to +1 (right)
    bool mute = false, solo = false;
    std::vector<SessionClip> clips;
    std::vector<SessionSend> sends;
    Envelope volumeEnvelope, panEnvelope;  // dB, and -1 to +1
    std::vector<EffectEnvelope> effectEnvelopes;   // only ones with points

    const SessionSend* findSend (juce::uint32 busId) const;

    /** The target's envelope; empty for an effect parameter with none. */
    const Envelope& envelope (const AutomationTarget&) const;

    /** The volume (dB) or pan at a session frame: the envelope's value, or the setting when
        there is no envelope. An effect parameter with no envelope has no setting here (its
        effect holds it) and gives 0. */
    float valueAt (const AutomationTarget&, juce::int64 frame) const;

    /** True when anything on the track is automated. */
    bool isAutomated() const noexcept
    {
        return ! volumeEnvelope.isEmpty() || ! panEnvelope.isEmpty() || ! effectEnvelopes.empty();
    }
};

/** One state of a session. Copies share the clips' audio. */
struct SessionState
{
    double sampleRate = 48000.0;
    std::vector<SessionTrack> tracks;
    juce::uint32 nextId = 1;

    /** The end of the last clip. */
    juce::int64 getLength() const;
    bool anySolo() const;

    /** Adds an empty track named "Track n" and returns it. */
    SessionTrack& addTrack();

    /** Adds a bus named "Bus n" and returns it. */
    SessionTrack& addBus();

    /** The track (or bus) with this id, or null. */
    const SessionTrack* findTrack (juce::uint32 trackId) const;

    /** The track and index of the clip with this id, or nothing. */
    std::optional<std::pair<int, int>> findClip (juce::uint32 clipId) const;
};

//==============================================================================
/** Edits on a session state. Each returns false (and leaves the state alone) when it cannot apply. */
namespace SessionEdits
{
    /** Adds a clip of the whole audio at position on a track (a new one when track is past the
        last or a bus). Returns the clip's id. */
    juce::uint32 addClip (SessionState&, int track, const juce::File& source, AudioDocument::Snapshot audio, juce::int64 position);

    /** Moves a clip to a track (a new one when past the last) and a position (not before 0).
        Onto a bus, it only moves in time. */
    bool moveClip (SessionState&, juce::uint32 clipId, int toTrack, juce::int64 position);

    /** Sets where a clip starts and ends in the session, showing more or less of its audio, which
        stays where it is in time. Limited to the audio there is; at least one frame long. */
    bool trimClip (SessionState&, juce::uint32 clipId, juce::int64 start, juce::int64 end);

    /** Splits a clip in two at a session frame strictly inside it. The first part keeps the fade
        in, the second the fade out. */
    bool splitClip (SessionState&, juce::uint32 clipId, juce::int64 frame);

    /** Sets a clip's fade lengths in frames. The fade in is limited to the clip's length and the
        fade out to what the fade in leaves. */
    bool setClipFades (SessionState&, juce::uint32 clipId, juce::int64 fadeIn, juce::int64 fadeOut);

    bool removeClip (SessionState&, juce::uint32 clipId);

    /** Crossfades a clip with the clips it partly overlaps on its track: where another clip ends
        inside it, its fade in and the other's fade out both span the overlap; where another starts
        inside it, its fade out and the other's fade in do. Clips inside another, or holding one,
        are left alone. False when it overlaps nothing that way. */
    bool crossfade (SessionState&, juce::uint32 clipId);

    /** Clears [start, end) on a track: clips inside it go, clips across its edges are cut back
        (or split in two) and lose their fade on the cut side. False when nothing was there. */
    bool clearRange (SessionState&, int track, juce::int64 start, juce::int64 end);

    /** Places a recorded take at position on a track (a new one when track is past the last or a
        bus), replacing what the track had there. Returns the clip's id. */
    juce::uint32 recordClip (SessionState&, int track, const juce::File& source, AudioDocument::Snapshot audio, juce::int64 position);
    /** Removes a track or bus; removing a bus removes the sends to it. */
    bool removeTrack (SessionState&, int track);

    /** Sets a track's send to a bus, adding it if there is none. */
    bool setSend (SessionState&, juce::uint32 trackId, const SessionSend&);
    bool removeSend (SessionState&, juce::uint32 trackId, juce::uint32 busId);

    /** Replaces a track's volume or pan envelope; its points are kept in range and in time
        order. An empty envelope hands the parameter back to the track's setting. */
    bool setEnvelope (SessionState&, juce::uint32 trackId, const AutomationTarget&, Envelope);

    /** Writes a pass of automation recorded while playing: points in time order. From the pass's
        first frame to returnFrames after its last, the envelope's old points give way to the
        pass, which starts from the old value there and ends by going back, over returnFrames,
        to the old value. Outside that, the envelope (or, with none, the track's setting) stays
        as it was. */
    bool writeAutomation (SessionState&, juce::uint32 trackId, const AutomationTarget&,
                          const std::vector<AutomationPoint>& pass, juce::int64 returnFrames);
}

//==============================================================================
/** Snapping: edits that land near something line up with it exactly. */
namespace SessionSnap
{
    /** What a dragged edge can snap to: the session start, the extra frames given (the cursor,
        a time range's edges), every clip's start and end but the ignored clip's, and with a grid
        step, every multiple of it. */
    struct Targets
    {
        std::vector<juce::int64> extra;
        juce::uint32 ignoreClip = 0;
        juce::int64 gridStep = 0;
    };

    /** The target nearest frame, when one is within tolerance frames; otherwise frame. */
    juce::int64 snap (const SessionState&, juce::int64 frame, juce::int64 tolerance, const Targets&);

    /** For a span being moved: how far to shift it so that its start or its end (whichever is
        nearer to a target) lands on one; 0 when neither is within tolerance. */
    juce::int64 snapSpan (const SessionState&, juce::Range<juce::int64> span, juce::int64 tolerance, const Targets&);
}

//==============================================================================
/** The open multitrack session: current state, undo history and dirty state, the multitrack
    counterpart of AudioDocument. Edit methods must be called on the message thread; getState()
    may be called from any thread. */
class Session final : public juce::ChangeBroadcaster
{
public:
    Session();

    using State = std::shared_ptr<const SessionState>;

    /** Replaces everything (a new or loaded session); clears undo history. */
    void reset (SessionState initial, const juce::File& file);

    State getState() const;
    juce::uint32 getVersion() const noexcept      { return version.load(); }
    double getSampleRate() const noexcept         { return sampleRate.load(); }
    juce::File getFile() const                    { return file; }

    /** Applies a new state as one undoable step. With merge, a step with the same description
        as the last one replaces it instead (a slider being dragged, say). */
    void apply (SessionState next, const juce::String& description, bool merge = false);

    bool canUndo() const noexcept                 { return position > 0; }
    bool canRedo() const noexcept                 { return position + 1 < (int) history.size(); }
    juce::String getUndoDescription() const;
    juce::String getRedoDescription() const;
    void undo();
    void redo();

    bool isDirty() const noexcept                 { return position != savedPosition || effectsChanged; }
    void markSaved (const juce::File& savedAs);

    /** Track effects live outside the undoable state; changing them still needs saving. */
    void markEffectsChanged();

    /** Frees states dropped from history once the audio thread has let go of them. */
    void releaseUnusedStates();

private:
    struct Entry
    {
        State state;
        juce::String description;
    };

    void setCurrent (const State&);

    mutable juce::SpinLock lock;
    State current;
    std::atomic<juce::uint32> version { 0 };
    std::atomic<double> sampleRate { 48000.0 };

    std::vector<Entry> history;
    int position = 0;
    int savedPosition = 0;
    bool effectsChanged = false;
    std::vector<State> pendingRelease;
    juce::File file;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Session)
};

//==============================================================================
/** Session files (.sbsession): XML naming each clip's audio file, relative to the session
    file where it can. */
namespace SessionFile
{
    /** trackEffects gives each track's EFFECTS element (or null), stored inside its TRACK. */
    juce::String save (const SessionState&, const juce::File& target,
                       const std::function<std::unique_ptr<juce::XmlElement> (juce::uint32 trackId)>& trackEffects = {});

    struct Loaded
    {
        SessionState state;
        juce::String error;
        juce::StringArray missing;   // audio files that could not be read; their clips are left out
        std::map<juce::uint32, std::shared_ptr<juce::XmlElement>> effects;   // EFFECTS elements, by new track id
    };

    /** Each audio file is read once, however many clips use it. Files at another sample rate
        are converted to the session's. */
    Loaded load (const juce::File&, juce::AudioFormatManager&);
}
