// Before JUCE's headers: COM, for asking Windows' audio system from a thread of our own.
#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
 #include <objbase.h>
#endif

#include "AudioEngine.h"
#include "RackCopy.h"
#include "Surround.h"
#include "Analysis.h"

#include "BuiltInEffects.h"
#include "AudioFileIO.h"
#include "Metronome.h"
#include "SessionMixer.h"

//==============================================================================
juce::int64 DocumentSource::getTotalLength() const
{
    const auto frames = document.getSnapshot()->getNumFrames();
    const juce::SpinLock::ScopedLockType lock (standInLock);
    return (standIn != nullptr ? juce::jmax (frames, standIn->getNumFrames()) : frames) + tail.load();
}

void DocumentSource::setStandIn (AudioDocument::Snapshot s)
{
    AudioDocument::Snapshot old;   // let go of after the lock, not under it
    {
        const juce::SpinLock::ScopedLockType lock (standInLock);
        old = std::exchange (standIn, std::move (s));
        standInSet = standIn != nullptr;
        ++standInVersion;
    }
}

void DocumentSource::getNextAudioBlock (const juce::AudioSourceChannelInfo& info)
{
    // Edits swap the snapshot; take a reference to the new one only when the version changes.
    if (const auto v = document.getVersion(); snapshot == nullptr || v != snapshotVersion)
    {
        snapshot = document.getSnapshot();
        snapshotVersion = v;
    }

    // A stand-in plays in place of the document while there is one (taken when the lock is free,
    // else on a later block).
    if (const auto sv = standInVersion.load(); sv != takenStandIn)
    {
        const juce::SpinLock::ScopedTryLockType lock (standInLock);

        if (lock.isLocked())
        {
            heldStandIn = standIn;
            takenStandIn = sv;
        }
    }

    played = heldStandIn != nullptr ? heldStandIn : snapshot;

    auto& buffer = *info.buffer;
    auto start = position.load();
    const auto from = loopStart.load(), to = loopEnd.load();

    // A loop: up to its end, then back to its start, as often as the block needs.
    if (to > from && start < to)
    {
        for (int done = 0; done < info.numSamples;)
        {
            const auto n = (int) juce::jmin ((juce::int64) (info.numSamples - done), to - start);
            readRun (buffer, info.startSample + done, start, n);
            done += n;
            start += n;

            if (start >= to)
                start = from;
        }

        position = start;
    }
    else
    {
        readRun (buffer, info.startSample, start, info.numSamples);
        position = start + info.numSamples;
    }

    // Mono documents play on both sides.
    if (played->getNumChannels() == 1)
        for (int ch = 1; ch < buffer.getNumChannels(); ++ch)
            buffer.copyFrom (ch, info.startSample, buffer, 0, info.startSample, info.numSamples);
}

void DocumentSource::readRun (juce::AudioBuffer<float>& buffer, int destStart, juce::int64 start, int numFrames)
{
    const auto stop = stopFrame.load();
    auto count = numFrames;

    if (stop >= 0)
        count = (int) juce::jlimit ((juce::int64) 0, (juce::int64) numFrames, stop - start);

    if (played->getNumChannels() > 2)
        readWide (buffer, destStart, start, count);
    else
        played->read (buffer, destStart, start, count);

    if (count < numFrames)
        buffer.clear (destStart + count, numFrames - count);
}

void DocumentSource::prepareToPlay (int blockSize, double)
{
    wide.setSize (Surround::maxPlayedChannels, juce::jmax (16384, 4 * blockSize));
}

void DocumentSource::readWide (juce::AudioBuffer<float>& buffer, int destStart, juce::int64 start, int numFrames)
{
    const auto first = separateFirst.load();
    const auto channels = juce::jmin (played->getNumChannels(), Surround::maxPlayedChannels);

    for (int done = 0; done < numFrames;)
    {
        const auto at = destStart + done;
        const auto n = juce::jmin (numFrames - done, wide.getNumSamples());

        if (n <= 0)   // not prepared: silence
        {
            buffer.clear (at, numFrames - done);
            return;
        }

        buffer.clear (at, n);

        played->read (wide, 0, start + done, n);

        if (first < 0 && Surround::isSurround (played->getNumChannels()) && buffer.getNumChannels() >= 2)
        {
            Surround::foldDown (wide.getArrayOfReadPointers(), played->getNumChannels(), buffer.getWritePointer (0, at), buffer.getWritePointer (1, at), n);
        }
        else
        {
            for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
                buffer.copyFrom (ch, at, wide, ch, 0, n);

            if (first >= 0)
                for (int ch = 2; ch < channels && first + ch - 2 < buffer.getNumChannels(); ++ch)
                    buffer.copyFrom (first + ch - 2, at, wide, ch, 0, n);
        }

        done += n;
    }
}

