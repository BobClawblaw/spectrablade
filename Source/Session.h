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

/** A track: clips (overlapping clips mix), a level, a pan, mute, solo and sends to buses.

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

    const SessionSend* findSend (juce::uint32 busId) const;
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

    bool removeClip (SessionState&, juce::uint32 clipId);
    /** Removes a track or bus; removing a bus removes the sends to it. */
    bool removeTrack (SessionState&, int track);

    /** Sets a track's send to a bus, adding it if there is none. */
    bool setSend (SessionState&, juce::uint32 trackId, const SessionSend&);
    bool removeSend (SessionState&, juce::uint32 trackId, juce::uint32 busId);
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
