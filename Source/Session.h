#pragma once

#include "AudioDocument.h"
#include "Metronome.h"

//==============================================================================
/** A clip: a stretch of an audio file placed on a track. The audio is shared, never copied. */
/** The curve a fade follows from silence to full level. */
enum class FadeShape
{
    sCurve,        // raised cosine: gentle at both ends
    linear,
    equalPower,    // a quarter sine: two of these crossfading keep uncorrelated sound's power steady
    slow,          // squared: starts slowly
    fast           // rises quickly, then levels off
};

namespace FadeShapes
{
    constexpr std::array<FadeShape, 5> all { FadeShape::sCurve, FadeShape::linear, FadeShape::equalPower, FadeShape::slow, FadeShape::fast };

    /** The level `frame` frames into a fade `length` frames long (1 from its end on). */
    float level (FadeShape, juce::int64 frame, juce::int64 length);

    /** "S-Curve", "Linear", ... as menus show them, and the names session files use. */
    juce::String displayName (FadeShape);
    juce::String id (FadeShape);
    FadeShape fromId (const juce::String&);   // S-curve for one it doesn't know
}

struct SessionClip
{
    juce::uint32 id = 0;
    juce::String name;
    juce::File source;                     // the file the audio came from
    AudioDocument::Snapshot audio;
    juce::int64 position = 0;              // session frame where the clip starts
    juce::int64 offset = 0;                // first frame of the audio it plays
    juce::int64 length = 0;                // frames it plays
    float gainDb = 0.0f;                   // from minGainDb to maxGainDb
    juce::int64 fadeIn = 0, fadeOut = 0;   // frames
    FadeShape fadeInShape = FadeShape::sCurve, fadeOutShape = FadeShape::sCurve;
    bool locked = false;                   // the edits below leave it where it is, as it is
    bool muted = false;                    // stays in place but isn't heard (in playback, mixdowns, bounces and stems)
    int colour = -1;                       // a place in TrackColours::palette, or -1 for its track's colour
    juce::String note;                     // free text, shown along the clip's bottom edge

    static constexpr float minGainDb = -60.0f, maxGainDb = 24.0f;

    /** Other recordings of the same stretch (loop recording's passes), one of them in use. A
        take's audio is read `shift` frames on from where take 0's would be, so trims and splits
        apply to every take at once. With takes, source, audio and offset are the active take's. */
    struct Take
    {
        juce::File source;
        AudioDocument::Snapshot audio;
        juce::int64 shift = 0;
    };

    std::vector<Take> takes;
    int activeTake = 0;

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
/** The device inputs a track records from: a first channel (absolute) and 1 or 2 channels, or
    the recording input chosen in the window (first < 0). */
struct TrackInput
{
    int first = -1, channels = 0;

    bool isDefault() const noexcept                       { return first < 0 || channels <= 0; }
    bool operator== (const TrackInput& o) const noexcept  { return isDefault() ? o.isDefault() : first == o.first && channels == o.channels; }
    bool operator!= (const TrackInput& o) const noexcept  { return ! operator== (o); }
};

struct SessionTrack
{
    juce::uint32 id = 0;
    juce::String name;
    bool isBus = false;
    float gainDb = 0.0f;
    float pan = 0.0f;                      // -1 (left) to +1 (right)
    bool mute = false, solo = false;
    int group = 0;                         // 0 for none, or 1 to maxGroups: volume, mute and solo changes follow through it
    int colour = -1;                       // a place in TrackColours::palette, or -1 for one by its place in the list
    bool hidden = false;                   // left out of the lanes (it still plays and shows in the mixer)
    juce::String note;                     // free text, shown in the header's tooltip
    TrackInput input;                      // what it records from (tracks only)

    static constexpr int maxGroups = 8;
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
    double tempo = 120.0;                  // beats a minute, for the metronome, from the start
    int beatsPerBar = 4;
    std::vector<Metronome::TempoChange> tempoChanges;   // at later bars, sorted by bar, one a bar
    std::vector<SessionTrack> tracks;
    Markers markers;                       // points and regions on the session's time, kept sorted
    juce::uint32 nextId = 1;

