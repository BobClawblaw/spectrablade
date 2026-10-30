#include "OfflineRender.h"
#include "AudioEdits.h"

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

    for (auto* slotXml : rackXml.getChildWithTagNameIterator ("SLOT"))
    {
        if (slotXml->getBoolAttribute ("bypassed"))
            continue;

        juce::PluginDescription description;

        if (auto* d = slotXml->getChildByName ("PLUGIN"); d == nullptr || ! description.loadFromXml (*d))
            return "The rack has a slot without a plug-in description.";

        juce::String error;
        auto plugin = formats.createPluginInstance (description, sampleRate, blockSize, error);

        if (plugin == nullptr)
            return description.name + ": " + (error.isNotEmpty() ? error : juce::String ("could not be created."));

        if (auto* stateXml = slotXml->getChildByName ("STATE"))
        {
            juce::MemoryBlock state;

            if (state.fromBase64Encoding (stateXml->getAllSubText()))
                plugin->setStateInformation (state.getData(), (int) state.getSize());
        }

        slots.push_back ({ std::move (plugin), description.name });
    }

    const auto docChannels = audio.getNumChannels();
    const auto numFrames = audio.getNumSamples();

    for (size_t index = 0; index < slots.size(); ++index)
    {
        auto& plugin = *slots[index].plugin;
        plugin.enableAllBuses();
        plugin.setNonRealtime (true);
        plugin.setRateAndBufferSizeDetails (sampleRate, blockSize);
        plugin.prepareToPlay (sampleRate, blockSize);

        const auto ins  = plugin.getTotalNumInputChannels();
        const auto outs = plugin.getTotalNumOutputChannels();
        const auto latency = juce::jmax (0, plugin.getLatencySamples());

        if (outs == 0)
        {
            plugin.releaseResources();
            return slots[index].name + " has no audio outputs.";
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

            // Input: the document's channels mapped onto the plug-in's inputs.
            if (ins > 0 && pos < numFrames)
            {
                const auto available = (int) juce::jmin ((juce::int64) count, (juce::int64) numFrames - pos);
                juce::AudioBuffer<float> chunk (docChannels, available);

                for (int ch = 0; ch < docChannels; ++ch)
                    chunk.copyFrom (ch, 0, audio, ch, (int) pos, available);

                const auto fitted = AudioEdits::matchChannels (chunk, ins);

                for (int ch = 0; ch < ins; ++ch)
                    work.copyFrom (ch, 0, fitted, ch, 0, available);
            }

            juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), work.getNumChannels(), count);
            midi.clear();
            plugin.processBlock (view, midi);

            // Output: the plug-in's outputs mapped back to the document's channels, skipping latency.
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

            const auto done = ((float) index + (float) (pos + count) / (float) totalIn) / (float) slots.size();

            if (progress != nullptr && ! progress (done))
            {
                plugin.releaseResources();
                return "Cancelled.";
            }
        }

        plugin.releaseResources();
        audio = std::move (result);
    }

    return {};
}
}
