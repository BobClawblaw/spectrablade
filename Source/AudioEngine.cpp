#include "AudioEngine.h"

#include "AudioFileIO.h"
#include "Metronome.h"
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
    liveBlock.setSize (2 * SessionMixer::Live::maxTracks, frames);
    liveRing.setSize (2 * SessionMixer::Live::maxTracks, juce::nextPowerOfTwo (2 * frames));
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

void SessionSource::feedLive (const SessionMixer::Live& layout, const juce::AudioBuffer<float>& input, int numFrames)
{
    const auto size = liveRing.getNumSamples();

    if (layout.count == 0 || numFrames <= 0 || numFrames > size / 2)
    {
        live.count = 0;
        liveRead = liveCount = 0;
        return;
    }

    live.inputs = layout.inputs;
    live.count = layout.count;
    const auto channels = juce::jmin (input.getNumChannels(), liveRing.getNumChannels());
    const auto mask = size - 1;

    for (int ch = 0; ch < channels; ++ch)
    {
        const auto* in = input.getReadPointer (ch);
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

const SessionMixer::Live* SessionSource::takeLive (int numFrames, bool clips)
{
    if (live.count == 0 || numFrames > liveBlock.getNumSamples())
        return nullptr;

    // Short of input (the resampler's first read wants a few frames more): silence first.
    const auto have = juce::jmin (numFrames, liveCount);
    const auto pad = numFrames - have;
    const auto mask = liveRing.getNumSamples() - 1;

    for (int ch = 0; ch < liveBlock.getNumChannels(); ++ch)
    {
        const auto* ring = liveRing.getReadPointer (ch);
        auto* out = liveBlock.getWritePointer (ch);
        juce::FloatVectorOperations::clear (out, pad);

        for (int i = 0; i < have; ++i)
            out[pad + i] = ring[(liveRead + i) & mask];
    }

    liveRead = (liveRead + have) & mask;
    liveCount -= have;
    live.audio = &liveBlock;
    live.clips = clips;
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
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Click", juce::AudioChannelSet::stereo(), true)),
      document (doc), session (s), sessionSource (s, inserts)
{
    sessionSource.setLevels (&levels);
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
    monitorInput.setSize (2 * SessionMixer::Live::maxTracks, juce::jmax (8192, 4 * blockSize));
    monitorFrames = 0;
    transport.prepareToPlay (blockSize, sampleRate);
}

void DocumentPlayerProcessor::setMonitoredTracks (const std::vector<MonitoredTrack>& tracks)
{
    const juce::SpinLock::ScopedLockType lock (monitoredLock);
    numMonitored = 0;

    for (auto& t : tracks)
        if (t.trackId != 0 && (t.numInputs == 1 || t.numInputs == 2) && numMonitored < (int) monitored.size())
            monitored[(size_t) numMonitored++] = t;
}

void DocumentPlayerProcessor::setMonitorInput (const float* const* inputs, int numInputs, int numFrames)
{
    // Pick up a new list of tracks when the message thread is not changing it.
    if (const juce::SpinLock::ScopedTryLockType lock (monitoredLock); lock.isLocked())
    {
        monitorTracks = monitored;
        numMonitorTracks = numMonitored;
    }

    monitorFrames = numFrames;
    monitorLayout.count = 0;

    if (inputs == nullptr || numFrames > monitorInput.getNumSamples())
        return;

    for (int i = 0; i < numMonitorTracks; ++i)
    {
        const auto& t = monitorTracks[(size_t) i];

        if (t.firstInput < 0 || t.firstInput + t.numInputs > numInputs)
            continue;

        const auto channel = 2 * monitorLayout.count;

        for (int c = 0; c < t.numInputs; ++c)
            monitorInput.copyFrom (channel + c, 0, inputs[t.firstInput + c], numFrames);

        monitorLayout.inputs[(size_t) monitorLayout.count++] = { t.trackId, channel, t.numInputs };
    }
}

void DocumentPlayerProcessor::releaseResources()
{
    transport.releaseResources();
}

void DocumentPlayerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const auto numFrames = buffer.getNumSamples();
    const auto monitoring = mode == Mode::session && monitorLayout.count > 0 && monitorFrames == numFrames
                            && juce::exactlyEqual (sourceRate, deviceRate);
    sessionSource.feedLive (monitoring ? monitorLayout : SessionMixer::Live(), monitorInput, numFrames);
    monitorFrames = 0;

    juce::AudioSourceChannelInfo info (&buffer, 0, numFrames);
    const auto playing = transport.isPlaying();
    const auto blocksRead = sessionSource.getBlocksRead();
    transport.getNextAudioBlock (info);

    // Stopped (the transport read nothing and played silence): the input plays on its own.
    if (monitoring && ! playing && sessionSource.getBlocksRead() == blocksRead)
        sessionSource.renderLive (info);

    if (buffer.getNumChannels() >= 4)
    {
        buffer.clear (2, 0, numFrames);
        buffer.clear (3, 0, numFrames);

        if (playing && mode == Mode::session && juce::exactlyEqual (sourceRate, deviceRate))
            addClicks (buffer, playedFrame.load(), numFrames);
    }

    if (playing)
        playedFrame += numFrames;
}

void DocumentPlayerProcessor::addClicks (juce::AudioBuffer<float>& buffer, juce::int64 firstFrame, int numFrames)
{
    if (const auto v = session.getVersion(); clickState == nullptr || v != clickVersion)
    {
        clickState = session.getState();
        clickVersion = v;
    }

    const auto on = metronome.load();
    const auto until = countInEnd.load();
    const auto [loopFrom, loopTo] = sessionSource.getLoop();
    const auto tempo = clickState->tempo;
    const auto perBar = juce::jmax (1, clickState->beatsPerBar);
    auto* left = buffer.getWritePointer (2);
    auto* right = buffer.getWritePointer (3);

    for (int i = 0; i < numFrames; ++i)
    {
        // The played frame counts on through a loop; the click follows the session frame.
        auto frame = firstFrame + i;

        if (loopTo > loopFrom && frame >= loopTo)
            frame = loopFrom + (frame - loopFrom) % (loopTo - loopFrom);

        if (frame < 0 || ! (on || frame < until))
            continue;

        const auto beat = Metronome::beatAt (frame, tempo, sourceRate);
        left[i] = right[i] = Metronome::sample (frame - Metronome::beatFrame (beat, tempo, sourceRate), beat % perBar == 0, sourceRate);
    }
}

//==============================================================================
/** Runs before the player in each device cycle and hands it the input to monitor. */
class AudioEngine::MonitorTap final : public juce::AudioIODeviceCallback
{
public:
    explicit MonitorTap (DocumentPlayerProcessor& p) : player (p) {}

    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputs[ch], numSamples);

        player.setMonitorInput (inputs, numInputs, numSamples);
    }

    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}

