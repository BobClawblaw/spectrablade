#include "OfflineRender.h"
#include "AudioEdits.h"
#include "TrackEffects.h"
#include "BuiltInEffects.h"

namespace OfflineRender
{
namespace
{
    constexpr int blockSize = 1024;

    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        juce::String name;
    };
}

namespace
{
    /** Runs `audio` (one or two channels) through one plug-in in place, lined up for its latency.
        progress gets 0..1 through it. */
    juce::String runThrough (Slot& slot, double sampleRate, juce::AudioBuffer<float>& audio, const std::function<bool (float)>& progress)
    {
        auto& plugin = *slot.plugin;
        plugin.enableAllBuses();
        plugin.setNonRealtime (true);
        plugin.setRateAndBufferSizeDetails (sampleRate, blockSize);
        plugin.prepareToPlay (sampleRate, blockSize);

        const auto docChannels = audio.getNumChannels();
        const auto numFrames = audio.getNumSamples();
        const auto ins  = plugin.getTotalNumInputChannels();
        const auto mainIns = plugin.getBusCount (true) > 0 ? plugin.getChannelCountOfBus (true, 0) : 0;   // a sidechain input stays silent
        const auto outs = plugin.getTotalNumOutputChannels();
        const auto latency = juce::jmax (0, plugin.getLatencySamples());

        if (outs == 0)
        {
            plugin.releaseResources();
            return slot.name + " has no audio outputs.";
        }

        // Feed the audio followed by `latency` frames of silence, and drop the first `latency`
        // output frames, so the result is time-aligned with the input.
        juce::AudioBuffer<float> work (juce::jmax (ins, outs), blockSize);
        juce::AudioBuffer<float> result (docChannels, numFrames);
        juce::MidiBuffer midi;
        const auto totalIn = (juce::int64) numFrames + latency;

        for (juce::int64 pos = 0; pos < totalIn; pos += blockSize)
        {
            const auto count = (int) juce::jmin ((juce::int64) blockSize, totalIn - pos);
            work.clear();

            // Input: the audio's channels mapped onto the plug-in's inputs.
            if (mainIns > 0 && pos < numFrames)
            {
                const auto available = (int) juce::jmin ((juce::int64) count, (juce::int64) numFrames - pos);
                juce::AudioBuffer<float> chunk (docChannels, available);

                for (int ch = 0; ch < docChannels; ++ch)
                    chunk.copyFrom (ch, 0, audio, ch, (int) pos, available);

                const auto fitted = AudioEdits::matchChannels (chunk, mainIns);

                for (int ch = 0; ch < mainIns; ++ch)
                    work.copyFrom (ch, 0, fitted, ch, 0, available);
            }

            juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), work.getNumChannels(), count);
            midi.clear();
            plugin.processBlock (view, midi);

            // Output: the plug-in's outputs mapped back to the audio's channels, skipping latency.
            const auto outStart = pos - latency;
            const auto skip = (int) juce::jmax ((juce::int64) 0, -outStart);
            const auto writeFrom = juce::jmax ((juce::int64) 0, outStart);
            const auto toWrite = (int) juce::jmin ((juce::int64) count - skip, (juce::int64) numFrames - writeFrom);

            if (toWrite > 0)
            {
                juce::AudioBuffer<float> produced (outs, toWrite);

                for (int ch = 0; ch < outs; ++ch)
                    produced.copyFrom (ch, 0, work, ch, skip, toWrite);

                const auto fitted = AudioEdits::matchChannels (produced, docChannels);

                for (int ch = 0; ch < docChannels; ++ch)
                    result.copyFrom (ch, (int) writeFrom, fitted, ch, 0, toWrite);
            }

