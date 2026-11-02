#pragma once

#include <JuceHeader.h>
#include "AudioDocument.h"
#include "AudioFileIO.h"
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

    /** Plays [start, end) over and over once it gets there (end <= start: no loop). */
    void setLoop (juce::int64 start, juce::int64 end)         { loopStart = start; loopEnd = end; }

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
    std::atomic<juce::int64> position { 0 }, stopFrame { -1 }, loopStart { 0 }, loopEnd { 0 };

    void readRun (juce::AudioBuffer<float>&, int destStart, juce::int64 start, int numFrames);
};

//==============================================================================
/** Mixes the session for playback, through each track's effects. Picks up edits on the fly. */
class SessionSource final : public juce::PositionableAudioSource
{
public:
    SessionSource (Session& s, SessionMixer::Inserts& i) : session (s), inserts (i) {}

    /** Frames from this one onwards play as silence (-1 = play to the end). */
    void setStopFrame (juce::int64 frame)                     { stopFrame = frame; }

    /** Plays [start, end) over and over once it gets there (end <= start: no loop). */
    void setLoop (juce::int64 start, juce::int64 end)         { loopStart = start; loopEnd = end; }

    /** Input monitoring, on the audio thread before each block: this cycle's input, its channels
        going to the tracks the layout says (none: off). It is kept in order, so however the
        transport's resampler splits its reads, the input plays through as one stream. */
    void feedLive (const SessionMixer::Live& layout, const juce::AudioBuffer<float>& input, int numFrames);

    /** While stopped: the live input alone (the clips silent) through the session at the cursor. */
    void renderLive (const juce::AudioSourceChannelInfo&);

    /** Meters the tracks and buses into these levels as it mixes. */
    void setLevels (SessionMixer::Levels* levels)             { buffers.levels = levels; }

    /** The loop set by setLoop (start, end; end <= start: none). */
    std::pair<juce::int64, juce::int64> getLoop() const noexcept { return { loopStart.load(), loopEnd.load() }; }

    /** Counts the blocks the transport has asked for (audio thread). */
    int getBlocksRead() const noexcept                         { return blocksRead; }

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
    std::atomic<juce::int64> position { 0 }, stopFrame { -1 }, loopStart { 0 }, loopEnd { 0 };

    // Input monitoring (audio thread): a FIFO of input frames, and a block taken from it.
    SessionMixer::Live live;
    int liveRead = 0, liveCount = 0, blocksRead = 0;
    juce::AudioBuffer<float> liveRing, liveBlock;

    void refreshState();
    const SessionMixer::Live* takeLive (int numFrames, bool clips);
    void renderRun (juce::int64 start, const juce::AudioSourceChannelInfo&, int destStart, int numFrames);
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

    /** Plays [start, end) of the document or session over and over once it gets there, until
        clearLoop() or the mode changes. The played frame keeps counting on through each pass, so
        frame f of pass k (from 0) is played frame f + k * (end - start). */
    void setLoop (juce::int64 start, juce::int64 end)      { source.setLoop (start, end); sessionSource.setLoop (start, end); }
    void clearLoop()                                       { setLoop (0, 0); }
    void pause()                                  { transport.stop(); }
    bool isPlaying() const                        { return transport.isPlaying(); }
    juce::int64 getPositionFrames() const;
    void setPositionFrames (juce::int64);
    juce::int64 getStopFrame() const noexcept     { return stopAt; }

    /** The metronome: while the session plays, a click on each beat of its tempo (accented on the
        first beat of each bar) goes out on the player's second output, Click (channels 2 and 3),
        which skips the effects rack. It follows the loop. Needs the device at the session's rate. */
    void setMetronome (bool on)                   { metronome = on; }
    bool isMetronomeOn() const noexcept           { return metronome.load(); }

    /** A count-in: clicks before this session frame even with the metronome off (-1: none). */
    void setCountIn (juce::int64 endFrame)        { countInEnd = endFrame; }

    /** Auditioning a file before it is opened: plays it once from its start, as it is (at its own
        rate, converted to the device's), on the second output with the click, so it skips the
        effects rack and leaves the document or session where it was. nullptr stops it. */
    void audition (std::unique_ptr<juce::AudioFormatReader>);
    bool isAuditioning() const                    { return auditioner.isPlaying(); }

    /** The session's tracks' and buses' levels as they play, by their place in the session. */
    SessionMixer::Levels& getLevels() noexcept    { return levels; }

    /** A track and the inputs it monitors: 1 or 2 from firstInput, numbered as the device
        callback numbers its (active) inputs. */
    struct MonitoredTrack
    {
        juce::uint32 trackId = 0;
        int firstInput = 0, numInputs = 0;
    };

    /** Input monitoring: in session mode, each of these tracks plays its inputs (given each cycle
        by setMonitorInput), joining its clips before its effects, while playing or stopped. Needs
        the device at the session's rate. Up to SessionMixer::Live::maxTracks; none: off. */
    void setMonitoredTracks (const std::vector<MonitoredTrack>&);

    /** On the audio thread, before the block of the same device cycle is processed: the device
        callback's inputs. */
    void setMonitorInput (const float* const* inputs, int numInputs, int numFrames);

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
    juce::TimeSliceThread auditionThread { "Audition" };
    std::unique_ptr<juce::AudioFormatReaderSource> auditionSource;   // before the transport playing it
    juce::AudioTransportSource auditioner;
    juce::AudioBuffer<float> auditionBuffer;
    SessionMixer::Levels levels;
    std::atomic<bool> metronome { false };
    std::atomic<juce::int64> countInEnd { -1 };
    Session::State clickState;                 // audio thread's own reference, for the tempo
    juce::uint32 clickVersion = 0;
    void addClicks (juce::AudioBuffer<float>&, juce::int64 firstFrame, int numFrames);