//==============================================================================
void SessionSource::prepareToPlay (int blockSize, double)
{
    // Room for the largest block the transport's resampler is likely to ask for, so the mixer
    // does not allocate on the audio thread.
    const auto frames = juce::jmax (8192, 4 * blockSize);
    buffers.reserve (frames);
    scrubBuffers.reserve (scrubChunk);
    // Room for the scrubber's widest window (four times normal speed, the audio at up to four
    // times the device's rate) twice over, so what is kept never has to be dropped mid-window.
    scrubMix.setSize (2, juce::jmax (scrubChunk, 4 * (int) (Scrubber::fastest * 4.0 * blockSize + 8.0)));
    scrubTo = -1;
    scrubBuffers.surroundChannel = -1;   // folded down
    scrubBuffers.interfaceChannel = -1;  // the mix alone
    liveBlock.setSize (2 * SessionMixer::Live::maxTracks, frames);
    liveRing.setSize (2 * SessionMixer::Live::maxTracks, juce::nextPowerOfTwo (2 * frames));
    liveRead = liveCount = 0;
    nextFrame = -1;
}

void SessionSource::refreshState()
{
    buffers.surroundChannel = surroundChannel.load();

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

void SessionSource::scrub (Scrubber& scrubber, double deviceRate, juce::AudioBuffer<float>& buffer, int numFrames, bool effects)
{
    refreshState();

    if (state == nullptr)
        return;

    struct Mix final : Scrubber::Source
    {
        Mix (SessionSource& o, bool fx) : owner (o), session (*o.state), withEffects (fx) {}

        juce::int64 getNumFrames() const override   { return session.getLength(); }
        int getNumChannels() const override         { return 2; }

        /** Frames [from, from + count) of the mix into dest from destStart, in pieces the mixer has
            room for, so it doesn't allocate. */
        void mix (juce::int64 from, juce::AudioBuffer<float>& dest, int destStart, int count)
        {
            for (int done = 0; done < count;)
            {
                const auto n = juce::jmin (scrubChunk, count - done);
                SessionMixer::render (session, from + done, dest, destStart + done, n, owner.scrubBuffers,
                                      withEffects ? &owner.inserts : nullptr);
                done += n;
            }
        }

        void read (juce::AudioBuffer<float>& into, juce::int64 first, int count) override
        {
            if (! withEffects)
            {
                mix (first, into, 0, count);
                return;
            }

            auto& kept = owner.scrubMix;
            const auto capacity = kept.getNumSamples();
            auto& from = owner.scrubFrom;
            auto& to = owner.scrubTo;

            if (count > capacity)   // wider than prepared for: as without effects
            {
                mix (first, into, 0, count);
                to = -1;
                return;
            }

            // Back before what is kept, or too far ahead of it: the effects primed and warmed up
            // on the mix before the window, which is then rendered on from there.
            if (to < from || first < from || first > to + scrubWarmUp)
            {
                SessionMixer::prime (session, first - scrubWarmUp, owner.scrubBuffers, owner.inserts);

                for (int done = 0; done < scrubWarmUp;)
                {
                    const auto n = juce::jmin (capacity, scrubWarmUp - done);
                    mix (first - scrubWarmUp + done, kept, 0, n);
                    done += n;
                }

                from = to = first;
            }

            // Only as much kept as fits: the oldest frames go.
            const auto end = first + count;

            if (end - from > capacity)
            {
                const auto drop = (int) (end - from - capacity);

                for (int ch = 0; ch < kept.getNumChannels(); ++ch)
                    std::memmove (kept.getWritePointer (ch), kept.getReadPointer (ch) + drop, sizeof (float) * (size_t) (to - from - drop));

                from += drop;
            }

            // Rendered on from where it was, through the effects unbroken.
            if (end > to)
            {
                mix (to, kept, (int) (to - from), (int) (end - to));
                to = end;
            }

            for (int ch = 0; ch < juce::jmin (2, into.getNumChannels()); ++ch)
                into.copyFrom (ch, 0, kept, ch, (int) (first - from), count);
        }

        SessionSource& owner;
        const SessionState& session;
        const bool withEffects;
    };

    // An edit since, or playing (the effects run from elsewhere), and what is kept is stale.
    if (std::exchange (scrubState, state.get()) != state.get())
        scrubTo = -1;

    Mix mix (*this, effects);
    scrubber.render (mix, state->sampleRate, deviceRate, buffer, 0, numFrames);

    // The effects were left where the scrub was: playing after it primes them again.
    if (effects)
        nextFrame = -1;
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
    scrubTo = -1;
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

    scrubTo = -1;   // the effects play from here now, not from where a scrub left them

    SessionMixer::render (*state, start, *info.buffer, destStart, count, buffers, &inserts, takeLive (numFrames, true));

    if (count < numFrames)
        info.buffer->clear (destStart + count, numFrames - count);

    nextFrame = start + numFrames;
}

//==============================================================================
DocumentPlayerProcessor::DocumentPlayerProcessor (AudioDocument& doc, Session& s, SessionMixer::Inserts& inserts)
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Interface", juce::AudioChannelSet::discreteChannels (SessionTrack::maxDeviceOutputs), true)
                                       .withOutput ("Click", juce::AudioChannelSet::stereo(), true)),
      document (doc), session (s), sessionSource (s, inserts)
{
    sessionSource.setLevels (&levels);
    resetSource();
}