    /** The end of the last clip. */
    juce::int64 getLength() const;

    /** The beats of the tempo and its changes. It refers to tempoChanges, so it lasts only as
        long as this state, unchanged. */
    Metronome::TempoMap tempoMap() const noexcept { return { tempo, beatsPerBar, tempoChanges, sampleRate }; }
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
/** A stretch of a session's time copied from every track, for pasting as inserted time. Frames
    count from the stretch's start. */
struct SessionTime
{
    struct Track
    {
        std::vector<SessionClip> clips;            // cut to the stretch, unlocked
        Envelope volume, pan;                      // empty where the track had none
        std::vector<EffectEnvelope> effects;
    };

    juce::int64 length = 0;
    double sampleRate = 0.0;
    std::vector<Track> tracks;                     // in the session's order, buses included
    Markers markers;
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

    /** Sets a clip's gain, limited to SessionClip::minGainDb to maxGainDb. */
    bool setClipGain (SessionState&, juce::uint32 clipId, float gainDb);

    /** Sets a clip's gain so that the loudest sample of the audio it plays reaches peakDb (within
        the gain's limits). False, changing nothing, when that audio is silent. */
    bool normalizeClip (SessionState&, juce::uint32 clipId, float peakDb);

    /** Sets a clip's fade lengths in frames. The fade in is limited to the clip's length and the
        fade out to what the fade in leaves. */
    bool setClipFades (SessionState&, juce::uint32 clipId, juce::int64 fadeIn, juce::int64 fadeOut);

    /** Sets the shape of a clip's fade in (or out). False for a locked clip or when it is so. */
    bool setClipFadeShape (SessionState&, juce::uint32 clipId, bool fadeOut, FadeShape);

    bool removeClip (SessionState&, juce::uint32 clipId);

    /** Locks or unlocks a clip. Moving, trimming, splitting, fading, changing the gain, take or
        audio of a locked clip and removing it are refused (false); other clips placed or recorded
        over it, and crossfades with it, leave it as it is, so they overlap it. Removing its track
        still removes it. Copies of a locked clip start unlocked. */
    bool setClipLocked (SessionState&, juce::uint32 clipId, bool locked);

    /** The session frame nearest to `frame`, at most `within` frames away and inside the clip's
        audio, where its channels' sum crosses zero: the sum changes sign between the frame before
        and it, or is 0 there. `frame` itself when there is none. */
    juce::int64 zeroCrossingNear (const SessionClip&, juce::int64 frame, juce::int64 within);

    /** Mutes or unmutes a clip, locked or not. False when it is already so. */
    bool setClipMuted (SessionState&, juce::uint32 clipId, bool muted);

    /** Gives a clip, locked or not, a colour of its own (a place in TrackColours::palette), or its
        track's with -1 or any value outside the palette. False when it already has that one. */
    bool setClipColour (SessionState&, juce::uint32 clipId, int colour);

    /** Puts take `index` of a clip in use. It keeps its place and length where the take's audio
        allows; otherwise it is cut back to what the take has. */
    bool chooseTake (SessionState&, juce::uint32 clipId, int index);

    /** Crossfades a clip with the clips it partly overlaps on its track: where another clip ends
        inside it, its fade in and the other's fade out both span the overlap; where another starts
        inside it, its fade out and the other's fade in do. Clips inside another, or holding one,
        are left alone. False when it overlaps nothing that way. */
    bool crossfade (SessionState&, juce::uint32 clipId);

    /** Places a copy of a clip (sharing its audio and takes) on a track at position, over what
        was there, which is cleared as clearRange does. Onto a bus or past the last track, it goes
        onto a new track. Returns the copy's id. */
    juce::uint32 placeClip (SessionState&, int track, const SessionClip&, juce::int64 position);

    /** Fills from a clip's end up to `until` with copies of it laid end to end on its track (over
        what is there), the last cut short at `until`. Returns how many copies were placed. */
    int repeatClip (SessionState&, juce::uint32 clipId, juce::int64 until);

    /** The part of a clip's audio it plays, back to front. */
    AudioDocument::Snapshot reversedAudio (const SessionClip&);

