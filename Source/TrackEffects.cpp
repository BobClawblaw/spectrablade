#include "TrackEffects.h"

TrackEffects::~TrackEffects()
{
    clear();
}

void TrackEffects::prepareSlot (Slot& slot) const
{
    auto& p = *slot.plugin;
    p.enableAllBuses();

    // Stereo in and out where the plug-in takes it; otherwise whatever it has.
    if (p.getBusCount (true) > 0)
        p.setChannelLayoutOfBus (true, 0, juce::AudioChannelSet::stereo());
    if (p.getBusCount (false) > 0)
        p.setChannelLayoutOfBus (false, 0, juce::AudioChannelSet::stereo());

    p.setRateAndBufferSizeDetails (rate, maxBlock);
    p.prepareToPlay (rate, maxBlock);
    slot.work.setSize (juce::jmax (2, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()), maxBlock);
}

void TrackEffects::prepare (double sampleRate)
{
    const juce::ScopedLock sl (lock);
    rate = sampleRate;
    midi.ensureSize (256);

    for (auto& [id, chain] : chains)
        for (auto& slot : chain)
        {
            slot->plugin->releaseResources();
            prepareSlot (*slot);
        }
}

void TrackEffects::clear()
{
    std::map<juce::uint32, Chain> old;

    {
        const juce::ScopedLock sl (lock);
        std::swap (old, chains);
    }

    for (auto& [id, chain] : old)
        for (auto& slot : chain)
            slot->plugin->releaseResources();
}

TrackEffects::Chain* TrackEffects::find (juce::uint32 trackId)
{
    const auto it = chains.find (trackId);
    return it != chains.end() ? &it->second : nullptr;
}

const TrackEffects::Chain* TrackEffects::find (juce::uint32 trackId) const
{
    const auto it = chains.find (trackId);
    return it != chains.end() ? &it->second : nullptr;
}

int TrackEffects::getNumSlots (juce::uint32 trackId) const
{
    const juce::ScopedLock sl (lock);
    const auto* chain = find (trackId);
    return chain != nullptr ? (int) chain->size() : 0;
}

juce::AudioPluginInstance* TrackEffects::getPlugin (juce::uint32 trackId, int slot) const
{
    const juce::ScopedLock sl (lock);
    const auto* chain = find (trackId);
    return chain != nullptr && juce::isPositiveAndBelow (slot, (int) chain->size()) ? (*chain)[(size_t) slot]->plugin.get() : nullptr;
}

bool TrackEffects::isBypassed (juce::uint32 trackId, int slot) const
{
    const juce::ScopedLock sl (lock);
    const auto* chain = find (trackId);
    return chain != nullptr && juce::isPositiveAndBelow (slot, (int) chain->size()) && (*chain)[(size_t) slot]->bypassed;
}

std::vector<juce::AudioPluginInstance*> TrackEffects::getAllPlugins() const
{
    const juce::ScopedLock sl (lock);
    std::vector<juce::AudioPluginInstance*> all;

    for (auto& [id, chain] : chains)
        for (auto& slot : chain)
            all.push_back (slot->plugin.get());

    return all;
}

std::map<juce::String, juce::AudioProcessorParameter*> TrackEffects::parametersById (juce::AudioPluginInstance& plugin)
{
    std::map<juce::String, juce::AudioProcessorParameter*> byId;

    for (auto* p : plugin.getParameters())
    {
        const auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (p);
        const auto id = hosted != nullptr ? hosted->getParameterID() : juce::String();
        byId[id.isNotEmpty() ? id : juce::String (p->getParameterIndex())] = p;
    }

    return byId;
}

juce::uint32 TrackEffects::add (juce::uint32 trackId, std::unique_ptr<juce::AudioPluginInstance> plugin, juce::uint32 slotId)
{
    auto slot = std::make_unique<Slot>();
    slot->plugin = std::move (plugin);
    slot->parameters = parametersById (*slot->plugin);

    // Prepared before it joins the chain, so the audio thread waits only for the insertion.
    // The rate changes only on this thread, so it needs no lock here.
    prepareSlot (*slot);

    const juce::ScopedLock sl (lock);
    slot->id = slotId != 0 ? slotId : nextSlotId;
    nextSlotId = juce::jmax (nextSlotId, slot->id + 1);
    chains[trackId].push_back (std::move (slot));
    return chains[trackId].back()->id;
}

juce::uint32 TrackEffects::getSlotId (juce::uint32 trackId, int slot) const
{
    const juce::ScopedLock sl (lock);
    const auto* chain = find (trackId);
    return chain != nullptr && juce::isPositiveAndBelow (slot, (int) chain->size()) ? (*chain)[(size_t) slot]->id : 0;
}

juce::AudioPluginInstance* TrackEffects::findSlot (juce::uint32 trackId, juce::uint32 slotId) const
{
    const juce::ScopedLock sl (lock);

    if (const auto* chain = find (trackId))
        for (auto& s : *chain)
            if (s->id == slotId)
                return s->plugin.get();

    return nullptr;
}

std::unique_ptr<juce::AudioPluginInstance> TrackEffects::remove (juce::uint32 trackId, int index)
{
    std::unique_ptr<Slot> slot;

    {
        const juce::ScopedLock sl (lock);
        auto* chain = find (trackId);

        if (chain == nullptr || ! juce::isPositiveAndBelow (index, (int) chain->size()))
            return {};

        slot = std::move ((*chain)[(size_t) index]);
        chain->erase (chain->begin() + index);

        if (chain->empty())
            chains.erase (trackId);
    }

    slot->plugin->releaseResources();
    return std::move (slot->plugin);
}