            if (progress != nullptr && ! progress ((float) (pos + count) / (float) totalIn))
            {
                plugin.releaseResources();
                return "Cancelled.";
            }
        }

        plugin.releaseResources();
        audio = std::move (result);
        return {};
    }

    /** Runs all of `audio`'s channels through a built-in effect that links them (as the rack plays
        them through it), in place and lined up for its latency. False, with nothing done, when the
        effect can't link them; error says why it stopped otherwise. */
    bool runLinked (Slot& slot, double sampleRate, juce::AudioBuffer<float>& audio, const std::function<bool (float)>& progress, juce::String& error)
    {
        auto* effect = dynamic_cast<BuiltInEffects::BuiltInEffect*> (slot.plugin.get());

        if (effect == nullptr || ! effect->linksChannels() || audio.getNumChannels() > BuiltInEffects::BuiltInEffect::mostLinkedChannels)
            return false;

        effect->enableAllBuses();
        effect->setNonRealtime (true);
        effect->setRateAndBufferSizeDetails (sampleRate, blockSize);
        effect->prepareToPlay (sampleRate, blockSize);

        const auto channels = audio.getNumChannels();
        const auto numFrames = audio.getNumSamples();
        const auto latency = juce::jmax (0, effect->getLatencySamples());
        const auto totalIn = (juce::int64) numFrames + latency;
        juce::AudioBuffer<float> work (channels, blockSize), result (channels, numFrames);

        for (juce::int64 pos = 0; pos < totalIn; pos += blockSize)
        {
            const auto count = (int) juce::jmin ((juce::int64) blockSize, totalIn - pos);
            work.clear();

            if (pos < numFrames)
                for (int ch = 0; ch < channels; ++ch)
                    work.copyFrom (ch, 0, audio, ch, (int) pos, (int) juce::jmin ((juce::int64) count, (juce::int64) numFrames - pos));

            juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), channels, count);

            if (! effect->processLinked (view))
            {
                effect->releaseResources();
                return false;
            }

            const auto outStart = pos - latency;
            const auto skip = (int) juce::jmax ((juce::int64) 0, -outStart);
            const auto writeFrom = juce::jmax ((juce::int64) 0, outStart);
            const auto toWrite = (int) juce::jmin ((juce::int64) count - skip, (juce::int64) numFrames - writeFrom);

            for (int ch = 0; toWrite > 0 && ch < channels; ++ch)
                result.copyFrom (ch, (int) writeFrom, work, ch, skip, toWrite);

            if (progress != nullptr && ! progress ((float) (pos + count) / (float) totalIn))
            {
                effect->releaseResources();
                error = "Cancelled.";
                return true;
            }
        }

        effect->releaseResources();
        audio = std::move (result);
        return true;
    }
}

juce::String processThroughRack (const juce::XmlElement& rackXml,
                                 juce::AudioPluginFormatManager& formats,
                                 double sampleRate,
                                 juce::AudioBuffer<float>& audio,
                                 const std::function<bool (float)>& progress)
{
    std::vector<Slot> slots;

    // Some plug-ins must be destroyed on the message thread, so hand them over when we're done.
    const juce::ScopeGuard destroyOnMessageThread { [&slots]
    {
        auto doomed = std::make_shared<std::vector<Slot>> (std::move (slots));
        juce::MessageManager::callAsync ([doomed] { doomed->clear(); });
    } };

    std::vector<const juce::XmlElement*> wanted;

    for (auto* slotXml : rackXml.getChildWithTagNameIterator ("SLOT"))
        if (! slotXml->getBoolAttribute ("bypassed"))
            wanted.push_back (slotXml);

    // The rack is stereo: the channels go through it two at a time (L R, C LFE, Ls Rs for 5.1),
    // a mono file or a last odd channel alone, each group through plug-ins of its own; a built-in
    // effect that links channels (the compressor, the limiter) takes them all at once.
    const auto docChannels = audio.getNumChannels();
    const auto numFrames = audio.getNumSamples();
    const auto groups = juce::jmax (1, (docChannels + 1) / 2);
    slots.reserve (wanted.size() * (size_t) groups);   // create hands out pointers into it

    const auto create = [&] (const juce::XmlElement& slotXml, juce::String& error) -> Slot*
    {
        juce::PluginDescription description;

        if (auto* d = slotXml.getChildByName ("PLUGIN"); d == nullptr || ! description.loadFromXml (*d))
        {
            error = "The rack has a slot without a plug-in description.";
            return nullptr;
        }

        auto plugin = formats.createPluginInstance (description, sampleRate, blockSize, error);

        if (plugin == nullptr)
        {
            error = description.name + ": " + (error.isNotEmpty() ? error : juce::String ("could not be created."));
            return nullptr;
        }

        if (auto* stateXml = slotXml.getChildByName ("STATE"))
        {
            juce::MemoryBlock state;

            if (state.fromBase64Encoding (stateXml->getAllSubText()))
                plugin->setStateInformation (state.getData(), (int) state.getSize());
        }

        slots.push_back ({ std::move (plugin), description.name });
        return &slots.back();
    };

    for (size_t index = 0; index < wanted.size(); ++index)
    {
        const auto step = (float) index, steps = (float) wanted.size();
        juce::String error;
        auto* first = create (*wanted[index], error);

        if (first == nullptr)
            return error;

        if (docChannels > 2)
        {
            const auto linked = runLinked (*first, sampleRate, audio, [&progress, step, steps] (float through)
                                           {
                                               return progress == nullptr || progress ((step + through) / steps);
                                           }, error);

            if (error.isNotEmpty())
                return error;

            if (linked)
                continue;
        }

        juce::AudioBuffer<float> result (docChannels, numFrames);

        for (int group = 0; group < groups; ++group)
        {
            const auto from = 2 * group, count = juce::jmin (2, docChannels - from);
            juce::AudioBuffer<float> part (count, numFrames);

            for (int ch = 0; ch < count; ++ch)
                part.copyFrom (ch, 0, audio, from + ch, 0, numFrames);

            auto* slot = group == 0 ? first : create (*wanted[index], error);

            if (slot == nullptr)
                return error;

            if (const auto failed = runThrough (*slot, sampleRate, part, [&progress, step, steps, group, groups] (float through)
                                                {
                                                    return progress == nullptr || progress ((step + ((float) group + through) / (float) groups) / steps);
                                                });
                failed.isNotEmpty())
                return failed;

            for (int ch = 0; ch < count; ++ch)
                result.copyFrom (from + ch, 0, part, ch, 0, numFrames);
        }

        audio = std::move (result);
    }

    return {};
}

