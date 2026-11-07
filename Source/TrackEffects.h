#pragma once

#include <JuceHeader.h>
#include "SessionMixer.h"

/** The plug-in effects on each session track, in order, keyed by track id.

    They live outside the session's undoable state: deleting a track keeps its chain, so undoing
    the delete brings the effects back. clear() drops them all for a new session.

    Plug-ins run at the session's rate on two channels. One with a sidechain input (a second input
    bus) is given the key the session feeds its slot there, or silence. The mixer reads each track ahead by its
    chain's latency, so tracks with and without effects stay in line.

    Each slot has an id that stays with its effect when the chain is reordered and is saved with
    it; envelopes on its parameters name it. Where a track has such envelopes, its chain runs in
    steps of automationStep frames, lined up with session time, and each parameter is set from
    its envelope at the start of each step, at the frame of the audio its effect is working on
    (earlier by the latency of the effects before it).

    Moves of the effects' parameters in their own windows are reported (onParameterTouched), so
    they can be written as automation. While a parameter is touched its envelope leaves it alone.

    On a 5.1 track each effect runs three times, on L R, on C LFE and on Ls Rs: the slot's own
    plug-in on the first pair, and two more of the same plug-in (made by makeCopy, given its state)
    on the others. The copies follow its parameters (set to the same values before each step) and
    are given its whole state again when refreshCopies is called for it, after its window closes;
    their latency is taken to be its. A pair whose copy couldn't be made plays silence. A built-in
    effect that can link channels (BuiltInEffect::processLinked: the compressor and the limiter)
    runs once on all six instead, its gain from the loudest of them.

    Changes are made on the message thread; the mixer calls the SessionMixer::Inserts methods on
    the audio thread. A track whose chain is being changed plays silence for that block. */