    /** Makes a clip play reversed audio (from reversedAudio(), saved as file): it plays all of it
        from its start, its fades change ends, and other takes go. */
    bool reverseClip (SessionState&, juce::uint32 clipId, const juce::File&, AudioDocument::Snapshot reversed);

    /** The part of the clip's audio that plays, stretched to `ratio` times its length (within
        TimeStretch::shortest to longest) at the same pitch. Null with no audio, or when
        progress (as TimeStretch::stretch has it) cancels. */
    AudioDocument::Snapshot stretchedAudio (const SessionClip&, double ratio, double sampleRate,
                                            const std::function<bool (float)>& progress = {});

    /** The part of the clip's audio that plays, `semitones` higher or lower and just as long.
        Null with no audio, or when progress cancels. */
    AudioDocument::Snapshot pitchedAudio (const SessionClip&, double semitones, double sampleRate,
                                          const std::function<bool (float)>& progress = {});

    /** Makes a clip play processed audio of the part it played (from stretchedAudio() or
        pitchedAudio(), saved as file), all of it, from where it starts; its fades scale with
        its length, and its takes go. False for a locked clip. */
    bool stretchClip (SessionState&, juce::uint32 clipId, const juce::File&, AudioDocument::Snapshot stretched);

    /** Clears [start, end) on a track: clips inside it go, clips across its edges are cut back
        (or split in two) and lose their fade on the cut side. False when nothing was there. */
    bool clearRange (SessionState&, int track, juce::int64 start, juce::int64 end);

    /** Places a recorded take at position on a track (a new one when track is past the last or a
        bus), replacing what the track had there. Returns the clip's id. */
    juce::uint32 recordClip (SessionState&, int track, const juce::File& source, AudioDocument::Snapshot audio, juce::int64 position);

    /** Places the passes of a loop recording: one clip at position, length frames long, with a
        take for each pass, starting at each of offsets in the recording, the last in use. It
        replaces what the track had there. Returns the clip's id (0 with no passes). */
    juce::uint32 recordTakes (SessionState&, int track, const juce::File& source, AudioDocument::Snapshot audio,
                              juce::int64 position, juce::int64 length, const std::vector<juce::int64>& offsets);
    /** After an edit of a track in a group (before is the track as it was), the others in its
        group follow: a volume change moves theirs by as many dB (within -60 to +12, and not for a
        volume on an envelope), and mute and solo take its state. */
    void followGroup (SessionState&, const SessionTrack& before);

    /** Moves a track or bus from one place in the list to another, the others closing up behind
        it. False when either place is out of range or they are the same. */
    bool moveTrack (SessionState&, int from, int to);

    /** Removes a track or bus; removing a bus removes the sends to it. */
    bool removeTrack (SessionState&, int track);

    /** Puts a copy of a track or bus right after it, named "Name Copy": its clips (sharing their
        audio, as unlocked copies), sends, envelopes, volume, pan, mute, solo, group, colour and
        input. Its effects are not in the state; copy them by the new id. Returns the copy's id,
        or 0 when there is no such track. */
    juce::uint32 duplicateTrack (SessionState&, int track);

    /** Moves each clip on a track that starts after the end of the clips before it back to that
        end, from the first clip on, so they play one after another. A clip overlapping the ones
        before it moves with them, keeping the overlap. Locked clips stay where they are, and the
        ones after them close up to them. Returns how many clips moved. */
    int closeGaps (SessionState&, int track);

    /** Moves each of these clips, on its own track, so that it starts on the grid line nearest
        its start (nearestLine gives it). Locked clips stay. Returns how many moved. */
    int quantizeClips (SessionState&, const std::vector<juce::uint32>& clipIds, const std::function<juce::int64 (juce::int64)>& nearestLine);

    /** Clears [start, end) on every track, hidden ones too, as clearRange does. False when
        nothing was there. */
    bool clearTime (SessionState&, juce::int64 start, juce::int64 end);

    /** Takes [start, end) out of the session's time. On every track, hidden ones too, it is
        cleared as clearRange does, and the clips, envelope points and markers after it move back
        by its length; envelopes keep their shape on both sides, and markers follow as
        MarkerEdits::afterReplace has them. Locked clips stay as they are, where they are. False
        when nothing changed. */
    bool deleteTime (SessionState&, juce::int64 start, juce::int64 end);