DocumentPlayerProcessor::~DocumentPlayerProcessor()
{
    transport.setSource (nullptr);
    auditioner.setSource (nullptr);
}

void DocumentPlayerProcessor::audition (std::unique_ptr<juce::AudioFormatReader> reader)
{
    auditioner.stop();
    auditioner.setSource (nullptr);
    auditionSource.reset();

    if (reader == nullptr)
        return;

    if (! auditionThread.isThreadRunning())
        auditionThread.startThread();

    const auto fileRate = reader->sampleRate;
    auditionSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);
    auditioner.setSource (auditionSource.get(), 32768, &auditionThread, fileRate, 2);
    auditioner.start();
}

double DocumentPlayerProcessor::modeRate() const
{
    return mode == Mode::session ? session.getSampleRate() : document.getSampleRate();
}

void DocumentPlayerProcessor::setSeparateOutputs (int firstChannel)
{
    source.setSeparateOutputs (firstChannel);
    sessionSource.setSurroundChannel (firstChannel);

    // Separate outputs need the interface channels through the transport; the mix alone doesn't.
    if ((firstChannel >= 0) != (separateOutputs >= 0))
    {
        separateOutputs = firstChannel;
        resetSource();
    }

    separateOutputs = firstChannel;
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
    // rate to the device's, so the interface keeps its own rate. A session plays the mix and what
    // goes out of each interface pair; a document, the mix alone.
    sourceRate = modeRate();
    updateTail();
    transport.setSource (mode == Mode::session ? static_cast<juce::PositionableAudioSource*> (&sessionSource) : &source,
                         0, nullptr, sourceRate, mode == Mode::session || separateOutputs >= 0 ? clickChannel : 2);
    transport.setPosition (0.0);
    stopAt = -1;
    source.setStopFrame (-1);
    sessionSource.setStopFrame (-1);
    clearLoop();
}

void DocumentPlayerProcessor::play (juce::int64 startFrame, juce::int64 stopFrame)
{
    transport.stop();
    setStopFrame (stopFrame);
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
    playStart = frame;
}

void DocumentPlayerProcessor::setOutputLatency (int deviceFrames)
{
    outputLatency = juce::jmax (0, deviceFrames);
    updateTail();
}

juce::int64 DocumentPlayerProcessor::latencyFrames() const
{
    const auto latency = (juce::int64) outputLatency.load();
    return deviceRate > 0.0 && sourceRate > 0.0 ? (juce::int64) std::llround ((double) latency * sourceRate / deviceRate) : latency;
}

void DocumentPlayerProcessor::updateTail()
{
    const auto tail = latencyFrames();
    source.setTail (tail);
    sessionSource.setTail (tail);
}

juce::int64 DocumentPlayerProcessor::getHeardFrame() const
{
    const auto start = playStart.load();

    // Frames played since the start, in the source's frames (the played frame counts the device's).
    auto elapsed = playedFrame.load() - start;

    if (deviceRate > 0.0 && sourceRate > 0.0)
        elapsed = (juce::int64) std::llround ((double) elapsed * sourceRate / deviceRate);

    // While the effects fill with the first frames, what sounds is from before the start.
    const auto heard = juce::jmax ((juce::int64) 0, elapsed - latencyFrames());
    auto frame = start + heard;

    // Round the loop, as the source plays it (from its end back to its start, once it gets there).
    const auto [from, to] = mode == Mode::session ? sessionSource.getLoop() : source.getLoop();

    if (to > from && start < to && frame >= to)
        frame = from + (frame - from) % (to - from);

    return frame;
}

void DocumentPlayerProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    deviceRate = sampleRate;
    updateTail();
    monitorInput.setSize (2 * SessionMixer::Live::maxTracks, juce::jmax (8192, 4 * blockSize));
    monitorFrames = 0;
    transport.prepareToPlay (blockSize, sampleRate);
    auditioner.prepareToPlay (blockSize, sampleRate);
    auditionBuffer.setSize (2, juce::jmax (8192, 4 * blockSize));
    scrubber.prepare (blockSize);
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
    auditioner.releaseResources();
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

    // Scrubbing a stopped session: its mix's sound, through its effects unless they are playing
    // the input monitored.
    if (mode == Mode::session && ! playing && scrubber.isSounding())
        sessionSource.scrub (scrubber, deviceRate, buffer, numFrames, ! monitoring);

    // Scrubbing a stopped document: its sound as the mix.
    if (mode == Mode::document && ! playing && scrubber.isSounding())
    {
        if (const auto v = document.getVersion(); scrubSnapshot == nullptr || v != scrubVersion)
        {
            scrubSnapshot = document.getSnapshot();
            scrubVersion = v;
        }

        if (scrubSnapshot != nullptr)
            scrubber.render (*scrubSnapshot, sourceRate, deviceRate, buffer, 0, numFrames);
    }

    // A document has nothing for the interface pairs (the transport read the mix alone), unless
    // its channels past the first two play out of their own outputs.
    if (mode != Mode::session && separateOutputs < 0)
        for (int ch = 2; ch < juce::jmin (clickChannel, buffer.getNumChannels()); ++ch)
            buffer.clear (ch, 0, numFrames);

    if (buffer.getNumChannels() >= clickChannel + 2)
    {
        buffer.clear (clickChannel, 0, numFrames);
        buffer.clear (clickChannel + 1, 0, numFrames);

        if (playing && mode == Mode::session && juce::exactlyEqual (sourceRate, deviceRate))
            addClicks (buffer, playedFrame.load(), numFrames);

        if (auditioner.isPlaying() && numFrames <= auditionBuffer.getNumSamples())
        {
            juce::AudioSourceChannelInfo heard (&auditionBuffer, 0, numFrames);
            auditioner.getNextAudioBlock (heard);
            buffer.addFrom (clickChannel, 0, auditionBuffer, 0, 0, numFrames);
            buffer.addFrom (clickChannel + 1, 0, auditionBuffer, 1, 0, numFrames);
        }
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
    const auto map = clickState->tempoMap();
    auto* left = buffer.getWritePointer (clickChannel);
    auto* right = buffer.getWritePointer (clickChannel + 1);

    for (int i = 0; i < numFrames; ++i)
    {
        // The played frame counts on through a loop; the click follows the session frame.
        auto frame = firstFrame + i;

        if (loopTo > loopFrom && frame >= loopTo)
            frame = loopFrom + (frame - loopFrom) % (loopTo - loopFrom);

        if (frame < 0 || ! (on || frame < until))
            continue;

        const auto beat = map.beatAt (frame);
        left[i] = right[i] = Metronome::sample (frame - map.beatFrame (beat), map.barAndBeat (beat).second == 0, sourceRate);
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

    std::pair<float, float> take (int reader)     { return { peaks[reader][0].exchange (0.0f), peaks[reader][1].exchange (0.0f) }; }

    void copyRecent (juce::AudioBuffer<float>& dest) const   { tap.copyRecent (dest); }

    const juce::String getName() const override   { return "Master Meter"; }
    void prepareToPlay (double, int) override     {}
    void releaseResources() override              {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        tap.write (buffer);

        for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
        {
            const auto range = juce::FloatVectorOperations::findMinAndMax (buffer.getReadPointer (ch), buffer.getNumSamples());
            const auto peak = juce::jmax (-range.getStart(), range.getEnd());

            for (auto& reader : peaks)
            {
                auto current = reader[ch].load();

                while (peak > current && ! reader[ch].compare_exchange_weak (current, peak))
                {
                }
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
    Analysis::OutputTap tap;
    std::atomic<float> peaks[3][2] { { { 0.0f }, { 0.0f } }, { { 0.0f }, { 0.0f } }, { { 0.0f }, { 0.0f } } };   // [reader][channel]
};

std::pair<float, float> AudioEngine::takeMasterPeaks (MasterReader reader)
{
    return static_cast<MasterMeter*> (meterNode->getProcessor())->take ((int) reader);
}

void AudioEngine::copyRecentOutput (juce::AudioBuffer<float>& dest) const
{
    static_cast<MasterMeter*> (meterNode->getProcessor())->copyRecent (dest);
}

double AudioEngine::getOutputSampleRate() const
{
    auto* device = deviceManager.getCurrentAudioDevice();
    return device != nullptr ? device->getCurrentSampleRate() : 0.0;
}

//==============================================================================
AudioEngine::AudioEngine()
{
    AudioFileIO::registerFormats (formatManager);
    juce::addDefaultFormatsToManager (pluginFormatManager);
    pluginFormatManager.addFormat (BuiltInEffects::createFormat());
    trackEffects.prepare (session.getSampleRate());
    trackEffects.makeCopy = [this] (const juce::PluginDescription& d)
    {
        juce::String error;
        return pluginFormatManager.createPluginInstance (d, session.getSampleRate(), TrackEffects::maxBlock, error);
    };

    graph.addListener (this);
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
    graph.removeListener (this);
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
    auto error = deviceManager.initialise (0, 256, namingADevice (savedState), true);

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

const juce::XmlElement* AudioEngine::namingADevice (const juce::XmlElement* savedState)
{
    if (savedState == nullptr)
        return nullptr;

    const auto names = savedState->getStringAttribute ("audioOutputDeviceName") + savedState->getStringAttribute ("audioInputDeviceName");
    return names.trim().isEmpty() ? nullptr : savedState;
}

juce::String AudioEngine::probeDevice (const juce::XmlElement* savedState)
{
   #if JUCE_WINDOWS
    // WASAPI is COM, which each thread that uses it starts for itself. Declared first, so it ends
    // after the device objects below are gone.
    struct ComForThisThread
    {
        ComForThisThread() : started (SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED))) {}
        ~ComForThisThread() { if (started) CoUninitialize(); }
        const bool started;
    } com;
   #endif

    std::unique_ptr<juce::AudioIODeviceType> type;
   #if JUCE_MAC
    type.reset (juce::AudioIODeviceType::createAudioIODeviceType_CoreAudio());
   #elif JUCE_WINDOWS
    // The type the saved setup used, or Windows Audio (shared mode), which the device manager opens first.
    const auto typeName = savedState != nullptr ? savedState->getStringAttribute ("deviceType") : juce::String();

    if (typeName == "DirectSound")
        type.reset (juce::AudioIODeviceType::createAudioIODeviceType_DirectSound());
    else if (typeName == "Windows Audio (Exclusive Mode)")
        type.reset (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::exclusive));
    else if (typeName == "Windows Audio (Low Latency Mode)")
        type.reset (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::sharedLowLatency));
    else if (typeName.isEmpty() || typeName == "Windows Audio")
        type.reset (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));
   #endif

    if (type == nullptr)
        return {};

    type->scanForDevices();
    const auto names = type->getDeviceNames (false);
    auto name = savedState != nullptr ? savedState->getStringAttribute ("audioOutputDeviceName") : juce::String();

    if (! names.contains (name))
        name = names[type->getDefaultDeviceIndex (false)];

    if (name.isEmpty())
        return {};

    // Making the device, and asking it what it offers, is where a stuck audio service was seen to wait.
    const std::unique_ptr<juce::AudioIODevice> device (type->createDevice (name, {}));

    if (device == nullptr)
        return {};

    device->getAvailableSampleRates();
    device->getAvailableBufferSizes();
    return name;
}