void TrackEffects::setBypassed (juce::uint32 trackId, int index, bool bypassed)
{
    const juce::ScopedLock sl (lock);

    if (auto* chain = find (trackId); chain != nullptr && juce::isPositiveAndBelow (index, (int) chain->size()))
    {
        auto& slot = *(*chain)[(size_t) index];

        // Coming back in, it starts from silence rather than from where it was left.
        if (slot.bypassed && ! bypassed)
            slot.plugin->reset();

        slot.bypassed = bypassed;
    }
}

void TrackEffects::move (juce::uint32 trackId, int from, int to)
{
    const juce::ScopedLock sl (lock);
    auto* chain = find (trackId);

    if (chain == nullptr || ! juce::isPositiveAndBelow (from, (int) chain->size()) || ! juce::isPositiveAndBelow (to, (int) chain->size()))
        return;

    auto slot = std::move ((*chain)[(size_t) from]);
    chain->erase (chain->begin() + from);
    chain->insert (chain->begin() + to, std::move (slot));
}

int TrackEffects::latencyOf (const Chain& chain) const
{
    int total = 0;

    for (auto& slot : chain)
        if (! slot->bypassed)
            total += juce::jmax (0, slot->plugin->getLatencySamples());

    return total;
}

int TrackEffects::getChainLatency (juce::uint32 trackId) const
{
    const juce::ScopedLock sl (lock);
    const auto* chain = find (trackId);
    return chain != nullptr ? latencyOf (*chain) : 0;
}

std::unique_ptr<juce::XmlElement> TrackEffects::createXml (juce::uint32 trackId) const
{
    const juce::ScopedLock sl (lock);
    const auto* chain = find (trackId);

    if (chain == nullptr)
        return {};

    auto xml = std::make_unique<juce::XmlElement> ("EFFECTS");

    for (auto& s : *chain)
    {
        auto* slot = xml->createNewChildElement ("SLOT");
        slot->setAttribute ("bypassed", s->bypassed);
        slot->setAttribute ("id", (int) s->id);
        slot->addChildElement (s->plugin->getPluginDescription().createXml().release());

        juce::MemoryBlock state;
        s->plugin->getStateInformation (state);
        slot->createNewChildElement ("STATE")->addTextElement (state.toBase64Encoding());
    }

    return xml;
}

//==============================================================================
bool TrackEffects::isActive (juce::uint32 trackId)
{
    const juce::ScopedTryLock sl (lock);

    // While the chain is being changed the track still counts as active, so it plays silence
    // rather than its dry signal.
    if (! sl.isLocked())
        return true;

    const auto* chain = find (trackId);
    return chain != nullptr && std::any_of (chain->begin(), chain->end(), [] (auto& s) { return ! s->bypassed; });
}

int TrackEffects::getLatency (juce::uint32 trackId)
{
    const juce::ScopedTryLock sl (lock);
    const auto* chain = sl.isLocked() ? find (trackId) : nullptr;
    return chain != nullptr ? latencyOf (*chain) : 0;
}

void TrackEffects::reset (juce::uint32 trackId)
{
    const juce::ScopedTryLock sl (lock);

    if (auto* chain = sl.isLocked() ? find (trackId) : nullptr)
        for (auto& slot : *chain)
            slot->plugin->reset();
}

void TrackEffects::process (const SessionTrack& track, juce::int64 frame, juce::AudioBuffer<float>& buffer, int numFrames)
{
    const juce::ScopedTryLock sl (lock);
    auto* chain = sl.isLocked() ? find (track.id) : nullptr;

    if (chain == nullptr)
    {
        buffer.clear (0, numFrames);
        return;
    }

    // Steps line up with session time, so the result doesn't depend on how the mix is split into blocks.
    const auto automated = std::any_of (track.effectEnvelopes.begin(), track.effectEnvelopes.end(), [chain] (const EffectEnvelope& e)
    {
        return std::any_of (chain->begin(), chain->end(), [&e] (auto& s) { return s->id == e.slotId && ! s->bypassed; });
    });

    for (int done = 0; done < numFrames;)
    {
        auto n = juce::jmin (maxBlock, numFrames - done);
        auto stepStart = frame + done;

        if (automated)
        {
            const auto into = (int) (((stepStart % automationStep) + automationStep) % automationStep);
            stepStart -= into;
            n = juce::jmin (n, automationStep - into);
        }

        // Effects earlier in the chain delay the audio, so each one is working on audio from
        // that much earlier, and its parameters follow their envelopes from there.
        juce::int64 delayed = 0;

        for (auto& s : *chain)
        {
            if (s->bypassed)
                continue;

            if (automated)
                for (auto& e : track.effectEnvelopes)
                    if (e.slotId == s->id)
                        if (const auto found = s->parameters.find (e.parameterId); found != s->parameters.end())
                            found->second->setValue (e.envelope.valueAt (stepStart - delayed));

            delayed += juce::jmax (0, s->plugin->getLatencySamples());
            auto& p = *s->plugin;
            juce::AudioBuffer<float> work (s->work.getArrayOfWritePointers(), s->work.getNumChannels(), n);
            work.clear();
            const auto ins = juce::jmin (2, juce::jmax (1, p.getTotalNumInputChannels()));

            // A mono input gets the average of the two sides.
            work.copyFrom (0, 0, buffer, 0, done, n);

            if (ins == 1)
            {
                work.addFrom (0, 0, buffer, 1, done, n);
                work.applyGain (0, 0, n, 0.5f);
            }
            else
            {
                work.copyFrom (1, 0, buffer, 1, done, n);
            }

            midi.clear();
            p.processBlock (work, midi);

            // A mono output plays on both sides.
            const auto outs = p.getTotalNumOutputChannels();
            buffer.copyFrom (0, done, work, 0, 0, n);
            buffer.copyFrom (1, done, work, outs >= 2 ? 1 : 0, 0, n);
        }

        done += n;
    }
}