private:
    DocumentPlayerProcessor& player;
};

//==============================================================================
/** Passes the mix through on its way to the outputs and keeps its peaks, for the master meter. */
class AudioEngine::MasterMeter final : public juce::AudioProcessor
{
public:
    MasterMeter() : AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true)
                                                     .withOutput ("Out", juce::AudioChannelSet::stereo(), true)) {}

    std::pair<float, float> take()                { return { peaks[0].exchange (0.0f), peaks[1].exchange (0.0f) }; }

    const juce::String getName() const override   { return "Master Meter"; }
    void prepareToPlay (double, int) override     {}
    void releaseResources() override              {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
        {
            const auto range = juce::FloatVectorOperations::findMinAndMax (buffer.getReadPointer (ch), buffer.getNumSamples());
            const auto peak = juce::jmax (-range.getStart(), range.getEnd());
            auto current = peaks[ch].load();

            while (peak > current && ! peaks[ch].compare_exchange_weak (current, peak))
            {
            }
        }
    }

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
    std::atomic<float> peaks[2] { { 0.0f }, { 0.0f } };
};

std::pair<float, float> AudioEngine::takeMasterPeaks()
{
    return static_cast<MasterMeter*> (meterNode->getProcessor())->take();
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
    meterNode = graph.addNode (std::make_unique<MasterMeter>());
    recorder = std::make_unique<Recorder> (player->getPlayedFrame());
    monitorTap = std::make_unique<MonitorTap> (*player);

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

    for (auto& [input, extra] : extraRecorders)
        deviceManager.removeAudioCallback (extra.get());
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

    for (auto& [input, extra] : extraRecorders)
        deviceManager.addAudioCallback (extra.get());

    deviceOpen = true;
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
    const auto names = getDeviceInputChannelNames();
    numChannels = juce::jlimit (0, 2, juce::jmin (numChannels, names.size() - firstChannel));
    inputFirstChannel = juce::jmax (0, firstChannel);
    inputNumChannels = juce::jmax (0, numChannels);

    if (inputNumChannels > 0)
        recorder->setInput (inputFirstChannel, inputNumChannels);

    return applyInputChannels();
}

juce::String AudioEngine::applyInputChannels()
{
    // On: the recording input and every extra input, and nothing else.
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.useDefaultInputChannels = false;
    setup.inputChannels.clear();
    const auto numDeviceInputs = getDeviceInputChannelNames().size();

    if (inputNumChannels > 0)
        setup.inputChannels.setRange (inputFirstChannel, inputNumChannels, true);

    for (auto& [input, extra] : extraRecorders)
        setup.inputChannels.setRange (input.first, input.channels, true);

    setup.inputChannels.setRange (numDeviceInputs, juce::jmax (0, setup.inputChannels.getHighestBit() + 1 - numDeviceInputs), false);

    if (! setup.inputChannels.isZero())
        setup.inputDeviceName = inputDeviceNameFor (setup);

    auto error = deviceManager.setAudioDeviceSetup (setup, true);

    // A setup that needed no restart does not call the recorders back, so update them here too.
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const auto active = device->getActiveInputChannels();
        appliedInputs = active;
        recorder->configure (device->getCurrentSampleRate(), inputNumChannels > 0 ? active : juce::BigInteger());

        for (auto& [input, extra] : extraRecorders)
            extra->configure (device->getCurrentSampleRate(), active);
    }

    sendChangeMessage();
    return error;
}

