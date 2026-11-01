#include "AudioEngine.h"

#include "AudioFileIO.h"
#include "SessionMixer.h"

//==============================================================================
juce::int64 DocumentSource::getTotalLength() const
{
    return document.getSnapshot()->getNumFrames();
}

void DocumentSource::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    // Edits swap the snapshot; take a reference to the new one only when the version changes.
    if (const auto v = document.getVersion(); snapshot == nullptr || v != snapshotVersion)
    {
        snapshot = document.getSnapshot();
        snapshotVersion = v;
    }

    auto& buffer = *info.buffer;
    const auto start = position.load();
    const auto stop = stopFrame.load();
    auto count = info.numSamples;

    if (stop >= 0)
        count = (int) juce::jlimit ((juce::int64) 0, (juce::int64) info.numSamples, stop - start);

    snapshot->read (buffer, info.startSample, start, count);

    if (count < info.numSamples)
        buffer.clear (info.startSample + count, info.numSamples - count);

    // Mono documents play on both sides.
    if (snapshot->getNumChannels() == 1)
        for (int ch = 1; ch < buffer.getNumChannels(); ++ch)
            buffer.copyFrom (ch, info.startSample, buffer, 0, info.startSample, info.numSamples);

    position = start + info.numSamples;
}

//==============================================================================
void SessionSource::prepareToPlay (int blockSize, double)
{
    // Room for the largest block the transport's resampler is likely to ask for, so the mixer
    // does not allocate on the audio thread.
    const auto frames = juce::jmax (8192, 4 * blockSize);
    buffers.reserve (frames);
    liveBlock.setSize (2, frames);
    liveRing.setSize (2, juce::nextPowerOfTwo (2 * frames));
    liveRead = liveCount = 0;
    nextFrame = -1;
}

void SessionSource::refreshState()
{
    if (const auto v = session.getVersion(); state == nullptr || v != stateVersion)
    {
        state = session.getState();
        stateVersion = v;
    }
}

void SessionSource::feedLive (juce::uint32 trackId, const juce::AudioBuffer<float>& input, int numChannels, int numFrames)
{
    const auto size = liveRing.getNumSamples();

    if (trackId == 0 || numChannels <= 0 || numFrames <= 0 || numFrames > size / 2)
    {
        liveTrack = 0;
        liveRead = liveCount = 0;
        return;
    }

    liveTrack = trackId;
    liveChannels = juce::jmin (2, numChannels);
    const auto mask = size - 1;

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto* in = input.getReadPointer (juce::jmin (ch, liveChannels - 1));
        auto* ring = liveRing.getWritePointer (ch);

        for (int i = 0; i < numFrames; ++i)
            ring[(liveRead + liveCount + i) & mask] = in[i];
    }

    // Keep no more than this block and a few frames the resampler reads ahead, so input
    // that was not read (stopped, a short read) never builds up into a delay.
    liveCount += numFrames;
    const auto keep = numFrames + 8;

    if (liveCount > keep)
    {
        liveRead = (liveRead + liveCount - keep) & mask;
        liveCount = keep;
    }
}

const SessionMixer::LiveInput* SessionSource::takeLive (int numFrames, bool clips)
{
    if (liveTrack == 0 || numFrames > liveBlock.getNumSamples())
        return nullptr;

    // Short of input (the resampler's first read wants a few frames more): silence first.
    const auto have = juce::jmin (numFrames, liveCount);
    const auto pad = numFrames - have;
    const auto mask = liveRing.getNumSamples() - 1;

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto* ring = liveRing.getReadPointer (ch);
        auto* out = liveBlock.getWritePointer (ch);
        juce::FloatVectorOperations::clear (out, pad);

        for (int i = 0; i < have; ++i)
            out[pad + i] = ring[(liveRead + i) & mask];
    }

    liveRead = (liveRead + have) & mask;
    liveCount -= have;
    live = { liveTrack, &liveBlock, liveChannels, clips };
    return &live;
}

void SessionSource::renderLive (const juce::AudioSourceChannelInfo& info)
{
    refreshState();
    const auto* in = takeLive (info.numSamples, false);

    if (in == nullptr)
    {
        info.clearActiveBufferRegion();
        return;
    }

    SessionMixer::render (*state, position.load(), *info.buffer, info.startSample, info.numSamples, buffers, &inserts, in);
    nextFrame = -1;     // playing from here starts in time, after a jump
}