juce::String loadTrackEffects (const std::map<juce::uint32, std::unique_ptr<juce::XmlElement>>& effects,
                               juce::AudioPluginFormatManager& formats,
                               double sampleRate,
                               TrackEffects& into)
{
    juce::StringArray errors;
    into.prepare (sampleRate);
    into.makeCopy = [&formats, sampleRate] (const juce::PluginDescription& d)
    {
        juce::String error;
        auto copy = formats.createPluginInstance (d, sampleRate, TrackEffects::maxBlock, error);

        if (copy != nullptr)
            copy->setNonRealtime (true);

        return copy;
    };

    for (auto& [trackId, xml] : effects)
    {
        if (xml == nullptr)
            continue;

        for (auto* slotXml : xml->getChildWithTagNameIterator ("SLOT"))
        {
            juce::PluginDescription description;

            if (auto* d = slotXml->getChildByName ("PLUGIN"); d == nullptr || ! description.loadFromXml (*d))
            {
                errors.add ("A track effect is missing its plug-in description.");
                continue;
            }

            juce::String error;
            auto plugin = formats.createPluginInstance (description, sampleRate, TrackEffects::maxBlock, error);

            if (plugin == nullptr)
            {
                errors.add (description.name + ": " + (error.isNotEmpty() ? error : juce::String ("could not be created.")));
                continue;
            }

            // State after preparing, as the live chains are restored.
            auto* instance = plugin.get();
            instance->setNonRealtime (true);
            into.add (trackId, std::move (plugin), (juce::uint32) slotXml->getIntAttribute ("id"));
            into.setBypassed (trackId, into.getNumSlots (trackId) - 1, slotXml->getBoolAttribute ("bypassed"));

            if (auto* stateXml = slotXml->getChildByName ("STATE"))
            {
                juce::MemoryBlock state;

                if (state.fromBase64Encoding (stateXml->getAllSubText()))
                    instance->setStateInformation (state.getData(), (int) state.getSize());
            }

            into.refreshCopies (*instance);
        }
    }

    return errors.joinIntoString ("\n");
}

bool mixSession (const SessionState& state, juce::Range<juce::int64> range, juce::AudioBuffer<float>& dest,
                 SessionMixer::Inserts* inserts, const std::function<bool (float)>& progress)
{
    const auto total = (int) range.getLength();
    dest.setSize (state.masterChannels(), total);

    SessionMixer::Buffers buffers;
    buffers.reserve (blockSize);
    buffers.surroundChannel = 2;     // a 5.1 or 7.1 master's channels in file order
    buffers.interfaceChannel = -1;   // the mix alone

    if (inserts != nullptr)
    {
        std::map<juce::uint32, int> wide;

        for (auto& t : state.tracks)
            if (t.isWide() && ! t.isFrozen())
                wide[t.id] = t.wideChannels();

        inserts->setWideChannels (wide);
        SessionMixer::prime (state, range.getStart(), buffers, *inserts);
    }

    for (int done = 0; done < total; done += blockSize)
    {
        const auto count = juce::jmin (blockSize, total - done);
        SessionMixer::render (state, range.getStart() + done, dest, done, count, buffers, inserts);

        if (progress != nullptr && ! progress ((float) (done + count) / (float) total))
            return false;
    }

    return true;
}
}
