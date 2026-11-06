#include "TrackEffects.h"

namespace
{
    // Set while an effect is given a value from an envelope, so its echo isn't taken for a touch.
    thread_local bool followingEnvelope = false;

    void release (std::array<std::unique_ptr<juce::AudioPluginInstance>, 2>& copies)
    {
        for (auto& c : copies)
            if (c != nullptr)
                c->releaseResources();
    }
}

TrackEffects::~TrackEffects()
{
    cancelPendingUpdate();
    clear();
}

//==============================================================================
TrackEffects::Watcher::Watcher (TrackEffects& o, juce::uint32 track, juce::uint32 slot,
                                const std::map<juce::String, juce::AudioProcessorParameter*>& parameters)
    : owner (o), trackId (track), slotId (slot)
{
    for (auto& [id, parameter] : parameters)
    {
        byIndex[parameter->getParameterIndex()] = { parameter, id };
        parameter->addListener (this);
    }
}

TrackEffects::Watcher::~Watcher()
{
    for (auto& [index, entry] : byIndex)
        entry.first->removeListener (this);
}

void TrackEffects::Watcher::parameterValueChanged (int index, float value)
{
    if (! followingEnvelope)
        report (ParameterTouch::Kind::change, index, value);
}

void TrackEffects::Watcher::parameterGestureChanged (int index, bool starting)
{
    if (const auto found = byIndex.find (index); found != byIndex.end())
        report (starting ? ParameterTouch::Kind::begin : ParameterTouch::Kind::end, index, found->second.first->getValue());
}

void TrackEffects::Watcher::report (ParameterTouch::Kind kind, int index, float value)
{
    const auto found = byIndex.find (index);

    if (found == byIndex.end())
        return;

    // Effects may report from any thread, the audio thread included: queue it briefly.
    {
        const juce::SpinLock::ScopedLockType sl (owner.touchesLock);
        owner.touches.push_back ({ kind, trackId, slotId, found->second.second, value });
    }

    owner.triggerAsyncUpdate();
}

void TrackEffects::handleAsyncUpdate()
{
    std::vector<ParameterTouch> pending;

    {
        const juce::SpinLock::ScopedLockType sl (touchesLock);
        std::swap (pending, touches);
    }

    if (onParameterTouched != nullptr)
        for (auto& t : pending)
            onParameterTouched (t);
}

void TrackEffects::setTouched (juce::uint32 trackId, juce::uint32 slotId, const juce::String& parameterId, bool isTouched)
{
    const juce::ScopedLock sl (lock);

    if (isTouched)
        touched.insert ({ trackId, slotId, parameterId });
    else
        touched.erase ({ trackId, slotId, parameterId });
}

void TrackEffects::prepareSlot (Slot& slot) const
{
    prepareInstance (*slot.plugin);

    for (auto& c : slot.copies)
        if (c != nullptr)
            prepareInstance (*c);

    auto& p = *slot.plugin;
    slot.work.setSize (juce::jmax (2, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()), maxBlock);
}

void TrackEffects::prepareInstance (juce::AudioPluginInstance& p) const
{
    p.enableAllBuses();

    // Stereo in and out where the plug-in takes it; otherwise whatever it has.
    if (p.getBusCount (true) > 0)
        p.setChannelLayoutOfBus (true, 0, juce::AudioChannelSet::stereo());
    if (p.getBusCount (false) > 0)
        p.setChannelLayoutOfBus (false, 0, juce::AudioChannelSet::stereo());

    p.setRateAndBufferSizeDetails (rate, maxBlock);
    p.prepareToPlay (rate, maxBlock);
}

TrackEffects::Copies TrackEffects::copiesOf (Slot& slot) const
{
    Copies made;

    if (makeCopy == nullptr)
        return made;

    juce::MemoryBlock state;
    slot.plugin->getStateInformation (state);

    for (auto& c : made)
    {
        c = makeCopy (slot.plugin->getPluginDescription());

        if (c == nullptr)
            return {};

        c->setNonRealtime (slot.plugin->isNonRealtime());
        prepareInstance (*c);
        c->setStateInformation (state.getData(), (int) state.getSize());
    }

    return made;
}