class TrackEffects final : public SessionMixer::Inserts,
                           private juce::AsyncUpdater
{
public:
    /** A parameter moved in an effect's window: a gesture (a knob grabbed) beginning or ending,
        or a new value (0 to 1). */
    struct ParameterTouch
    {
        enum class Kind { begin, change, end };
        Kind kind = Kind::change;
        juce::uint32 trackId = 0, slotId = 0;
        juce::String parameterId;
        float value = 0.0f;
    };

    /** Called on the message thread for each touch, in order. Values the effects are given from
        envelopes are not reported. */
    std::function<void (const ParameterTouch&)> onParameterTouched;

    /** While a parameter is touched, its envelope doesn't set it, so what the hand does wins. */
    void setTouched (juce::uint32 trackId, juce::uint32 slotId, const juce::String& parameterId, bool touched);

    /** Makes another instance of a plug-in, for a 5.1 track's copies (null if it can't). */
    std::function<std::unique_ptr<juce::AudioPluginInstance> (const juce::PluginDescription&)> makeCopy;

    /** Makes the copies for these tracks' effects (and for effects added to them later), and drops
        those of tracks no longer in the set. Returns the names of the effects that couldn't be
        copied. */
    juce::StringArray setWideTracks (const std::set<juce::uint32>&) override;

    /** Gives the copies of this plug-in (if it is a slot's on a 5.1 track) its whole state again. */
    void refreshCopies (const juce::AudioProcessor&);

    /** Sets a copy's parameters to the plug-in's (by index), where they differ. */
    static void followParameters (juce::AudioPluginInstance& original, juce::AudioPluginInstance& copy);

    /** How many copies the slot has: 2 on a 5.1 track, else 0. */
    int getNumCopies (juce::uint32 trackId, int slot) const;

    /** The largest block a plug-in is given; longer runs are split. */
    static constexpr int maxBlock = 1024;

    ~TrackEffects() override;

    /** Prepares every plug-in, and the ones added later, for this rate. */
    void prepare (double sampleRate);
    double getSampleRate() const noexcept               { return rate; }

    /** Removes every chain. */
    void clear();

    int getNumSlots (juce::uint32 trackId) const;
    juce::AudioPluginInstance* getPlugin (juce::uint32 trackId, int slot) const;
    bool isBypassed (juce::uint32 trackId, int slot) const;
    std::vector<juce::AudioPluginInstance*> getAllPlugins() const;

    /** Frames between parameter changes from envelopes. */
    static constexpr int automationStep = 64;

    /** Prepares the plug-in and adds it to the end of the track's chain, as slot slotId (a new
        id when 0). Returns the slot's id. */
    juce::uint32 add (juce::uint32 trackId, std::unique_ptr<juce::AudioPluginInstance>, juce::uint32 slotId = 0);

    /** The id of the slot at this place in the track's chain, or 0. */
    juce::uint32 getSlotId (juce::uint32 trackId, int slot) const;

    /** The plug-in in the slot with this id on the track, or null. */
    juce::AudioPluginInstance* findSlot (juce::uint32 trackId, juce::uint32 slotId) const;

    /** A plug-in's parameters by ID: the plug-in format's own ID where it has one, otherwise the
        parameter's index. */
    static std::map<juce::String, juce::AudioProcessorParameter*> parametersById (juce::AudioPluginInstance&);

    /** Takes the plug-in out of the chain and hands it back (to be closed and deleted). */
    std::unique_ptr<juce::AudioPluginInstance> remove (juce::uint32 trackId, int slot);
    void setBypassed (juce::uint32 trackId, int slot, bool);
    void move (juce::uint32 trackId, int from, int to);

    /** Frames by which the track's chain delays it: the sum over the plug-ins not bypassed. */
    int getChainLatency (juce::uint32 trackId) const;

    /** The track's chain as a rack-style EFFECTS element (each plug-in's description, state,
        bypass and slot id), or null when it has none. */
    std::unique_ptr<juce::XmlElement> createXml (juce::uint32 trackId) const;

    // SessionMixer::Inserts, for the audio thread
    bool isActive (juce::uint32 trackId) override;
    int getLatency (juce::uint32 trackId) override;
    void reset (juce::uint32 trackId) override;
    int getLatencyBefore (juce::uint32 trackId, juce::uint32 slotId) override;
    void process (const SessionTrack&, juce::int64 frame, juce::AudioBuffer<float>&, int numFrames,
                  const std::vector<SessionMixer::KeyAudio>& keys) override;

    /** True when the plug-in has a sidechain input: a second input bus, turned on. */
    static bool takesSidechain (const juce::AudioPluginInstance&);

    /** True when the plug-in in this slot of the track has a sidechain input. */
    bool hasSidechain (juce::uint32 trackId, int slot) const;

private:
    /** Listens to one slot's parameters for touches. */
    struct Watcher final : public juce::AudioProcessorParameter::Listener
    {
        Watcher (TrackEffects&, juce::uint32 trackId, juce::uint32 slotId, const std::map<juce::String, juce::AudioProcessorParameter*>&);
        ~Watcher() override;

        void parameterValueChanged (int index, float value) override;
        void parameterGestureChanged (int index, bool starting) override;
        void report (ParameterTouch::Kind, int index, float value);

        TrackEffects& owner;
        juce::uint32 trackId, slotId;
        std::map<int, std::pair<juce::AudioProcessorParameter*, juce::String>> byIndex;
    };

    using Copies = std::array<std::unique_ptr<juce::AudioPluginInstance>, 2>;

    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        Copies copies;                         // on a 5.1 track: for C LFE and Ls Rs
        juce::uint32 id = 0;
        bool bypassed = false;
        juce::AudioBuffer<float> work;
        std::map<juce::String, juce::AudioProcessorParameter*> parameters;
        std::unique_ptr<Watcher> watcher;      // after the plug-in, so it goes first
    };

    void handleAsyncUpdate() override;


    using Chain = std::vector<std::unique_ptr<Slot>>;

    void prepareSlot (Slot&) const;
    void prepareInstance (juce::AudioPluginInstance&) const;
    Copies copiesOf (Slot&) const;
    void install (juce::uint32 trackId, Slot*, Copies);
    Chain* find (juce::uint32 trackId);
    const Chain* find (juce::uint32 trackId) const;
    int latencyOf (const Chain&) const;

    juce::CriticalSection lock;
    std::map<juce::uint32, Chain> chains;
    double rate = 48000.0;
    juce::uint32 nextSlotId = 1;
    std::set<std::tuple<juce::uint32, juce::uint32, juce::String>> touched;   // guarded by lock
    std::set<juce::uint32> wide;   // 5.1 tracks; guarded by lock

    juce::SpinLock touchesLock;
    std::vector<ParameterTouch> touches;   // waiting for the message thread
    juce::MidiBuffer midi;
};