void SessionSource::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    ++blocksRead;
    refreshState();

    auto start = position.load();
    const auto from = loopStart.load(), to = loopEnd.load();

    // A loop: up to its end, then back to its start, as often as the block needs.
    if (to > from && start < to)
    {
        for (int done = 0; done < info.numSamples;)
        {
            const auto n = (int) juce::jmin ((juce::int64) (info.numSamples - done), to - start);
            renderRun (start, info, info.startSample + done, n);
            done += n;
            start += n;

            if (start >= to)
                start = from;
        }

        position = nextFrame = start;
        return;
    }

    renderRun (start, info, info.startSample, info.numSamples);
    position = nextFrame = start + info.numSamples;
}

void SessionSource::renderRun (juce::int64 start, const juce::AudioSourceChannelInfo& info, int destStart, int numFrames)
{
    const auto stop = stopFrame.load();
    auto count = numFrames;

    if (stop >= 0)
        count = (int) juce::jlimit ((juce::int64) 0, (juce::int64) numFrames, stop - start);

    // After a jump, effects with latency are run over the frames they read ahead, so they play
    // in time from the first block.
    if (start != nextFrame)
        SessionMixer::prime (*state, start, buffers, inserts);

    SessionMixer::render (*state, start, *info.buffer, destStart, count, buffers, &inserts, takeLive (numFrames, true));

    if (count < numFrames)
        info.buffer->clear (destStart + count, numFrames - count);

    nextFrame = start + numFrames;
}

//==============================================================================
DocumentPlayerProcessor::DocumentPlayerProcessor (AudioDocument& doc, Session& s, SessionMixer::Inserts& inserts)
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      document (doc), session (s), sessionSource (s, inserts)
{
    resetSource();
}

DocumentPlayerProcessor::~DocumentPlayerProcessor()
{
    transport.setSource (nullptr);
}

double DocumentPlayerProcessor::modeRate() const
{
    return mode == Mode::session ? session.getSampleRate() : document.getSampleRate();
}

void DocumentPlayerProcessor::setMode (Mode m)
{
    if (m == mode)
        return;

    mode = m;
    resetSource();
}

void DocumentPlayerProcessor::resetSource()
{
    transport.stop();
    // No read-ahead: the audio is already in memory. The transport resamples from the source's
    // rate to the device's, so the interface keeps its own rate.
    sourceRate = modeRate();
    transport.setSource (mode == Mode::session ? static_cast<juce::PositionableAudioSource*> (&sessionSource) : &source,
                         0, nullptr, sourceRate, 2);
    transport.setPosition (0.0);
    stopAt = -1;
    source.setStopFrame (-1);
    sessionSource.setStopFrame (-1);
    sessionSource.setLoop (0, 0);
}

void DocumentPlayerProcessor::play (juce::int64 startFrame, juce::int64 stopFrame)
{
    transport.stop();
    stopAt = stopFrame;
    source.setStopFrame (stopFrame);
    sessionSource.setStopFrame (stopFrame);
    setPositionFrames (startFrame);
    transport.start();
}

juce::int64 DocumentPlayerProcessor::getPositionFrames() const
{
    return (juce::int64) std::llround (transport.getCurrentPosition() * modeRate());
}

void DocumentPlayerProcessor::setPositionFrames (juce::int64 frame)
{
    transport.setPosition ((double) frame / modeRate());
    playedFrame = frame;
}

void DocumentPlayerProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    deviceRate = sampleRate;
    monitorInput.setSize (2, juce::jmax (8192, 4 * blockSize));
    monitorFrames = 0;
    transport.prepareToPlay (blockSize, sampleRate);
}

void DocumentPlayerProcessor::setMonitorInput (const float* const* channels, int numChannels, int numFrames)
{
    monitorChannels = channels != nullptr && numFrames <= monitorInput.getNumSamples() ? juce::jmin (2, numChannels) : 0;
    monitorFrames = numFrames;

    for (int ch = 0; ch < monitorChannels; ++ch)
        monitorInput.copyFrom (ch, 0, channels[ch], numFrames);
}

