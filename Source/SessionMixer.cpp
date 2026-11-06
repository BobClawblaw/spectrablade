#include "SessionMixer.h"

namespace SessionMixer
{
std::pair<float, float> panGains (float pan, int sourceChannels)
{
    pan = juce::jlimit (-1.0f, 1.0f, pan);
    constexpr auto halfPi = juce::MathConstants<float>::halfPi;

    if (sourceChannels == 1)
    {
        const auto angle = (pan + 1.0f) * 0.5f * halfPi;
        return { std::cos (angle), std::sin (angle) };
    }

    return { pan > 0.0f ? std::cos (pan * halfPi) : 1.0f, pan < 0.0f ? std::cos (-pan * halfPi) : 1.0f };
}

void Levels::raise (int track, float left, float right) noexcept
{
    if (! juce::isPositiveAndBelow (track, maxTracks))
        return;

    for (int reader = 0; reader < readers; ++reader)
    {
        const auto first = 2 * (reader * maxTracks + track);

        for (auto [slot, value] : { std::pair { first, left }, std::pair { first + 1, right } })
        {
            auto& peak = peaks[(size_t) slot];
            auto current = peak.load (std::memory_order_relaxed);

            while (value > current && ! peak.compare_exchange_weak (current, value, std::memory_order_relaxed))
            {
            }
        }
    }
}

std::pair<float, float> Levels::take (int track, int reader) noexcept
{
    if (! juce::isPositiveAndBelow (track, maxTracks) || ! juce::isPositiveAndBelow (reader, readers))
        return {};

    const auto slot = (size_t) (2 * (reader * maxTracks + track));
    return { peaks[slot].exchange (0.0f), peaks[slot + 1].exchange (0.0f) };
}

namespace
{
    float peakOf (const juce::AudioBuffer<float>& b, int channel, int numFrames)
    {
        const auto range = juce::FloatVectorOperations::findMinAndMax (b.getReadPointer (channel), numFrames);
        return juce::jmax (-range.getStart(), range.getEnd());
    }

    bool plays (const SessionState& state, const SessionTrack& track)
    {
        // A folder's mute and solo act on its members, as if their own.
        if (track.isFolder || state.isMuted (track))
            return false;

        // Solo safe: played whatever else is soloed (a bus still hears only what plays into it).
        if (! state.anySolo() || state.isSoloed (track) || state.isSoloSafe (track))
            return true;

        // A soloed track or bus brings what it reaches (by a send or an output, through any
        // buses between), and what reaches it.
        if (track.isBus && std::any_of (state.tracks.begin(), state.tracks.end(), [&] (auto& t)
            {
                return &t != &track && state.isSoloed (t) && ! state.isMuted (t) && state.feeds (t, track.id);
            }))
            return true;

        return std::any_of (state.tracks.begin(), state.tracks.end(), [&] (auto& bus)
        {
            return bus.isBus && &bus != &track && state.isSoloed (bus) && state.feeds (track, bus.id);
        });
    }

    void grow (juce::AudioBuffer<float>& b, int frames)
    {
        if (b.getNumChannels() < 2 || b.getNumSamples() < frames)
            b.setSize (2, frames, false, false, true);
    }

    int latencyOf (Inserts* inserts, juce::uint32 id)
    {
        return inserts != nullptr && inserts->isActive (id) ? inserts->getLatency (id) : 0;
    }

    /** A track's own latency: none when it is frozen, as its effects don't run. */
    int latencyOf (Inserts* inserts, const SessionTrack& track)
    {
        return track.isFrozen() ? 0 : latencyOf (inserts, track.id);
    }

    /** A frozen track's audio from frame start, into frames [0, numFrames) of out. */
    void readFrozen (const SessionTrack& track, juce::int64 start, juce::AudioBuffer<float>& out, int numFrames)
    {
        out.clear (0, 0, numFrames);
        out.clear (1, 0, numFrames);
        const auto& audio = *track.frozenAudio;
        const auto from = juce::jmax ((juce::int64) 0, start), to = juce::jmin (audio.getNumFrames(), start + numFrames);

        if (to <= from)
            return;

        // Read into the right place of out: frames before 0 stay silent.
        const auto offset = (int) (from - start), count = (int) (to - from);
        juce::AudioBuffer<float> part (out.getArrayOfWritePointers(), 2, offset, count);
        audio.read (part, 0, from, count);

        if (audio.getNumChannels() == 1)
            part.copyFrom (1, 0, part, 0, 0, count);
    }

