#pragma once

#include <JuceHeader.h>
#include "SessionMixer.h"

/** The plug-in effects on each session track, in order, keyed by track id.

    They live outside the session's undoable state: deleting a track keeps its chain, so undoing
    the delete brings the effects back. clear() drops them all for a new session.

    Plug-ins run at the session's rate on two channels. The mixer reads each track ahead by its
    chain's latency, so tracks with and without effects stay in line.

    Each slot has an id that stays with its effect when the chain is reordered and is saved with
    it; envelopes on its parameters name it. Where a track has such envelopes, its chain runs in
    steps of automationStep frames, lined up with session time, and each parameter is set from
    its envelope at the start of each step, at the frame of the audio its effect is working on
    (earlier by the latency of the effects before it).

    Changes are made on the message thread; the mixer calls the SessionMixer::Inserts methods on
    the audio thread. A track whose chain is being changed plays silence for that block. */
class TrackEffects final : public SessionMixer::Inserts
{
public:
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
    void process (const SessionTrack&, juce::int64 frame, juce::AudioBuffer<float>&, int numFrames) override;

private:
    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        juce::uint32 id = 0;
        bool bypassed = false;
        juce::AudioBuffer<float> work;
        std::map<juce::String, juce::AudioProcessorParameter*> parameters;
    };

    using Chain = std::vector<std::unique_ptr<Slot>>;

    void prepareSlot (Slot&) const;
    Chain* find (juce::uint32 trackId);
    const Chain* find (juce::uint32 trackId) const;
    int latencyOf (const Chain&) const;

    juce::CriticalSection lock;
    std::map<juce::uint32, Chain> chains;
    double rate = 48000.0;
    juce::uint32 nextSlotId = 1;
    juce::MidiBuffer midi;
};
