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
    buffers.reserve (juce::jmax (8192, 4 * blockSize));
    nextFrame = -1;
}

void SessionSource::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    if (const auto v = session.getVersion(); state == nullptr || v != stateVersion)
    {
        state = session.getState();
        stateVersion = v;
    }

    const auto start = position.load();
    const auto stop = stopFrame.load();
    auto count = info.numSamples;

    if (stop >= 0)
        count = (int) juce::jlimit ((juce::int64) 0, (juce::int64) info.numSamples, stop - start);

    // After a jump, effects with latency are run over the frames they read ahead, so they play
    // in time from the first block.
    if (start != nextFrame)
        SessionMixer::prime (*state, start, buffers, inserts);

    SessionMixer::render (*state, start, *info.buffer, info.startSample, count, buffers, &inserts);

    if (count < info.numSamples)
        info.buffer->clear (info.startSample + count, info.numSamples - count);

    position = nextFrame = start + info.numSamples;
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
    transport.prepareToPlay (blockSize, sampleRate);
}

void DocumentPlayerProcessor::releaseResources()
{
    transport.releaseResources();
}

void DocumentPlayerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::AudioSourceChannelInfo info (&buffer, 0, buffer.getNumSamples());
    const auto playing = transport.isPlaying();
    transport.getNextAudioBlock (info);

    if (playing)
        playedFrame += buffer.getNumSamples();
}

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
    graphPlayer.setProcessor (nullptr);
    deviceManager.closeAudioDevice();
}

juce::String AudioEngine::openDevice (const juce::XmlElement* savedState)
{
    // Inputs stay off until one is chosen for recording; up to 256 outputs so large interfaces such
    // as a 30-output Fireface show every channel. With no saved state the system default device
    // opens at its current sample rate, so other apps sharing the interface are not disturbed.
    auto error = deviceManager.initialise (0, 256, savedState, true);

    // The recorder runs after the player in each cycle, so it sees what the player just played.
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
                        engine.trackEffects.add (trackId, std::move (instance));
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
