#pragma once

#include <JuceHeader.h>
#include "AudioDocument.h"
#include "Recorder.h"
#include "Session.h"
#include "TrackEffects.h"

//==============================================================================
/** Reads the document's current snapshot for playback. Picks up edits on the fly. */
class DocumentSource final : public juce::PositionableAudioSource
{
public:
    explicit DocumentSource (AudioDocument& doc) : document (doc) {}

    /** Frames from this one onwards play as silence (-1 = play to the end). */
    void setStopFrame (juce::int64 frame)                     { stopFrame = frame; }

    void prepareToPlay (int, double) override                 {}
    void releaseResources() override                          {}
    void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;

    void setNextReadPosition (juce::int64 frame) override    { position = frame; }
    juce::int64 getNextReadPosition() const override         { return position; }
    juce::int64 getTotalLength() const override;
    bool isLooping() const override                           { return false; }

private:
    AudioDocument& document;
    AudioDocument::Snapshot snapshot;      // audio thread's own reference
    juce::uint32 snapshotVersion = 0;
    std::atomic<juce::int64> position { 0 }, stopFrame { -1 };
};

//==============================================================================
/** Mixes the session for playback, through each track's effects. Picks up edits on the fly. */
class SessionSource final : public juce::PositionableAudioSource
{
public:
    SessionSource (Session& s, SessionMixer::Inserts& i) : session (s), inserts (i) {}

    /** Frames from this one onwards play as silence (-1 = play to the end). */
    void setStopFrame (juce::int64 frame)                     { stopFrame = frame; }

    void prepareToPlay (int blockSize, double) override;
    void releaseResources() override                          {}
    void getNextAudioBlock (const juce::AudioSourceChannelInfo&) override;

    void setNextReadPosition (juce::int64 frame) override    { position = frame; }
    juce::int64 getNextReadPosition() const override         { return position; }
    /** The session's length, or the stop frame when that is later (recording past the end). */
    juce::int64 getTotalLength() const override              { return juce::jmax (session.getState()->getLength(), stopFrame.load()); }
    bool isLooping() const override                           { return false; }

private:
    Session& session;
    SessionMixer::Inserts& inserts;
    Session::State state;                  // audio thread's own reference
    juce::uint32 stateVersion = 0;
    SessionMixer::Buffers buffers;
    juce::int64 nextFrame = -1;            // where the last block ended; anything else is a jump
    std::atomic<juce::int64> position { 0 }, stopFrame { -1 };
};

//==============================================================================
/** Plays the document, or the multitrack session, as the source node of the processing graph. */
class DocumentPlayerProcessor final : public juce::AudioProcessor
{
public:
    DocumentPlayerProcessor (AudioDocument&, Session&, SessionMixer::Inserts&);
    ~DocumentPlayerProcessor() override;

    enum class Mode { document, session };

    /** Switches between playing the document and the session; stops and goes to the start. */
    void setMode (Mode);
    Mode getMode() const noexcept                 { return mode; }

    /** Call after the document or session is replaced (its sample rate may have changed). */
    void resetSource();

    /** The rate the player was last set up for. */
    double getSourceRate() const noexcept         { return sourceRate; }

    /** Plays from start; stops by itself at stopFrame (-1 = the end). */
    void play (juce::int64 startFrame, juce::int64 stopFrame);
    void pause()                                  { transport.stop(); }
    bool isPlaying() const                        { return transport.isPlaying(); }
    juce::int64 getPositionFrames() const;
    void setPositionFrames (juce::int64);
    juce::int64 getStopFrame() const noexcept     { return stopAt; }

    /** The document frame after the last one sent to the output while playing. The audio thread
        advances it by each block it plays, so a recorder running later in the same device cycle can
        tell which frames went out (the resampler reads a few frames ahead of the source position).
        Counts device frames, which are document frames only when the two rates match. */
    const std::atomic<juce::int64>& getPlayedFrame() const noexcept { return playedFrame; }

