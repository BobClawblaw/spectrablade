#pragma once

#include <JuceHeader.h>
#include "SessionMixer.h"

/** The plug-in effects on each session track, in order, keyed by track id.

    They live outside the session's undoable state: deleting a track keeps its chain, so undoing
    the delete brings the effects back. clear() drops them all for a new session.

    Plug-ins run at the session's rate on two channels. The mixer reads each track ahead by its
    chain's latency, so tracks with and without effects stay in line.

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

    /** Prepares the plug-in and adds it to the end of the track's chain. */
    void add (juce::uint32 trackId, std::unique_ptr<juce::AudioPluginInstance>);

    /** Takes the plug-in out of the chain and hands it back (to be closed and deleted). */
    std::unique_ptr<juce::AudioPluginInstance> remove (juce::uint32 trackId, int slot);
    void setBypassed (juce::uint32 trackId, int slot, bool);
    void move (juce::uint32 trackId, int from, int to);

    /** Frames by which the track's chain delays it: the sum over the plug-ins not bypassed. */
    int getChainLatency (juce::uint32 trackId) const;

    /** The track's chain as a rack-style EFFECTS element (each plug-in's description, state and
        bypass), or null when it has none. */
    std::unique_ptr<juce::XmlElement> createXml (juce::uint32 trackId) const;

    // SessionMixer::Inserts, for the audio thread
    bool isActive (juce::uint32 trackId) override;
    int getLatency (juce::uint32 trackId) override;
    void reset (juce::uint32 trackId) override;
    void process (juce::uint32 trackId, juce::AudioBuffer<float>&, int numFrames) override;

private:
    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        bool bypassed = false;
        juce::AudioBuffer<float> work;
    };

    using Chain = std::vector<std::unique_ptr<Slot>>;

    void prepareSlot (Slot&) const;
    Chain* find (juce::uint32 trackId);
    const Chain* find (juce::uint32 trackId) const;
    int latencyOf (const Chain&) const;

    juce::CriticalSection lock;
    std::map<juce::uint32, Chain> chains;
    double rate = 48000.0;
    juce::MidiBuffer midi;
};