    /** Puts `length` frames of empty time into the session at `at`. On every track, hidden ones
        too, clips across it are split there, and the clips, envelope points and markers from it on
        move later by `length`; envelopes hold their value at `at` over the new time. Locked clips
        stay as they are, where they are. False when nothing changed. */
    bool insertTime (SessionState&, juce::int64 at, juce::int64 length);

    /** Puts a tempo change at its bar (after the first; the first bar's tempo is the session's),
        replacing one there. Tempo 20 to 400, 1 to 16 beats in a bar; false outside those or when
        it is already so. Clips and markers stay where they are in time. */
    bool setTempoChange (SessionState&, Metronome::TempoChange);
    bool removeTempoChange (SessionState&, juce::int64 bar);

    /** Copies [start, end) from every track: the clips across it cut to it (losing their fades
        on the cut sides), each envelope's line across it, and the markers in it as
        MarkerEdits::afterCrop has them. Empty (length 0) when the range is. */
    SessionTime copyTime (const SessionState&, juce::int64 start, juce::int64 end);

    /** Puts copied time in at `at`: insertTime makes room, then each track's clips go onto the
        track at the same place in the list (as unlocked copies with new ids; tracks past the last
        are left out), its envelope lines replace the held values there, and its markers are
        added. A volume or pan line pasted onto a track without an envelope gives it one that
        stays at the track's setting outside the pasted time; an effect parameter's line is pasted
        only where the track already has that parameter's envelope. Where the copy had no line,
        the envelope holds its value, as insertTime does. False when the time is empty or at
        another sample rate. */
    bool pasteTime (SessionState&, const SessionTime&, juce::int64 at);

    /** Sets a track's send to a bus, adding it if there is none. */
    bool setSend (SessionState&, juce::uint32 trackId, const SessionSend&);
    bool removeSend (SessionState&, juce::uint32 trackId, juce::uint32 busId);

    /** Replaces a track's volume or pan envelope; its points are kept in range and in time
        order. An empty envelope hands the parameter back to the track's setting. */
    bool setEnvelope (SessionState&, juce::uint32 trackId, const AutomationTarget&, Envelope);

    /** Writes a pass of automation recorded while playing: points in time order. From the pass's
        first frame to returnFrames after its last, the envelope's old points give way to the
        pass, which starts from the old value there and ends by going back, over returnFrames,
        to the old value. Outside that, the envelope (or, with none, the track's setting, or for
        an effect parameter the value the pass starts from) stays as it was. */
    bool writeAutomation (SessionState&, juce::uint32 trackId, const AutomationTarget&,
                          const std::vector<AutomationPoint>& pass, juce::int64 returnFrames);
}

//==============================================================================
/** Snapping: edits that land near something line up with it exactly. */
namespace SessionSnap
{
    /** What a dragged edge can snap to: the session start, the extra frames given (the cursor,
        a time range's edges), every clip's start and end but the ignored clip's, and a grid: with
        a beat grid, its lines on the session's tempo map (the metronome's beats), else with a grid
        step, every multiple of it. */
    struct Targets
    {
        std::vector<juce::int64> extra;
        juce::uint32 ignoreClip = 0;
        std::vector<juce::uint32> ignoreClips;   // more clips being dragged
        int ignoreMarker = -1;             // the index of a marker being dragged
        juce::int64 gridStep = 0;
        std::optional<Metronome::Grid> beatGrid;
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

    /** Every step's description, oldest first: the first is where the session started (no
        description), and getPosition() is the one it is at. Steps after it can be redone. */
    juce::StringArray getHistory() const;
    int getPosition() const noexcept              { return position; }

    /** Goes back or forward to a step in getHistory(), as that many undos or redos would. */
    void goTo (int step);

    bool isDirty() const noexcept                 { return position != savedPosition || effectsChanged; }
    void markSaved (const juce::File& savedAs);

    /** Marks the session as having changes to save (a recovered copy, say), at every step. */
    void markUnsaved();

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
