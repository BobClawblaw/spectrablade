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

    for (auto [slot, value] : { std::pair { 2 * track, left }, std::pair { 2 * track + 1, right } })
    {
        auto& peak = peaks[(size_t) slot];
        auto current = peak.load (std::memory_order_relaxed);

        while (value > current && ! peak.compare_exchange_weak (current, value, std::memory_order_relaxed))
        {
        }
    }
}

std::pair<float, float> Levels::take (int track) noexcept
{
    if (! juce::isPositiveAndBelow (track, maxTracks))
        return {};

    return { peaks[(size_t) (2 * track)].exchange (0.0f), peaks[(size_t) (2 * track + 1)].exchange (0.0f) };
}

namespace
{
    float peakOf (const juce::AudioBuffer<float>& b, int channel, int numFrames)
    {
        const auto range = juce::FloatVectorOperations::findMinAndMax (b.getReadPointer (channel), numFrames);
        return juce::jmax (-range.getStart(), range.getEnd());
    }

    float fadeGain (juce::int64 frame, juce::int64 fade)
    {
        return frame >= fade ? 1.0f : 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * (float) frame / (float) fade);
    }

    bool plays (const SessionState& state, const SessionTrack& track)
    {
        if (track.mute)
            return false;

        if (! state.anySolo() || track.solo)
            return true;

        // A soloed bus brings the tracks feeding it; a soloed track brings the buses it feeds.
        if (track.isBus)
            return std::any_of (state.tracks.begin(), state.tracks.end(),
                                [&] (auto& t) { return t.solo && ! t.mute && t.findSend (track.id) != nullptr; });

        return std::any_of (track.sends.begin(), track.sends.end(), [&] (auto& send)
        {
            const auto* bus = state.findTrack (send.busId);
            return bus != nullptr && bus->solo;
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

    /** The most latency among the buses the track sends to: how far its sends must run ahead. */
    int sendLatency (const SessionState& state, const SessionTrack& track, Inserts* inserts)
    {
        int most = 0;

        for (auto& send : track.sends)
            if (const auto* bus = state.findTrack (send.busId); bus != nullptr && bus->isBus)
                most = juce::jmax (most, latencyOf (inserts, bus->id));

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
                      juce::AudioBuffer<float>& scratch, const float* pans)
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
                        s[i] *= fadeGain (inClip, clip.fadeIn) * fadeGain (clip.length - 1 - inClip, clip.fadeOut);
                    }
                }
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

    // Tracks: into the mix and their buses.
    for (auto& track : state.tracks)
    {
        if (track.isBus || ! plays (state, track))
            continue;

        const auto own = latencyOf (inserts, track.id);
        const auto ahead = sendLatency (state, track, inserts);
        const auto panAutomated = ! track.panEnvelope.isEmpty();

        // The clips are read at start + own + ahead, so that is where their pan is taken from.
        if (panAutomated)
            track.panEnvelope.fill (start + own + ahead, pans, numFrames);

        if (live == nullptr || live->clips)
            renderTrack (track, start + own + ahead, buffers.track, numFrames, buffers.clip, panAutomated ? pans : nullptr);
        else
            buffers.track.clear (0, numFrames);

        if (live != nullptr && live->audio != nullptr)
            for (int i = 0; i < live->count; ++i)
                if (const auto& in = live->inputs[(size_t) i]; in.trackId == track.id && in.numChannels > 0
                                                                && in.firstChannel + in.numChannels <= live->audio->getNumChannels())
                    addLive (in, *live->audio, buffers.track, numFrames, panAutomated ? pans : nullptr, track.pan);

        if (inserts != nullptr && inserts->isActive (track.id))
            inserts->process (track, start + own + ahead, buffers.track, numFrames);

        // The track's output is now the audio at start + ahead.
        const auto send = [&] (const SessionSend& s, float volume)
        {
            const auto* bus = state.findTrack (s.busId);

            if (bus == nullptr || ! bus->isBus || ! plays (state, *bus))
                return;

            const auto gain = juce::Decibels::decibelsToGain (s.gainDb, -150.0f) * (s.preFader ? 1.0f : volume);
            addHeld (buffers, { track.id, bus->id }, ahead - latencyOf (inserts, bus->id), buffers.track, numFrames,
                     buffers.buses[bus->id], 0, gain);
        };

        auto volume = juce::Decibels::decibelsToGain (track.gainDb, -150.0f);
        const auto volumeAutomated = ! track.volumeEnvelope.isEmpty();

        if (volumeAutomated)
        {
            // Pre-fader sends take the signal before the envelope; then it is applied in place.
            for (auto& s : track.sends)
                if (s.preFader)
                    send (s, 1.0f);

            track.volumeEnvelope.fill (start + ahead, volumes, numFrames);

            for (int i = 0; i < numFrames; ++i)
                volumes[i] = juce::Decibels::decibelsToGain (volumes[i], -150.0f);

            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::multiply (buffers.track.getWritePointer (ch), volumes, numFrames);

            volume = 1.0f;
        }

        if (buffers.levels != nullptr)
            buffers.levels->raise ((int) (&track - state.tracks.data()), peakOf (buffers.track, 0, numFrames) * volume,
                                   peakOf (buffers.track, 1, numFrames) * volume);

        addHeld (buffers, { track.id, 0 }, ahead, buffers.track, numFrames, dest, destStart, volume);

        for (auto& s : track.sends)
            if (! (volumeAutomated && s.preFader))
                send (s, volume);
    }

    // Buses: their input is the audio at start + their latency, so their effects bring it to start.
    for (auto& bus : state.tracks)
    {
        if (! bus.isBus || ! plays (state, bus))
            continue;

        auto& input = buffers.buses[bus.id];

        if (inserts != nullptr && inserts->isActive (bus.id))
            inserts->process (bus, start + latencyOf (inserts, bus.id), input, numFrames);

        const auto index = (int) (&bus - state.tracks.data());

        if (bus.volumeEnvelope.isEmpty() && bus.panEnvelope.isEmpty())
        {
            const auto volume = juce::Decibels::decibelsToGain (bus.gainDb, -150.0f);
            const auto [left, right] = panGains (bus.pan, 2);
            dest.addFrom (0, destStart, input, 0, 0, numFrames, volume * left);
            dest.addFrom (1, destStart, input, 1, 0, numFrames, volume * right);

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
        auto* outLeft = dest.getWritePointer (0, destStart);
        auto* outRight = dest.getWritePointer (1, destStart);

        auto peakLeft = 0.0f, peakRight = 0.0f;

        for (int i = 0; i < numFrames; ++i)
        {
            const auto volume = juce::Decibels::decibelsToGain (volumes[i], -150.0f);
            const auto [left, right] = panGains (pans[i], 2);
            outLeft[i] += inLeft[i] * (volume * left);
            outRight[i] += inRight[i] * (volume * right);
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

    // The longest path: a track's own latency plus that of the buses it feeds.
    int longest = 0;

    for (auto& track : state.tracks)
        if (! track.isBus && plays (state, track))
            longest = juce::jmax (longest, latencyOf (&inserts, track.id) + sendLatency (state, track, &inserts));

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
