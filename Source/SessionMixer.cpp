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

namespace
{
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

    /** The track's clips, panned, before its effects and volume, into frames [0, numFrames) of out. */
    void renderTrack (const SessionTrack& track, juce::int64 start, juce::AudioBuffer<float>& out, int numFrames,
                      juce::AudioBuffer<float>& scratch)
    {
        out.clear (0, 0, numFrames);
        out.clear (1, 0, numFrames);
        const auto end = start + numFrames;

        for (auto& clip : track.clips)
        {
            const auto from = juce::jmax (start, clip.position);
            const auto to = juce::jmin (end, clip.getEnd());

            if (to <= from || clip.audio == nullptr)
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
            const auto [left, right] = panGains (track.pan, channels);
            const auto offset = (int) (from - start);
            out.addFrom (0, offset, scratch, 0, 0, count, clipGain * left);
            out.addFrom (1, offset, scratch, channels - 1, 0, count, clipGain * right);
        }
    }
}

void render (const SessionState& state, juce::int64 start, juce::AudioBuffer<float>& dest, int destStart, int numFrames,
             Buffers& buffers, Inserts* inserts)
{
    for (int ch = 0; ch < dest.getNumChannels(); ++ch)
        dest.clear (ch, destStart, numFrames);

    grow (buffers.track, numFrames);

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
        renderTrack (track, start + own + ahead, buffers.track, numFrames, buffers.clip);

        if (inserts != nullptr && inserts->isActive (track.id))
            inserts->process (track.id, buffers.track, numFrames);

        // The track's output is now the audio at start + ahead.
        const auto volume = juce::Decibels::decibelsToGain (track.gainDb, -150.0f);
        addHeld (buffers, { track.id, 0 }, ahead, buffers.track, numFrames, dest, destStart, volume);

        for (auto& send : track.sends)
        {
            const auto* bus = state.findTrack (send.busId);

            if (bus == nullptr || ! bus->isBus || ! plays (state, *bus))
                continue;

            const auto gain = juce::Decibels::decibelsToGain (send.gainDb, -150.0f) * (send.preFader ? 1.0f : volume);
            addHeld (buffers, { track.id, bus->id }, ahead - latencyOf (inserts, bus->id), buffers.track, numFrames,
                     buffers.buses[bus->id], 0, gain);
        }
    }

    // Buses: their input is the audio at start + their latency, so their effects bring it to start.
    for (auto& bus : state.tracks)
    {
        if (! bus.isBus || ! plays (state, bus))
            continue;

        auto& input = buffers.buses[bus.id];

        if (inserts != nullptr && inserts->isActive (bus.id))
            inserts->process (bus.id, input, numFrames);

        const auto volume = juce::Decibels::decibelsToGain (bus.gainDb, -150.0f);
        const auto [left, right] = panGains (bus.pan, 2);
        dest.addFrom (0, destStart, input, 0, 0, numFrames, volume * left);
        dest.addFrom (1, destStart, input, 1, 0, numFrames, volume * right);
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