bool AudioEngine::auditionFile (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader;

    if (file.existsAsFile())
        reader = std::unique_ptr<juce::AudioFormatReader> (formatManager.createReaderFor (file));

    const auto readable = reader != nullptr && reader->lengthInSamples > 0 && reader->sampleRate > 0.0;
    player->audition (readable ? std::move (reader) : nullptr);
    return readable;
}

juce::String AudioEngine::openFile (const juce::File& file)
{
    return adoptFile (file, AudioFileIO::load (file, formatManager));
}

juce::String AudioEngine::adoptFile (const juce::File& file, AudioFileIO::Loaded loaded)
{
    if (loaded.error.isNotEmpty())
        return loaded.error;

    player->pause();
    document.reset (std::move (loaded.snapshot), loaded.sampleRate, file, loaded.bitDepth, std::move (loaded.markers), std::move (loaded.info));
    player->resetSource();
    sendChangeMessage();
    return {};
}

void AudioEngine::newDocument (int channels, double sampleRate, int bitDepth, juce::int64 frames)
{
    player->pause();
    document.reset (AudioEdits::blank (channels, frames), sampleRate, {}, bitDepth);
    player->resetSource();
    sendChangeMessage();
}

void AudioEngine::swapDocument (AudioDocument& other)
{
    player->pause();
    document.swapWith (other);
    player->resetSource();
    sendChangeMessage();
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

void AudioEngine::audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details)
{
    // The graph has rebuilt its chain (an effect added, removed or changed its latency): the player
    // shows the heard frame that much behind, and plays on that much past the end.
    if (details.latencyChanged && player != nullptr)
        player->setOutputLatency (graph.getLatencySamples());
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

    useDeviceOutputs();   // a file's extra channels follow the pair
    rebuildConnections();
    sendChangeMessage();
}

juce::BigInteger AudioEngine::deviceOutputsOf (const SessionState& state, int surroundFrom)
{
    juce::BigInteger used;

    if (state.surround && surroundFrom >= 0)
        used.setRange (surroundFrom, state.masterChannels() - 2, true);

    for (auto& t : state.tracks)
        if (t.deviceOutput >= 0 && t.outputBusId == 0 && ! t.isFolder)
            used.setRange (t.deviceOutput, 2, true);

    return used;
}

namespace
{
    /** The graph's output channels are the device's active output channels, in order: a device
        channel's place among them. */
    int graphChannelOf (const juce::BigInteger& active, int deviceChannel)
    {
        int graphChannel = 0;

        for (int before = 0; before < deviceChannel; ++before)
            if (active[before])
                ++graphChannel;

        return graphChannel;
    }

    /** A rack slot that takes the channels past L and R itself, linked (the built-in compressor and limiter). */
    bool linksInRack (const juce::AudioProcessorGraph::Node& slot)
    {
        auto* effect = dynamic_cast<const BuiltInEffects::BuiltInEffect*> (slot.getProcessor());
        return effect != nullptr && effect->linksChannels();
    }

    const RackCopy* asCopy (const juce::AudioProcessorGraph::Node::Ptr& node)
    {
        return node != nullptr ? dynamic_cast<const RackCopy*> (node->getProcessor()) : nullptr;
    }

    /** Where a pair comes out of a node in its chain: a copy's first two outputs, a linking slot's own. */
    int firstOutputOf (const juce::AudioProcessorGraph::Node::Ptr& node, int pair)
    {
        return asCopy (node) != nullptr ? 0 : 2 + 2 * pair;
    }
}