void DocumentPlayerProcessor::releaseResources()
{
    transport.releaseResources();
}

void DocumentPlayerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const auto numFrames = buffer.getNumSamples();
    const auto track = monitoredTrack.load();
    const auto monitoring = mode == Mode::session && track != 0 && monitorChannels > 0 && monitorFrames == numFrames
                            && juce::exactlyEqual (sourceRate, deviceRate);
    sessionSource.feedLive (monitoring ? track : 0, monitorInput, monitorChannels, numFrames);
    monitorFrames = 0;

    juce::AudioSourceChannelInfo info (&buffer, 0, numFrames);
    const auto playing = transport.isPlaying();
    const auto blocksRead = sessionSource.getBlocksRead();
    transport.getNextAudioBlock (info);

    // Stopped (the transport read nothing and played silence): the input plays on its own.
    if (monitoring && ! playing && sessionSource.getBlocksRead() == blocksRead)
        sessionSource.renderLive (info);

    if (playing)
        playedFrame += numFrames;
}

//==============================================================================
/** Runs before the player in each device cycle and hands it the recording input to monitor. */
class AudioEngine::MonitorTap final : public juce::AudioIODeviceCallback
{
public:
    MonitorTap (DocumentPlayerProcessor& p, const Recorder& r) : player (p), recorder (r) {}

    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputs[ch], numSamples);

        const auto [first, count] = recorder.getMappedInput();
        const auto usable = first >= 0 && count > 0 && first + count <= numInputs;
        player.setMonitorInput (usable ? inputs + first : nullptr, usable ? count : 0, numSamples);
    }

    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

private:
    DocumentPlayerProcessor& player;
    const Recorder& recorder;
};

//==============================================================================
AudioEngine::AudioEngine()
{
    formatManager.registerBasicFormats();
    juce::addDefaultFormatsToManager (pluginFormatManager);
    trackEffects.prepare (session.getSampleRate());

    auto playerProcessor = std::make_unique<DocumentPlayerProcessor> (document, session, trackEffects);
    player = playerProcessor.get();
    playerNode = graph.addNode (std::move (playerProcessor));

    using IO = juce::AudioProcessorGraph::AudioGraphIOProcessor;
    outputNode = graph.addNode (std::make_unique<IO> (IO::audioOutputNode));
    recorder = std::make_unique<Recorder> (player->getPlayedFrame());
    monitorTap = std::make_unique<MonitorTap> (*player, *recorder);

    graphPlayer.setProcessor (&graph);
    deviceManager.addChangeListener (this);
    document.addChangeListener (this);
    session.addChangeListener (this);
}

AudioEngine::~AudioEngine()
{
    *alive = false;
    document.removeChangeListener (this);
    session.removeChangeListener (this);
    deviceManager.removeChangeListener (this);
    deviceManager.removeAudioCallback (recorder.get());
    deviceManager.removeAudioCallback (&graphPlayer);
    deviceManager.removeAudioCallback (monitorTap.get());
    graphPlayer.setProcessor (nullptr);
    deviceManager.closeAudioDevice();
}

juce::String AudioEngine::openDevice (const juce::XmlElement* savedState)
{
    // Inputs stay off until one is chosen for recording; up to 256 outputs so large interfaces such
    // as a 30-output Fireface show every channel. With no saved state the system default device
    // opens at its current sample rate, so other apps sharing the interface are not disturbed.
    auto error = deviceManager.initialise (0, 256, savedState, true);

    // The monitor tap runs before the player in each cycle, so the player hears this cycle's
    // input; the recorder runs after it, so it sees what the player just played.
    deviceManager.addAudioCallback (monitorTap.get());
    deviceManager.addAudioCallback (&graphPlayer);
    deviceManager.addAudioCallback (recorder.get());
    rebuildConnections();
    return error;
}

juce::String AudioEngine::openFile (const juce::File& file)
{
    auto loaded = AudioFileIO::load (file, formatManager);

    if (loaded.error.isNotEmpty())
        return loaded.error;

    player->pause();
    document.reset (std::move (loaded.snapshot), loaded.sampleRate, file, loaded.bitDepth, std::move (loaded.markers));
    player->resetSource();
    sendChangeMessage();
    return {};
}

