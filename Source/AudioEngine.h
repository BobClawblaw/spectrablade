#pragma once

#include <JuceHeader.h>
#include "AudioDocument.h"

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
/** Plays the document as the source node of the processing graph. */
class DocumentPlayerProcessor final : public juce::AudioProcessor
{
public:
    explicit DocumentPlayerProcessor (AudioDocument&);
    ~DocumentPlayerProcessor() override;

    /** Call after the document is replaced (its sample rate may have changed). */
    void documentReset();

    /** Plays from start; stops by itself at stopFrame (-1 = the end). */
    void play (juce::int64 startFrame, juce::int64 stopFrame);
    void pause()                                  { transport.stop(); }
    bool isPlaying() const                        { return transport.isPlaying(); }
    juce::int64 getPositionFrames() const;
    void setPositionFrames (juce::int64);
    juce::int64 getStopFrame() const noexcept     { return stopAt; }

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
    AudioDocument& document;
    DocumentSource source { document };
    juce::AudioTransportSource transport;
    juce::int64 stopAt = -1;

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

private:
    using Node = juce::AudioProcessorGraph::Node;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void rebuildConnections();
    void addPluginsFromXml (std::shared_ptr<std::vector<std::unique_ptr<juce::XmlElement>>> slots,
                            size_t next, juce::StringArray errors, AddCallback onDone);
    double currentSampleRate() const;
    int currentBlockSize() const;

    juce::AudioProcessorGraph graph;
    juce::AudioProcessorPlayer graphPlayer;
    DocumentPlayerProcessor* player = nullptr;
    Node::Ptr playerNode, outputNode;
    std::vector<Node::Ptr> rack;
    int outputPairFirstChannel = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