bool AudioEngine::playsThroughRack (int deviceChannel) const
{
    auto* device = deviceManager.getCurrentAudioDevice();
    const auto pair = (deviceChannel - outputPairFirstChannel - 2) / 2;

    if (device == nullptr || deviceChannel < outputPairFirstChannel + 2 || ! juce::isPositiveAndBelow (deviceChannel, SessionTrack::maxDeviceOutputs)
        || ! device->getActiveOutputChannels()[deviceChannel] || pair >= (int) rackCopies.size() || rackCopies[(size_t) pair].empty()
        || rackCopies[(size_t) pair].back() == nullptr)
        return false;

    const auto& last = rackCopies[(size_t) pair].back();
    return graph.isConnected ({ { last->nodeID, firstOutputOf (last, pair) + (deviceChannel - outputPairFirstChannel - 2) % 2 },
                                { outputNode->nodeID, graphChannelOf (device->getActiveOutputChannels(), deviceChannel) } });
}

bool AudioEngine::playsOutOf (int deviceChannel) const
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr || ! juce::isPositiveAndBelow (deviceChannel, SessionTrack::maxDeviceOutputs)
        || ! device->getActiveOutputChannels()[deviceChannel])
        return false;

    return graph.isConnected ({ { playerNode->nodeID, DocumentPlayerProcessor::firstInterfaceChannel + deviceChannel },
                                { outputNode->nodeID, graphChannelOf (device->getActiveOutputChannels(), deviceChannel) } });
}

void AudioEngine::setSurroundOutputs (bool separate)
{
    surroundOutputs = separate;
    useDeviceOutputs();
}

void AudioEngine::useDeviceOutputs()
{
    const auto fileMode = player->getMode() != DocumentPlayerProcessor::Mode::session;
    player->setSeparateOutputs (surroundOutputs ? DocumentPlayerProcessor::firstInterfaceChannel + outputPairFirstChannel + 2 : -1);
    const auto used = fileMode ? Surround::extraOutputs (document.getNumChannels(), outputPairFirstChannel, surroundOutputs)
                               : deviceOutputsOf (*session.getState(), surroundOutputs ? outputPairFirstChannel + 2 : -1);

    if (used == usedDeviceOutputs)
        return;

    usedDeviceOutputs = used;

    // The device only delivers buffers for active channels, so turn on the ones now used (the
    // device change then connects them); those no longer used stay on.
    auto setup = deviceManager.getAudioDeviceSetup();
    const auto numDeviceOutputs = getDeviceOutputChannelNames().size();
    auto changed = false;

    for (int ch = 0; ch < juce::jmin (numDeviceOutputs, SessionTrack::maxDeviceOutputs); ++ch)
    {
        if (used[ch] && ! setup.outputChannels[ch])
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
}

void AudioEngine::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &session || source == &document)
        useDeviceOutputs();

    if (source == &document || source == &session)
    {
        // A conversion (or undoing one), or a new session, changed the rate the player must resample from.
        const auto rate = player->getMode() == DocumentPlayerProcessor::Mode::session ? session.getSampleRate()
                                                                                     : document.getSampleRate();

        if (! juce::exactlyEqual (player->getSourceRate(), rate))
            player->resetSource();

        if (! juce::exactlyEqual (trackEffects.getSampleRate(), session.getSampleRate()))
            trackEffects.prepare (session.getSampleRate());

        updateWideTracks();
        return;
    }

    // The device manager also tells of changes to the lists of devices, which some plug-ins set off
    // many times a second while they are loaded; only a change to the device in use matters here.
    if (const auto now = deviceSignature(); now == lastDeviceSignature)
        return;
    else
        lastDeviceSignature = now;

    // Device, sample rate, buffer size or active channels changed. If the inputs on are not the
    // ones last set, the track inputs are checked again.
    if (auto* device = deviceManager.getCurrentAudioDevice(); device == nullptr || device->getActiveInputChannels() != appliedInputs)
        trackInputsCurrent = false;

    rebuildConnections();
    sendChangeMessage();
}

void AudioEngine::updateWideTracks()
{
    std::map<juce::uint32, int> now;

    for (auto& t : session.getState()->tracks)
        if (t.isWide() && ! t.isFrozen())
            now[t.id] = t.wideChannels();

    if (now == wideTracks)
        return;

    wideTracks = now;

    if (const auto failed = trackEffects.setWideChannels (now); ! failed.isEmpty() && onEffectsNotCopied != nullptr)
        onEffectsNotCopied (failed);
}

juce::String AudioEngine::deviceSignature() const
{
    auto* d = deviceManager.getCurrentAudioDevice();

    if (d == nullptr)
        return "none";

    return d->getTypeName() + "|" + d->getName() + "|" + juce::String (d->getCurrentSampleRate()) + "|"
         + juce::String (d->getCurrentBufferSizeSamples()) + "|" + d->getActiveInputChannels().toString (2) + "|"
         + d->getActiveOutputChannels().toString (2) + "|" + juce::String ((int) d->isOpen()) + juce::String ((int) d->isPlaying());
}

int AudioEngine::extraChannels() const
{
    if (! surroundOutputs)
        return 0;

    if (player->getMode() == DocumentPlayerProcessor::Mode::session)
        return session.getState()->masterChannels() - 2;

    return juce::jlimit (0, 6, document.getNumChannels() - 2);
}