//==============================================================================
juce::StringArray AudioEngine::getDeviceOutputChannelNames() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getOutputChannelNames();

    return {};
}

juce::String AudioEngine::inputDeviceNameFor (const juce::AudioDeviceManager::AudioDeviceSetup& setup) const
{
    if (setup.inputDeviceName.isNotEmpty())
        return setup.inputDeviceName;

    auto* type = deviceManager.getCurrentDeviceTypeObject();

    if (type == nullptr)
        return {};

    // Prefer the output interface's own inputs (a Fireface records and plays through one device).
    const auto inputs = type->getDeviceNames (true);

    if (inputs.contains (setup.outputDeviceName))
        return setup.outputDeviceName;

    return inputs[type->getDefaultDeviceIndex (true)];
}

juce::StringArray AudioEngine::getDeviceInputChannelNames() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        if (const auto names = device->getInputChannelNames(); ! names.isEmpty())
            return names;

    // No input open yet: ask an unopened device object for its names.
    auto* type = deviceManager.getCurrentDeviceTypeObject();
    const auto name = inputDeviceNameFor (deviceManager.getAudioDeviceSetup());

    if (type == nullptr || name.isEmpty())
        return {};

    if (std::unique_ptr<juce::AudioIODevice> device (type->createDevice ({}, name)); device != nullptr)
        return device->getInputChannelNames();

    return {};
}

juce::String AudioEngine::setInput (int firstChannel, int numChannels)
{
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();

    const auto names = getDeviceInputChannelNames();
    numChannels = juce::jlimit (0, 2, juce::jmin (numChannels, names.size() - firstChannel));

    if (numChannels > 0)
    {
        setup.inputDeviceName = inputDeviceNameFor (setup);
        setup.inputChannels.setRange (firstChannel, numChannels, true);
    }

    inputFirstChannel = juce::jmax (0, firstChannel);
    inputNumChannels = juce::jmax (0, numChannels);

    if (inputNumChannels > 0)
        recorder->setInput (inputFirstChannel, inputNumChannels);

    auto error = deviceManager.setAudioDeviceSetup (setup, true);

    // A setup that needed no restart does not call the recorder back, so update it here too.
    if (auto* device = deviceManager.getCurrentAudioDevice())
        recorder->configure (device->getCurrentSampleRate(), inputNumChannels > 0 ? device->getActiveInputChannels()
                                                                                  : juce::BigInteger());

    sendChangeMessage();
    return error;
}

int AudioEngine::getRoundTripLatency() const
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
        return 0;

    return device->getInputLatencyInSamples() + device->getOutputLatencyInSamples() + graph.getLatencySamples();
}

void AudioEngine::setOutputPair (int firstChannel)
{
    // A pair saved for a larger interface falls back to the first pair on a smaller one.
    const auto numDeviceOutputs = getDeviceOutputChannelNames().size();
    outputPairFirstChannel = juce::isPositiveAndBelow (firstChannel, numDeviceOutputs) ? firstChannel : 0;

    // The device only delivers buffers for active channels, so make sure the chosen pair is on.
    auto setup = deviceManager.getAudioDeviceSetup();
    auto changed = false;

    for (int ch = outputPairFirstChannel; ch < juce::jmin (outputPairFirstChannel + 2, numDeviceOutputs); ++ch)
    {
        if (! setup.outputChannels[ch])
        {
            setup.outputChannels.setBit (ch);
            changed = true;
        }
    }

    if (changed)
    {
        setup.useDefaultOutputChannels = false;
        deviceManager.setAudioDeviceSetup (setup, true);
    }

    rebuildConnections();
    sendChangeMessage();
}

void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &document || source == &session)
    {
        // A conversion (or undoing one), or a new session, changed the rate the player must resample from.
        const auto rate = player->getMode() == DocumentPlayerProcessor::Mode::session ? session.getSampleRate()
                                                                                     : document.getSampleRate();

        if (! juce::exactlyEqual (player->getSourceRate(), rate))
            player->resetSource();

        if (! juce::exactlyEqual (trackEffects.getSampleRate(), session.getSampleRate()))
            trackEffects.prepare (session.getSampleRate());

        return;
    }

    // Device, sample rate, buffer size or active channels changed.
    rebuildConnections();
    sendChangeMessage();
}