    juce::SpinLock monitoredLock;
    std::array<MonitoredTrack, SessionMixer::Live::maxTracks> monitored;   // guarded by the lock
    int numMonitored = 0;
    std::array<MonitoredTrack, SessionMixer::Live::maxTracks> monitorTracks;   // audio thread's copy
    int numMonitorTracks = 0;
    juce::AudioBuffer<float> monitorInput;     // audio thread: this cycle's input, two channels a track
    SessionMixer::Live monitorLayout;
    int monitorFrames = 0;
    double deviceRate = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DocumentPlayerProcessor)
};

//==============================================================================
/** Owns the audio device, the plug-in formats and the processing graph:
    document player -> effect rack (in order) -> master meter -> a chosen stereo pair of device outputs.

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
    bool isDeviceOpen() const noexcept            { return deviceOpen; }

    /** Asks the audio system for the output device a saved setup names (or the default one), with
        a device object of its own and from whatever thread calls it: the calls that wait when the
        system's audio service doesn't answer, made where waiting holds nothing up, so openDevice
        afterwards doesn't wait. Returns the device's name, or "" when there is none. */
    static juce::String probeDevice (const juce::XmlElement* savedState);

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    juce::AudioPluginFormatManager pluginFormatManager;
    juce::KnownPluginList knownPlugins;

    AudioDocument document;
    Session session;
    DocumentPlayerProcessor& getPlayer()          { return *player; }

    /** Loads a file into the document. Returns an error message, or empty on success. */
    juce::String openFile (const juce::File& file);

    /** Takes a file already read (by openFile, or on another thread) into the document. Returns
        its error, or empty on success. */
    juce::String adoptFile (const juce::File& file, AudioFileIO::Loaded);

    /** Plays a file once from its start without opening it (see DocumentPlayerProcessor::audition).
        Returns false, with nothing playing, when it can't be read; an empty File stops it. */
    bool auditionFile (const juce::File&);

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

    /** Several tracks at once: the inputs armed tracks record from. Each distinct one other than
        the recording input gets a recorder of its own, run after the player like the first, and
        its channels are turned on too. Call while not recording; does nothing when they are the
        same as before. Returns an error message, or empty on success. */
    juce::String setTrackInputs (const std::vector<TrackInput>&);

    /** The recorder for a track's input: the first one for the default input (or its channels),
        else one made by setTrackInputs; nullptr when there is none. */
    Recorder* getRecorderFor (const TrackInput&);

    /** Every recorder: the first, then one for each extra input. */
    std::vector<Recorder*> getRecorders();

    /** The peaks (left, right) of what went to the outputs since this reader's last call, after the
        effects rack (without the metronome's click), for a master meter. Each reader (the mixer's,
        the main window's) has its own, so neither takes the other's peaks. */
    enum class MasterReader { mixer, mainWindow };
    std::pair<float, float> takeMasterPeaks (MasterReader = MasterReader::mixer);

    /** Input monitoring: these tracks play what comes in on their inputs (none: off). */
    void setMonitoredTracks (const std::vector<std::pair<juce::uint32, TrackInput>>&);

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

    /** Previewing an effect: the player goes through this one before the rack, until it is
        replaced or cleared (nullptr). It is prepared at the device's rate. Not saved with the rack. */
    void setPreviewEffect (std::unique_ptr<juce::AudioPluginInstance>);
    juce::AudioPluginInstance* getPreviewEffect() const;
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

    /** Hears the track effects' parameters moved in their windows (TrackEffects::onParameterTouched),
        and holds one off its envelope while it is touched. */
    void setTrackEffectsListener (std::function<void (const TrackEffects::ParameterTouch&)> listener) { trackEffects.onParameterTouched = std::move (listener); }
    void setTrackEffectTouched (juce::uint32 trackId, juce::uint32 slotId, const juce::String& parameterId, bool touched)
    {
        trackEffects.setTouched (trackId, slotId, parameterId, touched);
    }

    /** Restores track effects from session file EFFECTS elements, by track id, one after another. */
    void loadTrackEffects (std::map<juce::uint32, std::shared_ptr<juce::XmlElement>> effects, AddCallback onDone);

private:
    using Node = juce::AudioProcessorGraph::Node;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void rebuildConnections();
    juce::String deviceSignature() const;   // the device in use, its rate, block size and channels
    juce::String lastDeviceSignature;       // as of the last change handled
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
    Node::Ptr playerNode, outputNode, meterNode, previewNode;
    class MasterMeter;
    std::vector<Node::Ptr> rack;
    int outputPairFirstChannel = 0;
    int inputFirstChannel = 0, inputNumChannels = 0;
    std::unique_ptr<Recorder> recorder;
    std::vector<std::pair<TrackInput, std::unique_ptr<Recorder>>> extraRecorders;
    std::vector<TrackInput> requestedInputs;   // the last setTrackInputs, and the recording input then
    TrackInput requestedMain;
    juce::BigInteger appliedInputs;            // the device's inputs on after they were last set
    bool trackInputsCurrent = false;
    bool deviceOpen = false;
    juce::String applyInputChannels();
    class MonitorTap;
    std::unique_ptr<MonitorTap> monitorTap;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