void AudioEngine::updateRackCopies()
{
    const auto extra = extraChannels();
    const auto pairs = (extra + 1) / 2;
    std::vector<std::vector<Node::Ptr>> wanted ((size_t) pairs);
    juce::StringArray failed;

    // A slot that links takes them itself: its main bus as wide as L and R and the rest (stereo
    // again when there are none), changed between blocks.
    for (auto& slot : rack)
        if (linksInRack (*slot))
        {
            auto& p = *slot->getProcessor();
            const auto width = extra > 0 ? juce::AudioChannelSet::discreteChannels (2 + extra) : juce::AudioChannelSet::stereo();

            if (p.getChannelLayoutOfBus (true, 0) != width)
            {
                const juce::ScopedLock between (p.getCallbackLock());
                p.setChannelLayoutOfBus (true, 0, width);
                p.setChannelLayoutOfBus (false, 0, width);
            }
        }

    for (int pair = 0; pair < pairs; ++pair)
        for (auto& slot : rack)
        {
            if (linksInRack (*slot))
            {
                wanted[(size_t) pair].push_back (slot);
                continue;
            }

            // The one already made for this slot, else a new one.
            Node::Ptr found;

            if ((size_t) pair < rackCopies.size())
                for (auto& c : rackCopies[(size_t) pair])
                    if (auto* copy = asCopy (c); copy != nullptr && copy->getOriginal() == slot->getProcessor())
                        found = c;

            if (found == nullptr)
                if (auto* original = dynamic_cast<juce::AudioPluginInstance*> (slot->getProcessor()))
                {
                    juce::String error;

                    if (auto copy = pluginFormatManager.createPluginInstance (original->getPluginDescription(), currentSampleRate(), currentBlockSize(), error))
                        found = graph.addNode (std::make_unique<RackCopy> (slot, std::move (copy)), {}, juce::AudioProcessorGraph::UpdateKind::none);
                    else if (! failed.contains (original->getName()))
                        failed.add (original->getName());
                }

            wanted[(size_t) pair].push_back (found);
        }

    for (auto& copies : rackCopies)
        for (auto& c : copies)
            if (asCopy (c) != nullptr && std::none_of (wanted.begin(), wanted.end(), [&c] (auto& w) { return std::find (w.begin(), w.end(), c) != w.end(); }))
                graph.removeNode (c.get(), juce::AudioProcessorGraph::UpdateKind::none);

    rackCopies = std::move (wanted);

    if (! failed.isEmpty() && onEffectsNotCopied != nullptr)
        juce::MessageManager::callAsync ([this, failed, isAlive = alive]
        {
            if (*isAlive && onEffectsNotCopied != nullptr)
                onEffectsNotCopied (failed);
        });
}

int AudioEngine::getNumRackCopies() const
{
    int count = 0;

    for (auto& copies : rackCopies)
        count += (int) std::count_if (copies.begin(), copies.end(), [] (auto& c) { return asCopy (c) != nullptr; });

    return count;
}

int AudioEngine::getNumRackSlotsLinking() const
{
    auto count = 0;

    for (auto& slot : rack)
        if (linksInRack (*slot) && slot->getProcessor()->getMainBusNumInputChannels() > 2)
            ++count;

    return count;
}

void AudioEngine::refreshEffectCopies (const juce::AudioProcessor& p)
{
    trackEffects.refreshCopies (p);
    auto any = false;

    for (auto& copies : rackCopies)
        for (auto& c : copies)
            if (auto* copy = asCopy (c); copy != nullptr && copy->getOriginal() == &p)
            {
                graph.removeNode (c.get(), juce::AudioProcessorGraph::UpdateKind::none);
                c = nullptr;
                any = true;
            }

    // Made again, with its state as it is now.
    if (any)
        rebuildConnections();
}