void AudioEngine::rebuildConnections()
{
    for (auto& c : graph.getConnections())
        graph.removeConnection (c, juce::AudioProcessorGraph::UpdateKind::none);

    // Chain: player -> each rack slot in order -> output. Bypassed slots stay in the chain;
    // the graph calls their bypass processing so latency compensation keeps working.
    auto* source = playerNode.get();

    auto connectStereo = [this] (Node* from, Node* to, int toFirstChannel)
    {
        const auto fromChannels = from->getProcessor()->getTotalNumOutputChannels();
        const auto toChannels   = to->getProcessor()->getTotalNumInputChannels();

        for (int ch = 0; ch < 2; ++ch)
        {
            const auto src = juce::jmin (ch, fromChannels - 1);
            const auto dst = toFirstChannel + ch;

            if (src >= 0 && dst < toChannels)
                graph.addConnection ({ { from->nodeID, src }, { to->nodeID, dst } },
                                     juce::AudioProcessorGraph::UpdateKind::none);
        }
    };

    for (auto& slot : rack)
    {
        connectStereo (source, slot.get(), 0);
        source = slot.get();
    }

    // The graph's output channels are the device's *active* output channels, in order,
    // so map the absolute device channel to its position among the active ones.
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const auto active = device->getActiveOutputChannels();

        if (active[outputPairFirstChannel])
        {
            int graphChannel = 0;

            for (int ch = 0; ch < outputPairFirstChannel; ++ch)
                if (active[ch])
                    ++graphChannel;

            connectStereo (source, outputNode.get(), graphChannel);

            // A pair whose second channel is inactive (or a mono device) still gets the left side.
            if (! active[outputPairFirstChannel + 1])
                graph.removeConnection ({ { source->nodeID, 1 }, { outputNode->nodeID, graphChannel + 1 } },
                                        juce::AudioProcessorGraph::UpdateKind::none);
        }
    }

    graph.rebuild();
}

//==============================================================================
juce::AudioPluginInstance* AudioEngine::getRackPlugin (int index) const
{
    if (juce::isPositiveAndBelow (index, (int) rack.size()))
        return dynamic_cast<juce::AudioPluginInstance*> (rack[(size_t) index]->getProcessor());

    return nullptr;
}

bool AudioEngine::isRackSlotBypassed (int index) const
{
    return juce::isPositiveAndBelow (index, (int) rack.size()) && rack[(size_t) index]->isBypassed();
}

void AudioEngine::setRackSlotBypassed (int index, bool shouldBeBypassed)
{
    if (juce::isPositiveAndBelow (index, (int) rack.size()))
    {
        rack[(size_t) index]->setBypassed (shouldBeBypassed);
        sendChangeMessage();
    }
}

void AudioEngine::moveRackSlot (int fromIndex, int toIndex)
{
    if (! juce::isPositiveAndBelow (fromIndex, (int) rack.size())
        || ! juce::isPositiveAndBelow (toIndex, (int) rack.size())
        || fromIndex == toIndex)
        return;

    auto node = rack[(size_t) fromIndex];
    rack.erase (rack.begin() + fromIndex);
    rack.insert (rack.begin() + toIndex, node);
    rebuildConnections();
    sendChangeMessage();
}

double AudioEngine::currentSampleRate() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentSampleRate();

    return 44100.0;
}

int AudioEngine::currentBlockSize() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getCurrentBufferSizeSamples();

    return 512;
}

void AudioEngine::createPlugin (const juce::PluginDescription& description, double rate,
                                std::function<void (std::unique_ptr<juce::AudioPluginInstance>, const juce::String&)> onCreated)
{
    // Asynchronous creation is required for AUv3 and keeps the UI responsive for slow plug-ins.
    pluginFormatManager.createPluginInstanceAsync (description, rate, currentBlockSize(),
        [isAlive = alive, done = std::move (onCreated)] (std::unique_ptr<juce::AudioPluginInstance> instance,
                                                          const juce::String& error)
        {
            if (! *isAlive)
                return;

            if (instance == nullptr)
            {
                done (nullptr, error.isNotEmpty() ? error : juce::String ("The plug-in could not be created."));
                return;
            }

            instance->enableAllBuses();
            done (std::move (instance), {});
        });
}