juce::String AudioEngine::setTrackInputs (const std::vector<TrackInput>& inputs)
{
    // The distinct inputs that are not the recording input. This is called often, so it returns
    // before asking the device anything when they are what they were.
    const TrackInput main { inputNumChannels > 0 ? inputFirstChannel : -1, inputNumChannels };
    std::vector<TrackInput> requested;

    for (auto& input : inputs)
        if (! input.isDefault() && input != main && std::find (requested.begin(), requested.end(), input) == requested.end())
            requested.push_back (input);

    std::sort (requested.begin(), requested.end(), [] (auto& a, auto& b) { return std::tie (a.first, a.channels) < std::tie (b.first, b.channels); });

    // After the device changed what is on (another device, Audio Settings), check them again.
    const auto stale = ! trackInputsCurrent;
    trackInputsCurrent = true;

    if (! stale && requested == requestedInputs && requestedMain == main)
        return {};

    requestedInputs = requested;
    requestedMain = main;
    const auto numDeviceInputs = getDeviceInputChannelNames().size();
    std::vector<TrackInput> wanted, current;

    for (auto& input : requested)
        if (input.first + input.channels <= numDeviceInputs)
            wanted.push_back (input);

    for (auto& [input, extra] : extraRecorders)
        current.push_back (input);

    if (wanted == current)
        return stale ? applyInputChannels() : juce::String();

    for (auto& [input, extra] : extraRecorders)
    {
        jassert (! extra->isRecording());

        if (deviceOpen)
            deviceManager.removeAudioCallback (extra.get());
    }

    extraRecorders.clear();

    for (auto& input : wanted)
    {
        auto extra = std::make_unique<Recorder> (player->getPlayedFrame());
        extra->setInput (input.first, input.channels);

        // Added after the player (and the first recorder), so it too sees what was just played.
        if (deviceOpen)
            deviceManager.addAudioCallback (extra.get());

        extraRecorders.emplace_back (input, std::move (extra));
    }

    return applyInputChannels();
}

Recorder* AudioEngine::getRecorderFor (const TrackInput& input)
{
    if (input.isDefault() || (input.first == inputFirstChannel && input.channels == inputNumChannels))
        return inputNumChannels > 0 ? recorder.get() : nullptr;

    for (auto& [extraInput, extra] : extraRecorders)
        if (extraInput == input)
            return extra.get();

    return nullptr;
}

std::vector<Recorder*> AudioEngine::getRecorders()
{
    std::vector<Recorder*> all { recorder.get() };

    for (auto& [input, extra] : extraRecorders)
        all.push_back (extra.get());

    return all;
}

void AudioEngine::setMonitoredTracks (const std::vector<std::pair<juce::uint32, TrackInput>>& tracks)
{
    std::vector<DocumentPlayerProcessor::MonitoredTrack> monitored;

    for (auto& [trackId, input] : tracks)
        if (auto* r = getRecorderFor (input); r != nullptr && r->isInputActive())
            monitored.push_back ({ trackId, r->getMappedInput().first, r->getMappedInput().second });

    player->setMonitoredTracks (monitored);
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

    // Device, sample rate, buffer size or active channels changed. If the inputs on are not the
    // ones last set, the track inputs are checked again.
    if (auto* device = deviceManager.getCurrentAudioDevice(); device == nullptr || device->getActiveInputChannels() != appliedInputs)
        trackInputsCurrent = false;

    rebuildConnections();
    sendChangeMessage();
}

void AudioEngine::rebuildConnections()
{
    for (auto& c : graph.getConnections())
        graph.removeConnection (c, juce::AudioProcessorGraph::UpdateKind::none);

    // Chain: player -> each rack slot in order -> master meter -> output. Bypassed slots stay in
    // the chain; the graph calls their bypass processing so latency compensation keeps working.
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

    // Then the master meter, which passes the mix on as it is.
    connectStereo (source, meterNode.get(), 0);
    source = meterNode.get();

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

            // The metronome's click goes to the same pair past the rack; the graph delays it by the
            // rack's latency, so it stays in time with the music.
            for (int ch = 0; ch < 2; ++ch)
                graph.addConnection ({ { playerNode->nodeID, 2 + ch }, { outputNode->nodeID, graphChannel + ch } },
                                     juce::AudioProcessorGraph::UpdateKind::none);

            // A pair whose second channel is inactive (or a mono device) still gets the left side.
            if (! active[outputPairFirstChannel + 1])
            {
                graph.removeConnection ({ { source->nodeID, 1 }, { outputNode->nodeID, graphChannel + 1 } },
                                        juce::AudioProcessorGraph::UpdateKind::none);
                graph.removeConnection ({ { playerNode->nodeID, 3 }, { outputNode->nodeID, graphChannel + 1 } },
                                        juce::AudioProcessorGraph::UpdateKind::none);
            }
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