    /** The bus a track's or bus's output goes to, or null for the mix. */
    const SessionTrack* outputBus (const SessionState& state, const SessionTrack& track)
    {
        return state.outputBusOf (track);
    }

    /** The latency from a bus's input to the mix: its own and that of the buses its output
        goes through. Its input is the audio at start + this. */
    int pathLatency (const SessionState& state, const SessionTrack& bus, Inserts* inserts)
    {
        int total = 0;
        const auto* b = &bus;

        for (size_t steps = 0; b != nullptr && steps <= state.tracks.size(); ++steps, b = outputBus (state, *b))
            total += latencyOf (inserts, b->id);

        return total;
    }

    /** How many buses a bus's output passes through to the mix: 0 when it plays into the mix.
        Buses are mixed deepest first, so each comes before the buses it plays into. */
    int depthOf (const SessionState& state, const SessionTrack& bus)
    {
        int depth = 0;

        for (const auto* b = outputBus (state, bus); b != nullptr && depth <= (int) state.tracks.size(); b = outputBus (state, *b))
            ++depth;

        return depth;
    }

    /** The most latency among the buses the track feeds: how far its sends and output must run ahead. */
    int sendLatency (const SessionState& state, const SessionTrack& track, Inserts* inserts)
    {
        int most = 0;

        if (const auto* bus = outputBus (state, track))
            most = pathLatency (state, *bus, inserts);

        for (auto& send : track.sends)
            if (const auto* bus = state.findTrack (send.busId); bus != nullptr && bus->isBus)
                most = juce::jmax (most, pathLatency (state, *bus, inserts));

        return most;
    }

    /** Adds the first n frames of src, held back by frames, times gain, into dest from destStart.
        The delay line is the path's own, so it carries over from block to block. */
    void addHeld (Buffers& buffers, std::pair<juce::uint32, juce::uint32> path, int frames, const juce::AudioBuffer<float>& src,
                  int n, juce::AudioBuffer<float>& dest, int destStart, float gain)
    {
        const juce::AudioBuffer<float>* from = &src;

        if (frames > 0)
        {
            auto& delay = buffers.delays[path];

            if (delay.frames != frames)
            {
                delay.ring.setSize (2, juce::nextPowerOfTwo (frames + 1));
                delay.ring.clear();
                delay.frames = frames;
                delay.write = 0;
            }

            grow (buffers.held, n);
            const auto mask = delay.ring.getNumSamples() - 1;

            for (int ch = 0; ch < 2; ++ch)
            {
                auto* ring = delay.ring.getWritePointer (ch);
                const auto* in = src.getReadPointer (ch);
                auto* out = buffers.held.getWritePointer (ch);

                for (int i = 0; i < n; ++i)
                {
                    const auto w = (delay.write + i) & mask;
                    ring[w] = in[i];
                    out[i] = ring[(w - frames) & mask];
                }
            }

            delay.write = (delay.write + n) & mask;
            from = &buffers.held;
        }

        for (int ch = 0; ch < 2; ++ch)
            dest.addFrom (ch, destStart, *from, ch, 0, n, gain);
    }

    /** A live input, panned like a clip, into frames [0, numFrames) of out. */
    void addLive (const LiveInput& live, const juce::AudioBuffer<float>& audio, juce::AudioBuffer<float>& out, int numFrames,
                  const float* pans, float pan)
    {
        const auto channels = juce::jlimit (1, 2, live.numChannels);
        const auto* inLeft = audio.getReadPointer (live.firstChannel);
        const auto* inRight = audio.getReadPointer (live.firstChannel + channels - 1);
        auto* outLeft = out.getWritePointer (0);
        auto* outRight = out.getWritePointer (1);

        if (pans == nullptr)
        {
            const auto [left, right] = panGains (pan, channels);
            juce::FloatVectorOperations::addWithMultiply (outLeft, inLeft, left, numFrames);
            juce::FloatVectorOperations::addWithMultiply (outRight, inRight, right, numFrames);
            return;
        }

        for (int i = 0; i < numFrames; ++i)
        {
            const auto [left, right] = panGains (pans[i], channels);
            outLeft[i] += inLeft[i] * left;
            outRight[i] += inRight[i] * right;
        }
    }