    //==============================================================================
    const juce::String getName() const override   { return "Document Player"; }
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    double getTailLengthSeconds() const override  { return 0.0; }
    bool acceptsMidi() const override             { return false; }
    bool producesMidi() const override            { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override               { return false; }
    int getNumPrograms() override                 { return 1; }
    int getCurrentProgram() override              { return 0; }
    void setCurrentProgram (int) override         {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    double modeRate() const;

    AudioDocument& document;
    Session& session;
    DocumentSource source { document };
    SessionSource sessionSource;
    Mode mode = Mode::document;
    juce::AudioTransportSource transport;
    juce::int64 stopAt = -1;
    double sourceRate = 0.0;
    std::atomic<juce::int64> playedFrame { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DocumentPlayerProcessor)
};

//==============================================================================
/** Owns the audio device, the plug-in formats and the processing graph:
    document player -> effect rack (in order) -> a chosen stereo pair of device outputs.

    All public methods must be called on the message thread.
*/
class AudioEngine final : public juce::ChangeBroadcaster,
                          private juce::ChangeListener
{
public:
    AudioEngine();
    ~AudioEngine() override;

    /** Opens the audio device, restoring the saved setup when there is one. */
    juce::String openDevice (const juce::XmlElement* savedState);

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    juce::AudioPluginFormatManager pluginFormatManager;
    juce::KnownPluginList knownPlugins;

    AudioDocument document;
    Session session;
    DocumentPlayerProcessor& getPlayer()          { return *player; }

    /** Loads a file into the document. Returns an error message, or empty on success. */
    juce::String openFile (const juce::File& file);

    //==============================================================================
    /** Output routing: the rack's stereo output goes to device output channels
        firstChannel and firstChannel + 1 (absolute channel indices of the device). */
    int getOutputPair() const                     { return outputPairFirstChannel; }
    void setOutputPair (int firstChannel);
    juce::StringArray getDeviceOutputChannelNames() const;

    //==============================================================================
    /** Recording input: numChannels (1 or 2) device inputs from firstChannel (absolute index),
        or none when numChannels is 0. Turns those inputs on (and only those); macOS asks for
        microphone access the first time. Returns an error message, or empty on success. */
    juce::String setInput (int firstChannel, int numChannels);
    int getInputFirstChannel() const              { return inputFirstChannel; }
    int getInputNumChannels() const               { return inputNumChannels; }

    /** The input channel names of the device in use, even before any input is on. */
    juce::StringArray getDeviceInputChannelNames() const;

    Recorder& getRecorder()                       { return *recorder; }

    /** Frames from a document frame leaving the player to the same moment arriving back at the
        recorder: device output and input latency (each including a buffer) plus the rack's latency. */
    int getRoundTripLatency() const;

    //==============================================================================
    int getNumRackSlots() const                   { return (int) rack.size(); }
    juce::AudioPluginInstance* getRackPlugin (int index) const;
    bool isRackSlotBypassed (int index) const;
    void setRackSlotBypassed (int index, bool shouldBeBypassed);
    void moveRackSlot (int fromIndex, int toIndex);

    /** Called before a plug-in is removed so its editor window can be closed first. */
    std::function<void (juce::AudioProcessor*)> onPluginAboutToBeRemoved;

    using AddCallback = std::function<void (const juce::String& error)>;
    void addPlugin (const juce::PluginDescription& description, AddCallback onDone);
    void removeRackSlot (int index);
    void clearRack();

    std::unique_ptr<juce::XmlElement> createRackXml() const;
    void loadRackXml (const juce::XmlElement& xml, AddCallback onDone);

    //==============================================================================
    /** Each session track's effects. Read them here; change them through the methods below. */
    const TrackEffects& getTrackEffects() const   { return trackEffects; }

    /** Creates the plug-in at the session's rate and adds it to the end of the track's chain. */
    void addTrackPlugin (juce::uint32 trackId, const juce::PluginDescription&, AddCallback onDone);
    void removeTrackPlugin (juce::uint32 trackId, int slot);
    void setTrackPluginBypassed (juce::uint32 trackId, int slot, bool);
    void moveTrackPlugin (juce::uint32 trackId, int from, int to);

    /** Removes every track's effects (for a new session). */
    void clearTrackEffects();

    /** Restores track effects from session file EFFECTS elements, by track id, one after another. */
    void loadTrackEffects (std::map<juce::uint32, std::shared_ptr<juce::XmlElement>> effects, AddCallback onDone);

private:
    using Node = juce::AudioProcessorGraph::Node;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void rebuildConnections();
    void addPluginsFromXml (std::shared_ptr<std::vector<std::unique_ptr<juce::XmlElement>>> slots,
                            size_t next, juce::StringArray errors, AddCallback onDone);
    void createPlugin (const juce::PluginDescription&, double rate,
                       std::function<void (std::unique_ptr<juce::AudioPluginInstance>, const juce::String& error)>);
    double currentSampleRate() const;
    juce::String inputDeviceNameFor (const juce::AudioDeviceManager::AudioDeviceSetup&) const;
    int currentBlockSize() const;

    TrackEffects trackEffects;   // before the graph: the player in it uses them
    juce::AudioProcessorGraph graph;
    juce::AudioProcessorPlayer graphPlayer;
    DocumentPlayerProcessor* player = nullptr;
    Node::Ptr playerNode, outputNode;
    std::vector<Node::Ptr> rack;
    int outputPairFirstChannel = 0;
    int inputFirstChannel = 0, inputNumChannels = 0;
    std::unique_ptr<Recorder> recorder;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