void AudioEngine::addPlugin (const juce::PluginDescription& description, AddCallback onDone)
{
    createPlugin (description, currentSampleRate(),
                  [this, done = std::move (onDone)] (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
                  {
                      if (instance != nullptr)
                      {
                          rack.push_back (graph.addNode (std::move (instance)));
                          rebuildConnections();
                          sendChangeMessage();
                      }

                      if (done != nullptr)
                          done (error);
                  });
}

void AudioEngine::removeRackSlot (int index)
{
    if (! juce::isPositiveAndBelow (index, (int) rack.size()))
        return;

    auto node = rack[(size_t) index];

    if (onPluginAboutToBeRemoved != nullptr)
        onPluginAboutToBeRemoved (node->getProcessor());

    rack.erase (rack.begin() + index);
    graph.removeNode (node.get());
    rebuildConnections();
    sendChangeMessage();
}

void AudioEngine::clearRack()
{
    while (! rack.empty())
        removeRackSlot ((int) rack.size() - 1);
}

//==============================================================================
std::unique_ptr<juce::XmlElement> AudioEngine::createRackXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("SPECTRABLADE_RACK");
    xml->setAttribute ("version", 1);

    for (auto& node : rack)
    {
        auto* plugin = dynamic_cast<juce::AudioPluginInstance*> (node->getProcessor());

        if (plugin == nullptr)
            continue;

        auto* slot = xml->createNewChildElement ("SLOT");
        slot->setAttribute ("bypassed", node->isBypassed());
        slot->addChildElement (plugin->getPluginDescription().createXml().release());

        juce::MemoryBlock state;
        plugin->getStateInformation (state);
        slot->createNewChildElement ("STATE")->addTextElement (state.toBase64Encoding());
    }

    return xml;
}

void AudioEngine::loadRackXml (const juce::XmlElement& xml, AddCallback onDone)
{
    // WAVEDRILL_RACK and WAVELOOM_RACK: racks saved under the app's earlier names.
    if (! xml.hasTagName ("SPECTRABLADE_RACK") && ! xml.hasTagName ("WAVEDRILL_RACK") && ! xml.hasTagName ("WAVELOOM_RACK"))
    {
        if (onDone != nullptr)
            onDone ("Not a SpectraBlade rack file.");
        return;
    }

    clearRack();

    auto slots = std::make_shared<std::vector<std::unique_ptr<juce::XmlElement>>>();

    for (auto* slot : xml.getChildWithTagNameIterator ("SLOT"))
        slots->push_back (std::make_unique<juce::XmlElement> (*slot));

    addPluginsFromXml (slots, 0, {}, std::move (onDone));
}

void AudioEngine::addPluginsFromXml (std::shared_ptr<std::vector<std::unique_ptr<juce::XmlElement>>> slots,
                                     size_t next, juce::StringArray errors, AddCallback onDone)
{
    if (next >= slots->size())
    {
        if (onDone != nullptr)
            onDone (errors.joinIntoString ("\n"));
        return;
    }

    const auto& slot = *(*slots)[next];
    juce::PluginDescription description;

    if (auto* descXml = slot.getChildByName ("PLUGIN"); descXml == nullptr || ! description.loadFromXml (*descXml))
    {
        errors.add ("Slot " + juce::String ((int) next + 1) + ": missing plug-in description.");
        addPluginsFromXml (slots, next + 1, errors, std::move (onDone));
        return;
    }

    // Plug-ins load one after another so the rack keeps its order.
    addPlugin (description, [this, slots, next, errors, onDone, description] (const juce::String& error) mutable
    {
        if (error.isNotEmpty())
        {
            errors.add (description.name + ": " + error);
        }
        else
        {
            const auto& loadedSlot = *(*slots)[next];
            auto* plugin = getRackPlugin ((int) rack.size() - 1);

            if (auto* stateXml = loadedSlot.getChildByName ("STATE"); plugin != nullptr && stateXml != nullptr)
            {
                juce::MemoryBlock state;

                if (state.fromBase64Encoding (stateXml->getAllSubText()))
                    plugin->setStateInformation (state.getData(), (int) state.getSize());
            }

            setRackSlotBypassed ((int) rack.size() - 1, loadedSlot.getBoolAttribute ("bypassed"));
        }

        addPluginsFromXml (slots, next + 1, errors, std::move (onDone));
    });
}