    /** The track's clips, panned, before its effects and volume, into frames [0, numFrames) of out.
        pans, when the pan is automated, holds the pan for each of those frames. */
    void renderTrack (const SessionTrack& track, juce::int64 start, juce::AudioBuffer<float>& out, int numFrames,
                      juce::AudioBuffer<float>& scratch, juce::AudioBuffer<float>& gains, const float* pans)
    {
        out.clear (0, 0, numFrames);
        out.clear (1, 0, numFrames);
        const auto end = start + numFrames;

        for (auto& clip : track.clips)
        {
            const auto from = juce::jmax (start, clip.position);
            const auto to = juce::jmin (end, clip.getEnd());

            if (to <= from || clip.audio == nullptr || clip.muted)
                continue;

            const auto count = (int) (to - from);
            const auto channels = juce::jmin (2, clip.audio->getNumChannels());
            grow (scratch, count);
            clip.audio->read (scratch, 0, clip.offset + (from - clip.position), count);

            if (clip.fadeIn > 0 || clip.fadeOut > 0)
            {
                for (int ch = 0; ch < channels; ++ch)
                {
                    auto* s = scratch.getWritePointer (ch);

                    for (int i = 0; i < count; ++i)
                    {
                        const auto inClip = from - clip.position + i;
                        s[i] *= FadeShapes::level (clip.fadeInShape, inClip, clip.fadeIn)
                                * FadeShapes::level (clip.fadeOutShape, clip.length - 1 - inClip, clip.fadeOut);
                    }
                }
            }

            // The gain envelope, read at the clip's audio frames.
            if (! clip.gainEnvelope.isEmpty())
            {
                grow (gains, count);
                auto* g = gains.getWritePointer (0);
                clip.gainEnvelope.fill (clip.offset + (from - clip.position), g, count);

                for (int i = 0; i < count; ++i)
                    g[i] = juce::Decibels::decibelsToGain (g[i], -150.0f);

                for (int ch = 0; ch < channels; ++ch)
                    juce::FloatVectorOperations::multiply (scratch.getWritePointer (ch), g, count);
            }

            const auto clipGain = juce::Decibels::decibelsToGain (clip.gainDb, -150.0f);
            const auto offset = (int) (from - start);

            if (pans == nullptr)
            {
                const auto [left, right] = panGains (track.pan, channels);
                out.addFrom (0, offset, scratch, 0, 0, count, clipGain * left);
                out.addFrom (1, offset, scratch, channels - 1, 0, count, clipGain * right);
                continue;
            }

            const auto* inLeft = scratch.getReadPointer (0);
            const auto* inRight = scratch.getReadPointer (channels - 1);
            auto* outLeft = out.getWritePointer (0, offset);
            auto* outRight = out.getWritePointer (1, offset);

            for (int i = 0; i < count; ++i)
            {
                const auto [left, right] = panGains (pans[offset + i], channels);
                outLeft[i] += inLeft[i] * (clipGain * left);
                outRight[i] += inRight[i] * (clipGain * right);
            }
        }
    }
}

namespace
{
    /** Places track i in the mixing order after its key (marks: 1 placed, 2 a key, 4 being
        placed). The edits allow no loops; one here is just cut. */
    void place (const SessionState& state, Buffers& buffers, int i)
    {
        if ((buffers.marks[(size_t) i] & 5) != 0)
            return;

        buffers.marks[(size_t) i] |= 4;
        const auto& t = state.tracks[(size_t) i];

        if (! t.isBus && t.ducking.isOn())
        {
            for (size_t k = 0; k < state.tracks.size(); ++k)
            {
                if (state.tracks[k].id == t.ducking.keyTrackId && ! state.tracks[k].isBus)
                {
                    buffers.marks[k] |= 2;
                    place (state, buffers, (int) k);
                }
            }
        }

        buffers.marks[(size_t) i] = (char) ((buffers.marks[(size_t) i] & ~4) | 1);
        buffers.order.push_back (i);
    }