void TrackEffects::install (juce::uint32 trackId, Slot* slot, Copies made)
{
    {
        const juce::ScopedLock sl (lock);
        const auto* chain = find (trackId);

        // Only if the slot is still there, on a 5.1 track.
        if (chain != nullptr && wide.count (trackId) != 0
            && std::any_of (chain->begin(), chain->end(), [slot] (auto& s) { return s.get() == slot; }))
            std::swap (slot->copies, made);
    }

    release (made);
}

juce::StringArray TrackEffects::setWideTracks (const std::set<juce::uint32>& tracks)
{
    std::vector<std::pair<juce::uint32, Slot*>> needed;
    std::vector<Copies> dropped;

    {
        const juce::ScopedLock sl (lock);
        wide = tracks;

        for (auto& [id, chain] : chains)
            for (auto& s : chain)
            {
                if (wide.count (id) != 0 && s->copies[0] == nullptr)
                    needed.emplace_back (id, s.get());
                else if (wide.count (id) == 0 && s->copies[0] != nullptr)
                    dropped.push_back (std::move (s->copies));
            }
    }

    for (auto& c : dropped)
        release (c);

    // Made here, off the lock; slots are added and removed only on this thread.
    juce::StringArray failed;

    for (auto& [id, slot] : needed)
    {
        auto made = copiesOf (*slot);

        if (made[0] == nullptr)
            failed.add (slot->plugin->getName());
        else
            install (id, slot, std::move (made));
    }

    return failed;
}

void TrackEffects::refreshCopies (const juce::AudioProcessor& processor)
{
    juce::uint32 trackId = 0;
    Slot* found = nullptr;

    {
        const juce::ScopedLock sl (lock);

        for (auto& [id, chain] : chains)
            for (auto& s : chain)
                if (s->plugin.get() == &processor && s->copies[0] != nullptr)
                {
                    trackId = id;
                    found = s.get();
                }
    }

    if (found != nullptr)
        if (auto made = copiesOf (*found); made[0] != nullptr)
            install (trackId, found, std::move (made));
}

void TrackEffects::followParameters (juce::AudioPluginInstance& original, juce::AudioPluginInstance& copy)
{
    const auto& from = original.getParameters();
    const auto& to = copy.getParameters();

    for (int i = 0; i < juce::jmin (from.size(), to.size()); ++i)
        if (const auto value = from[i]->getValue(); ! juce::exactlyEqual (value, to[i]->getValue()))
            to[i]->setValue (value);
}