//==============================================================================
void AudioEngine::addTrackPlugin (juce::uint32 trackId, const juce::PluginDescription& description, AddCallback onDone)
{
    createPlugin (description, session.getSampleRate(),
                  [this, trackId, done = std::move (onDone)] (std::unique_ptr<juce::AudioPluginInstance> instance,
                                                              const juce::String& error)
                  {
                      if (instance != nullptr)
                      {
                          trackEffects.add (trackId, std::move (instance));
                          session.markEffectsChanged();
                      }

                      if (done != nullptr)
                          done (error);
                  });
}

void AudioEngine::removeTrackPlugin (juce::uint32 trackId, int slot)
{
    auto* plugin = trackEffects.getPlugin (trackId, slot);

    if (plugin == nullptr)
        return;

    if (onPluginAboutToBeRemoved != nullptr)
        onPluginAboutToBeRemoved (plugin);

    trackEffects.remove (trackId, slot);
    session.markEffectsChanged();
}

void AudioEngine::setTrackPluginBypassed (juce::uint32 trackId, int slot, bool bypassed)
{
    trackEffects.setBypassed (trackId, slot, bypassed);
    session.markEffectsChanged();
}

void AudioEngine::moveTrackPlugin (juce::uint32 trackId, int from, int to)
{
    trackEffects.move (trackId, from, to);
    session.markEffectsChanged();
}

void AudioEngine::clearTrackEffects()
{
    if (onPluginAboutToBeRemoved != nullptr)
        for (auto* plugin : trackEffects.getAllPlugins())
            onPluginAboutToBeRemoved (plugin);

    trackEffects.clear();
}

void AudioEngine::loadTrackEffects (std::map<juce::uint32, std::shared_ptr<juce::XmlElement>> effects, AddCallback onDone)
{
    // Flattened to (track, slot) pairs, loaded one after another so each chain keeps its order.
    auto queue = std::make_shared<std::vector<std::pair<juce::uint32, std::shared_ptr<juce::XmlElement>>>>();

    for (auto& [id, xml] : effects)
        if (xml != nullptr)
            for (auto* slot : xml->getChildWithTagNameIterator ("SLOT"))
                queue->emplace_back (id, std::make_shared<juce::XmlElement> (*slot));

    struct Loader
    {
        static void next (AudioEngine& engine, std::shared_ptr<bool> isAlive, decltype (queue) q, size_t index,
                          juce::StringArray errors, AddCallback done)
        {
            if (! *isAlive)
                return;

            if (index >= q->size())
            {
                if (done != nullptr)
                    done (errors.joinIntoString ("\n"));
                return;
            }

            const auto [trackId, slot] = (*q)[index];
            juce::PluginDescription description;

            if (auto* descXml = slot->getChildByName ("PLUGIN"); descXml == nullptr || ! description.loadFromXml (*descXml))
            {
                errors.add ("A track effect is missing its plug-in description.");
                next (engine, isAlive, q, index + 1, errors, std::move (done));
                return;
            }

            engine.createPlugin (description, engine.session.getSampleRate(),
                [&engine, isAlive, q, index, errors, done, description, trackId, slot]
                (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error) mutable
                {
                    if (instance == nullptr)
                    {
                        errors.add (description.name + ": " + error);
                    }
                    else
                    {
                        // State after preparing, as the rack does it.
                        auto* plugin = instance.get();
                        engine.trackEffects.add (trackId, std::move (instance), (juce::uint32) slot->getIntAttribute ("id"));
                        const auto added = engine.trackEffects.getNumSlots (trackId) - 1;
                        engine.trackEffects.setBypassed (trackId, added, slot->getBoolAttribute ("bypassed"));

                        if (auto* stateXml = slot->getChildByName ("STATE"))
                        {
                            juce::MemoryBlock state;

                            if (state.fromBase64Encoding (stateXml->getAllSubText()))
                                plugin->setStateInformation (state.getData(), (int) state.getSize());
                        }
                    }

                    next (engine, isAlive, q, index + 1, errors, std::move (done));
                });
        }
    };

    Loader::next (*this, alive, queue, 0, {}, std::move (onDone));
}