    /** The tracks' places in mixing order: the session's order, but each ducked track after its
        key. Marks which tracks are keys (2) along the way. */
    void mixingOrder (const SessionState& state, Buffers& buffers)
    {
        buffers.order.clear();
        buffers.marks.assign (state.tracks.size(), 0);

        for (size_t i = 0; i < state.tracks.size(); ++i)
            place (state, buffers, (int) i);
    }

    /** The gains by which a ducked track comes down over frames [0, numFrames), from its key's
        output (two channels), into gains. */
    void duckGains (const Ducking& d, double sampleRate, Buffers::Duck& state, const juce::AudioBuffer<float>& key, float* gains, int numFrames)
    {
        const auto hold = (float) std::exp (-1.0 / (0.02 * sampleRate));
        const auto attack = (float) (1.0 - std::exp (-1.0 / (d.attackMs * 0.001 * sampleRate)));
        const auto release = (float) (1.0 - std::exp (-1.0 / (d.releaseMs * 0.001 * sampleRate)));
        const auto threshold = juce::Decibels::decibelsToGain (d.thresholdDb);
        const auto* left = key.getReadPointer (0);
        const auto* right = key.getReadPointer (1);

        for (int i = 0; i < numFrames; ++i)
        {
            state.held = juce::jmax (std::abs (left[i]), std::abs (right[i]), state.held * hold);
            const auto target = state.held >= threshold ? d.amountDb : 0.0f;
            state.gainDb += (target - state.gainDb) * (target < state.gainDb ? attack : release);
            gains[i] = juce::Decibels::decibelsToGain (state.gainDb, -150.0f);
        }
    }
}

void render (const SessionState& state, juce::int64 start, juce::AudioBuffer<float>& dest, int destStart, int numFrames,
             Buffers& buffers, Inserts* inserts, const Live* live)
{
    for (int ch = 0; ch < dest.getNumChannels(); ++ch)
        dest.clear (ch, destStart, numFrames);

    grow (buffers.track, numFrames);
    grow (buffers.automation, numFrames);
    auto* pans = buffers.automation.getWritePointer (0);
    auto* volumes = buffers.automation.getWritePointer (1);

    for (auto& bus : state.tracks)
    {
        if (bus.isBus)
        {
            auto& input = buffers.buses[bus.id];
            grow (input, numFrames);
            input.clear (0, numFrames);
        }
    }

    mixingOrder (state, buffers);

    // A key's output is silence until it is mixed (and stays so when it doesn't play).
    for (size_t i = 0; i < state.tracks.size(); ++i)
    {
        if ((buffers.marks[i] & 2) != 0)
        {
            auto& key = buffers.keys[state.tracks[i].id];
            grow (key, numFrames);
            key.clear (0, numFrames);
        }
    }

    // Tracks: into the mix and their buses.
    for (const auto index : buffers.order)
    {
        const auto& track = state.tracks[(size_t) index];

        if (track.isBus || ! plays (state, track))
            continue;

        const auto own = latencyOf (inserts, track);
        const auto ahead = sendLatency (state, track, inserts);
        const auto panAutomated = ! track.panEnvelope.isEmpty();

        // The clips are read at start + own + ahead, so that is where their pan is taken from.
        if (panAutomated)
            track.panEnvelope.fill (start + own + ahead, pans, numFrames);

        // A frozen track's audio already has its clips, pan and effects: it is the track's
        // output at start + ahead.
        if (track.isFrozen())
            readFrozen (track, start + ahead, buffers.track, numFrames);
        else if (live == nullptr || live->clips)
            renderTrack (track, start + own + ahead, buffers.track, numFrames, buffers.clip, buffers.clipGains, panAutomated ? pans : nullptr);
        else
            buffers.track.clear (0, numFrames);

        if (live != nullptr && live->audio != nullptr && ! track.isFrozen())
            for (int i = 0; i < live->count; ++i)
                if (const auto& in = live->inputs[(size_t) i]; in.trackId == track.id && in.numChannels > 0
                                                                && in.firstChannel + in.numChannels <= live->audio->getNumChannels())
                    addLive (in, *live->audio, buffers.track, numFrames, panAutomated ? pans : nullptr, track.pan);

        if (inserts != nullptr && ! track.isFrozen() && inserts->isActive (track.id))
            inserts->process (track, start + own + ahead, buffers.track, numFrames);

        if (track.invertPolarity)
            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::negate (buffers.track.getWritePointer (ch), buffers.track.getReadPointer (ch), numFrames);

        // The track's output is now the audio at start + ahead.
        const auto send = [&] (const SessionSend& s, float volume)
        {
            const auto* bus = state.findTrack (s.busId);

            if (bus == nullptr || ! bus->isBus || ! plays (state, *bus))
                return;

            const auto gain = juce::Decibels::decibelsToGain (s.gainDb, -150.0f) * (s.preFader ? 1.0f : volume);
            addHeld (buffers, { track.id, bus->id }, ahead - pathLatency (state, *bus, inserts), buffers.track, numFrames,
                     buffers.buses[bus->id], 0, gain);
        };

        auto volume = juce::Decibels::decibelsToGain (track.gainDb, -150.0f);
        const auto volumeAutomated = ! track.volumeEnvelope.isEmpty();
        const auto* key = track.ducking.isOn() ? state.findTrack (track.ducking.keyTrackId) : nullptr;
        const auto ducked = key != nullptr && ! key->isBus && ! key->isFolder;

        // Pre-fader sends take the signal before the envelope and the ducking, which then act in place.
        if (volumeAutomated || ducked)
            for (auto& s : track.sends)
                if (s.preFader)
                    send (s, 1.0f);

        if (volumeAutomated)
        {
            track.volumeEnvelope.fill (start + ahead, volumes, numFrames);

            for (int i = 0; i < numFrames; ++i)
                volumes[i] = juce::Decibels::decibelsToGain (volumes[i], -150.0f);

            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::multiply (buffers.track.getWritePointer (ch), volumes, numFrames);

            volume = 1.0f;
        }

        if (ducked)
        {
            // The key as it was at this track's time: held back by what its buses' latency runs
            // it ahead of this track's.
            const auto& keyOut = buffers.keys[key->id];
            const auto behind = sendLatency (state, *key, inserts) - ahead;
            const juce::AudioBuffer<float>* lined = &keyOut;
            grow (buffers.duckGains, numFrames);

            if (behind > 0)
            {
                grow (buffers.keyHeld, numFrames);
                buffers.keyHeld.clear (0, numFrames);
                addHeld (buffers, { key->id, track.id }, behind, keyOut, numFrames, buffers.keyHeld, 0, 1.0f);
                lined = &buffers.keyHeld;
            }

            auto* gains = buffers.duckGains.getWritePointer (0);
            duckGains (track.ducking, state.sampleRate, buffers.ducks[track.id], *lined, gains, numFrames);

            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::multiply (buffers.track.getWritePointer (ch), gains, numFrames);
        }

        // Its folder's volume scales what it sends on after its own.
        if (const auto folder = state.folderGain (track); ! juce::exactlyEqual (folder, 1.0f))
            volume *= folder;

        if (buffers.levels != nullptr)
            buffers.levels->raise ((int) (&track - state.tracks.data()), peakOf (buffers.track, 0, numFrames) * volume,
                                   peakOf (buffers.track, 1, numFrames) * volume);

        // What a track ducking under this one listens to: its output after its volume.
        if ((buffers.marks[(size_t) index] & 2) != 0)
        {
            auto& keyOut = buffers.keys[track.id];

            for (int ch = 0; ch < 2; ++ch)
                keyOut.copyFrom (ch, 0, buffers.track.getReadPointer (ch), numFrames, volume);
        }

        // Its output: the mix, or a bus in its place (nothing when that bus doesn't play).
        if (const auto* bus = outputBus (state, track))
        {
            if (plays (state, *bus))
                addHeld (buffers, { track.id, 0 }, ahead - pathLatency (state, *bus, inserts), buffers.track, numFrames,
                         buffers.buses[bus->id], 0, volume);
        }
        else
        {
            addHeld (buffers, { track.id, 0 }, ahead, buffers.track, numFrames, dest, destStart, volume);
        }

        for (auto& s : track.sends)
            if (! ((volumeAutomated || ducked) && s.preFader))
                send (s, volume);
    }

    // Buses, each before the buses it plays into: their input is the audio at start + the latency
    // from there to the mix, so their effects bring it to the time their output's input needs.
    buffers.busOrder.clear();

    for (size_t i = 0; i < state.tracks.size(); ++i)
        if (state.tracks[i].isBus)
            buffers.busOrder.emplace_back (-depthOf (state, state.tracks[i]), (int) i);

    std::sort (buffers.busOrder.begin(), buffers.busOrder.end());

    for (const auto [negativeDepth, busIndex] : buffers.busOrder)
    {
        juce::ignoreUnused (negativeDepth);
        const auto& bus = state.tracks[(size_t) busIndex];

        if (! plays (state, bus))
            continue;

        auto& input = buffers.buses[bus.id];

        if (inserts != nullptr && inserts->isActive (bus.id))
            inserts->process (bus, start + pathLatency (state, bus, inserts), input, numFrames);

        if (bus.invertPolarity)
            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::negate (input.getWritePointer (ch), input.getReadPointer (ch), numFrames);

        const auto index = busIndex;
        const auto folder = state.folderGain (bus);

        // Its output: the mix, or the bus it plays into (nothing when that bus doesn't play).
        auto* out = &dest;
        auto outStart = destStart;

        if (const auto* next = outputBus (state, bus))
        {
            out = plays (state, *next) ? &buffers.buses[next->id] : nullptr;
            outStart = 0;
        }

        if (bus.volumeEnvelope.isEmpty() && bus.panEnvelope.isEmpty())
        {
            const auto volume = juce::Decibels::decibelsToGain (bus.gainDb, -150.0f) * folder;
            const auto [left, right] = panGains (bus.pan, 2);

            if (out != nullptr)
            {
                out->addFrom (0, outStart, input, 0, 0, numFrames, volume * left);
                out->addFrom (1, outStart, input, 1, 0, numFrames, volume * right);
            }

            if (buffers.levels != nullptr)
                buffers.levels->raise (index, peakOf (input, 0, numFrames) * (volume * left), peakOf (input, 1, numFrames) * (volume * right));

            continue;
        }

        for (auto [parameter, values] : { std::pair { AutomationParameter::volume, volumes }, std::pair { AutomationParameter::pan, pans } })
        {
            if (const auto& envelope = bus.envelope (parameter); ! envelope.isEmpty())
                envelope.fill (start, values, numFrames);
            else
                juce::FloatVectorOperations::fill (values, parameter == AutomationParameter::volume ? bus.gainDb : bus.pan, numFrames);
        }

        const auto* inLeft = input.getReadPointer (0);
        const auto* inRight = input.getReadPointer (1);
        auto* outLeft = out != nullptr ? out->getWritePointer (0, outStart) : nullptr;
        auto* outRight = out != nullptr ? out->getWritePointer (1, outStart) : nullptr;

        auto peakLeft = 0.0f, peakRight = 0.0f;

        for (int i = 0; i < numFrames; ++i)
        {
            const auto volume = juce::Decibels::decibelsToGain (volumes[i], -150.0f) * folder;
            const auto [left, right] = panGains (pans[i], 2);

            if (outLeft != nullptr)
            {
                outLeft[i] += inLeft[i] * (volume * left);
                outRight[i] += inRight[i] * (volume * right);
            }

            peakLeft = juce::jmax (peakLeft, std::abs (inLeft[i] * (volume * left)));
            peakRight = juce::jmax (peakRight, std::abs (inRight[i] * (volume * right)));
        }

        if (buffers.levels != nullptr)
            buffers.levels->raise (index, peakLeft, peakRight);
    }
}

void prime (const SessionState& state, juce::int64 start, Buffers& buffers, Inserts& inserts)
{
    for (auto& track : state.tracks)
        inserts.reset (track.id);

    for (auto& [path, delay] : buffers.delays)
    {
        delay.ring.clear();
        delay.write = 0;
    }

    buffers.ducks.clear();

    // The longest path: a track's own latency plus that of the buses it feeds.
    int longest = 0;

    for (auto& track : state.tracks)
        if (! track.isBus && plays (state, track))
            longest = juce::jmax (longest, latencyOf (&inserts, track) + sendLatency (state, track, &inserts));

    const auto chunk = juce::jmax (1, buffers.discard.getNumSamples());

    for (int done = 0; done < longest;)
    {
        const auto n = juce::jmin (chunk, longest - done);
        grow (buffers.discard, n);
        render (state, start - longest + done, buffers.discard, 0, n, buffers, &inserts);
        done += n;
    }
}
}