int TrackEffects::getNumCopies (juce::uint32 trackId, int slot) const
{
    const juce::ScopedLock sl (lock);
    const auto* chain = find (trackId);

    if (chain == nullptr || ! juce::isPositiveAndBelow (slot, (int) chain->size()))
        return 0;

    const auto& copies = (*chain)[(size_t) slot]->copies;
    return (int) std::count_if (copies.begin(), copies.end(), [] (auto& c) { return c != nullptr; });
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
            release (slot->copies);
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
        {
            slot->plugin->releaseResources();
            release (slot->copies);
        }
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
    bool onWide = false;

    {
        const juce::ScopedLock sl (lock);
        onWide = wide.count (trackId) != 0;
    }

    if (onWide)
        slot->copies = copiesOf (*slot);

    const juce::ScopedLock sl (lock);
    slot->id = slotId != 0 ? slotId : nextSlotId;
    nextSlotId = juce::jmax (nextSlotId, slot->id + 1);
    slot->watcher = std::make_unique<Watcher> (*this, trackId, slot->id, slot->parameters);
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
    release (slot->copies);
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
        {
            slot.plugin->reset();

            for (auto& c : slot.copies)
                if (c != nullptr)
                    c->reset();
        }

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

bool TrackEffects::takesSidechain (const juce::AudioPluginInstance& p)
{
    return p.getBusCount (true) > 1 && p.getChannelCountOfBus (true, 1) > 0;
}

bool TrackEffects::hasSidechain (juce::uint32 trackId, int slot) const
{
    const auto* plugin = getPlugin (trackId, slot);
    return plugin != nullptr && takesSidechain (*plugin);
}

int TrackEffects::getLatencyBefore (juce::uint32 trackId, juce::uint32 slotId)
{
    const juce::ScopedTryLock sl (lock);
    const auto* chain = sl.isLocked() ? find (trackId) : nullptr;

    if (chain == nullptr)
        return -1;

    int before = 0;

    for (auto& slot : *chain)
    {
        if (slot->id == slotId)
            return slot->bypassed || ! takesSidechain (*slot->plugin) ? -1 : before;

        if (! slot->bypassed)
            before += juce::jmax (0, slot->plugin->getLatencySamples());
    }

    return -1;
}

void TrackEffects::reset (juce::uint32 trackId)
{
    const juce::ScopedTryLock sl (lock);

    if (auto* chain = sl.isLocked() ? find (trackId) : nullptr)
        for (auto& slot : *chain)
        {
            slot->plugin->reset();

            for (auto& c : slot->copies)
                if (c != nullptr)
                    c->reset();
        }
}

void TrackEffects::process (const SessionTrack& track, juce::int64 frame, juce::AudioBuffer<float>& buffer, int numFrames,
                            const std::vector<SessionMixer::KeyAudio>& keys)
{
    const juce::ScopedTryLock sl (lock);
    auto* chain = sl.isLocked() ? find (track.id) : nullptr;

    if (chain == nullptr)
    {
        buffer.clear (0, numFrames);
        return;
    }

    // A 5.1 track's six channels go through as three pairs: L R, C LFE, Ls Rs.
    const auto pairs = track.fiveOne && buffer.getNumChannels() >= 6 ? 3 : 1;

    // Steps line up with session time, so the result doesn't depend on how the mix is split into blocks.
    const auto automated = std::any_of (track.effectEnvelopes.begin(), track.effectEnvelopes.end(), [chain] (const EffectEnvelope& e)
    {
        return std::any_of (chain->begin(), chain->end(), [&e] (auto& s) { return s->id == e.slotId && ! s->bypassed; });
    });

    // One plug-in over one pair of the buffer's channels, from `first`.
    const auto run = [&] (juce::AudioPluginInstance& p, Slot& s, int first, int done, int n)
    {
        juce::AudioBuffer<float> work (s.work.getArrayOfWritePointers(), s.work.getNumChannels(), n);
        work.clear();
        const auto ins = juce::jmin (2, juce::jmax (1, p.getBusCount (true) > 0 ? p.getChannelCountOfBus (true, 0) : 0));

        // The key, if the slot has one, into the sidechain input (a mono one gets the average).
        if (takesSidechain (p))
        {
            const auto key = std::find_if (keys.begin(), keys.end(), [&s] (auto& k) { return k.slotId == s.id; });

            if (key != keys.end() && key->audio != nullptr)
            {
                const auto firstKey = p.getChannelIndexInProcessBlockBuffer (true, 1, 0);
                const auto count = p.getChannelCountOfBus (true, 1);

                if (count == 1)
                {
                    work.addFrom (firstKey, 0, *key->audio, 0, done, n, 0.5f);
                    work.addFrom (firstKey, 0, *key->audio, 1, done, n, 0.5f);
                }
                else
                {
                    work.copyFrom (firstKey, 0, *key->audio, 0, done, n);
                    work.copyFrom (firstKey + 1, 0, *key->audio, 1, done, n);
                }
            }
        }

        // A mono input gets the average of the two sides.
        work.copyFrom (0, 0, buffer, first, done, n);

        if (ins == 1)
        {
            work.addFrom (0, 0, buffer, first + 1, done, n);
            work.applyGain (0, 0, n, 0.5f);
        }
        else
        {
            work.copyFrom (1, 0, buffer, first + 1, done, n);
        }

        midi.clear();
        p.processBlock (work, midi);

        // A mono output plays on both sides.
        const auto outs = p.getTotalNumOutputChannels();
        buffer.copyFrom (first, done, work, 0, 0, n);
        buffer.copyFrom (first + 1, done, work, outs >= 2 ? 1 : 0, 0, n);
    };

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
                            if (touched.empty() || touched.count ({ track.id, s->id, e.parameterId }) == 0)
                            {
                                const juce::ScopedValueSetter<bool> following (followingEnvelope, true);
                                found->second->setValue (e.envelope.valueAt (stepStart - delayed));
                            }

            delayed += juce::jmax (0, s->plugin->getLatencySamples());

            for (int pair = 0; pair < pairs; ++pair)
            {
                auto* p = pair == 0 ? s->plugin.get() : s->copies[(size_t) pair - 1].get();

                if (p == nullptr)
                {
                    buffer.clear (2 * pair, done, n);
                    buffer.clear (2 * pair + 1, done, n);
                    continue;
                }

                if (pair > 0)
                    followParameters (*s->plugin, *p);

                run (*p, *s, 2 * pair, done, n);
            }
        }

        done += n;
    }
}