void AudioEngine::rebuildConnections()
{
    for (auto& c : graph.getConnections())
        graph.removeConnection (c, juce::AudioProcessorGraph::UpdateKind::none);

    updateRackCopies();

    // Chain: player -> the effect being previewed, if any -> each rack slot in order -> master
    // meter -> output. Bypassed slots stay in the chain; the graph calls their bypass processing
    // so latency compensation keeps working.
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

    if (previewNode != nullptr)
    {
        connectStereo (source, previewNode.get(), 0);
        source = previewNode.get();
    }

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
            const auto graphChannel = graphChannelOf (active, outputPairFirstChannel);

            connectStereo (source, outputNode.get(), graphChannel);

            // The metronome's click goes to the same pair past the rack; the graph delays it by the
            // rack's latency, so it stays in time with the music.
            for (int ch = 0; ch < 2; ++ch)
                graph.addConnection ({ { playerNode->nodeID, DocumentPlayerProcessor::clickChannel + ch }, { outputNode->nodeID, graphChannel + ch } },
                                     juce::AudioProcessorGraph::UpdateKind::none);

            // A pair whose second channel is inactive (or a mono device) still gets the left side.
            if (! active[outputPairFirstChannel + 1])
            {
                graph.removeConnection ({ { source->nodeID, 1 }, { outputNode->nodeID, graphChannel + 1 } },
                                        juce::AudioProcessorGraph::UpdateKind::none);
                graph.removeConnection ({ { playerNode->nodeID, DocumentPlayerProcessor::clickChannel + 1 }, { outputNode->nodeID, graphChannel + 1 } },
                                        juce::AudioProcessorGraph::UpdateKind::none);
            }
        }

        // The channels past L and R played to their own outputs: two at a time through the rack's
        // copies, or through a slot that links them (its channels after L and R), in the rack's
        // order (an empty rack: straight there); a pair one of whose copies couldn't be made is silent.
        const auto extra = extraChannels();
        const auto throughRack = ! rack.empty();

        for (int pair = 0; throughRack && pair < (int) rackCopies.size(); ++pair)
        {
            const auto first = outputPairFirstChannel + 2 + 2 * pair, count = juce::jmin (2, extra - 2 * pair);
            auto from = playerNode;
            auto fromFirst = DocumentPlayerProcessor::firstInterfaceChannel + first;
            auto complete = true;

            for (auto& copy : rackCopies[(size_t) pair])
            {
                if (copy == nullptr)
                {
                    complete = false;
                    break;
                }

                const auto toFirst = firstOutputOf (copy, pair);

                for (int ch = 0; ch < (toFirst == 0 ? 2 : count); ++ch)
                    graph.addConnection ({ { from->nodeID, fromFirst + juce::jmin (ch, count - 1) }, { copy->nodeID, toFirst + ch } },
                                         juce::AudioProcessorGraph::UpdateKind::none);

                from = copy;
                fromFirst = toFirst;
            }

            for (int ch = 0; complete && ch < count; ++ch)
                if (first + ch < SessionTrack::maxDeviceOutputs && active[first + ch])
                    graph.addConnection ({ { from->nodeID, fromFirst + ch }, { outputNode->nodeID, graphChannelOf (active, first + ch) } },
                                         juce::AudioProcessorGraph::UpdateKind::none);
        }

        // Tracks and buses playing out of interface pairs: straight from the player to those
        // outputs, past the rack and the master meter; the graph delays them by the rack's latency
        // too. A pair the interface doesn't have stays silent. (Ones on the outputs the channels
        // past L and R use go through the rack's copies with them.)
        for (int ch = 0; ch < SessionTrack::maxDeviceOutputs; ++ch)
        {
            if (! usedDeviceOutputs[ch] || ! active[ch])
                continue;

            if (throughRack && ch >= outputPairFirstChannel + 2 && ch < outputPairFirstChannel + 2 + extra)
                continue;

            graph.addConnection ({ { playerNode->nodeID, DocumentPlayerProcessor::firstInterfaceChannel + ch },
                                   { outputNode->nodeID, graphChannelOf (active, ch) } },
                                 juce::AudioProcessorGraph::UpdateKind::none);
        }
    }

    graph.rebuild();
}

//==============================================================================
void AudioEngine::setPreviewEffect (std::unique_ptr<juce::AudioPluginInstance> effect)
{
    if (previewNode != nullptr)
    {
        if (onPluginAboutToBeRemoved != nullptr)
            onPluginAboutToBeRemoved (previewNode->getProcessor());

        graph.removeNode (previewNode.get());
        previewNode = nullptr;
    }

    if (effect != nullptr)
    {
        effect->setRateAndBufferSizeDetails (currentSampleRate(), currentBlockSize());
        previewNode = graph.addNode (std::move (effect));
    }

    rebuildConnections();
}

juce::AudioPluginInstance* AudioEngine::getPreviewEffect() const
{
    return previewNode != nullptr ? dynamic_cast<juce::AudioPluginInstance*> (previewNode->getProcessor()) : nullptr;
}

void AudioEngine::setPreviewBypassed (bool bypassed)
{
    if (previewNode != nullptr)
        previewNode->setBypassed (bypassed);
}

bool AudioEngine::isPreviewBypassed() const
{
    return previewNode != nullptr && previewNode->isBypassed();
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
                  [this, trackId, description, done = std::move (onDone)] (std::unique_ptr<juce::AudioPluginInstance> instance,
                                                                           const juce::String& error)
                  {
                      if (instance != nullptr)
                      {
                          const auto slotId = trackEffects.add (trackId, std::move (instance));
                          session.markEffectsChanged();

                          if (const auto wide = wideTracks.find (trackId); wide != wideTracks.end() && onEffectsNotCopied != nullptr)
                              for (int i = 0; i < trackEffects.getNumSlots (trackId); ++i)
                                  if (trackEffects.getSlotId (trackId, i) == slotId && trackEffects.getNumCopies (trackId, i) < wide->second / 2 - 1)
                                      onEffectsNotCopied ({ description.name });
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

                        engine.trackEffects.refreshCopies (*plugin);
                    }

                    next (engine, isAlive, q, index + 1, errors, std::move (done));
                });
        }
    };

    Loader::next (*this, alive, queue, 0, {}, std::move (onDone));
}
