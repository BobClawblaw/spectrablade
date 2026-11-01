// The effects are expected to apply exactly the envelope's steps, so mixes are compared bit for bit.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/SessionMixer.h"
#include "../Source/TrackEffects.h"
#include "TestPlugins.h"
#include "TestUtils.h"
#include "EffectAutomationTests.h"

namespace
{
    using TestUtils::check;
    using TestPlugins::DelayPlugin;
    using TestPlugins::GainPlugin;

    constexpr double rate = 48000.0;
    constexpr int length = 9000;
    constexpr int step = TrackEffects::automationStep;

    AudioDocument::Snapshot noise (int frames, int seed)
    {
        juce::Random r (seed);
        juce::AudioBuffer<float> b (2, frames);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, r.nextFloat() * 0.5f - 0.25f);

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, frames } };
        return std::make_shared<const AudioSnapshot> (2, std::move (segments));
    }

    SessionState oneTrack()
    {
        SessionState s;
        s.sampleRate = rate;
        auto& t = s.addTrack();
        SessionClip c;
        c.id = s.nextId++;
        c.audio = noise (length, 91);
        c.length = length;
        t.clips.push_back (c);
        return s;
    }

    juce::AudioBuffer<float> play (const SessionState& s, juce::int64 start, int frames, int blockSize, SessionMixer::Inserts* inserts)
    {
        juce::AudioBuffer<float> out (2, frames);
        SessionMixer::Buffers buffers;
        buffers.reserve (blockSize);

        if (inserts != nullptr)
            SessionMixer::prime (s, start, buffers, *inserts);

        for (int p = 0; p < frames; p += blockSize)
            SessionMixer::render (s, start + p, out, p, juce::jmin (blockSize, frames - p), buffers, inserts);

        return out;
    }

    bool same (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
                if (a.getSample (ch, i) != b.getSample (ch, i))
                    return false;

        return true;
    }

    /** The dry mix scaled frame by frame as the gain plug-in would be. With `delayed` frames of
        latency before it in the chain, it works on audio frame a at the chain's input frame
        a + delayed, in the 64-frame step starting at stepStart, set from the envelope at
        stepStart - delayed. */
    juce::AudioBuffer<float> expected (const juce::AudioBuffer<float>& dry, const Envelope& e, int delayed)
    {
        auto out = dry;

        for (int i = 0; i < out.getNumSamples(); ++i)
        {
            const auto stepStart = (((juce::int64) i + delayed) / step) * step;
            const auto g = 2.0f * e.valueAt (stepStart - delayed);

            for (int ch = 0; ch < 2; ++ch)
                out.setSample (ch, i, out.getSample (ch, i) * g);
        }

        return out;
    }
}

void runEffectAutomationTests (juce::AudioFormatManager& formats)
{
    auto s = oneTrack();
    const auto trackId = s.tracks[0].id;
    const auto dry = play (s, 0, length, length, nullptr);
    const Envelope ramp { { { 500, 0.0f }, { 8000, 1.0f } } };

    // A gain effect alone, its parameter on a ramp: it steps every 64 frames of session time.
    {
        TrackEffects effects;
        effects.prepare (rate);
        const auto slot = effects.add (trackId, std::make_unique<GainPlugin>());
        auto automated = s;
        check (SessionEdits::setEnvelope (automated, trackId, AutomationTarget::effect (slot, "gain"), ramp)
                   && automated.tracks[0].isAutomated() && automated.tracks[0].effectEnvelopes.size() == 1,
               "effect automation: an envelope is set on an effect's parameter");

        const auto want = expected (dry, ramp, 0);
        auto allSame = true;

        for (auto blockSize : { 1, 37, 512, 3000 })
            allSame = allSame && same (play (automated, 0, length, blockSize, &effects), want);

        check (allSame, "effect automation: the effect follows its envelope in 64-frame steps of session time, bit for bit in 1-, 37-, 512- and 3000-frame blocks");

        // Played from part way in, after priming.
        const auto later = play (automated, 4321, 3000, 256, &effects);
        juce::AudioBuffer<float> wantLater (2, 3000);

        for (int ch = 0; ch < 2; ++ch)
            wantLater.copyFrom (ch, 0, want, ch, 4321, 3000);

        check (same (later, wantLater), "effect automation: playing from 4321 matches");

        // Without the envelope the effect keeps the value it was left at.
        auto* gain = dynamic_cast<GainPlugin*> (effects.getPlugin (trackId, 0));
        gain->gain->value = 0.5f;
        const auto changes = gain->gain->changes;
        check (same (play (s, 0, length, 512, &effects), dry) && gain->gain->changes == changes,
               "effect automation: with no envelope the parameter is left alone");
    }

    // After a 300-frame delay in the chain, the gain works on audio 300 frames old, and follows
    // its envelope from there. Moved ahead of the delay, its envelope goes with it.
    {
        TrackEffects effects;
        effects.prepare (rate);
        effects.add (trackId, std::make_unique<DelayPlugin> (300, 1.0f));
        const auto slot = effects.add (trackId, std::make_unique<GainPlugin>());
        auto automated = s;
        SessionEdits::setEnvelope (automated, trackId, AutomationTarget::effect (slot, "gain"), ramp);

        check (same (play (automated, 0, length, 512, &effects), expected (dry, ramp, 300)),
               "effect automation: after 300 frames of latency the envelope still lines up with the audio, bit for bit");

        effects.move (trackId, 1, 0);
        check (effects.getSlotId (trackId, 0) == slot && same (play (automated, 0, length, 333, &effects), expected (dry, ramp, 0)),
               "effect automation: moved to the front of the chain, the effect keeps its envelope");

        effects.setBypassed (trackId, 0, true);
        check (same (play (automated, 0, length, 512, &effects), dry), "effect automation: a bypassed effect's envelope does nothing");
    }

    // Touches in an effect's window are reported in order; the envelope's own values are not,
    // even from an effect that echoes every change. A touched parameter is left to the hand.
    {
        TrackEffects effects;
        effects.prepare (rate);
        const auto slot = effects.add (trackId, std::make_unique<GainPlugin>());
        auto* gain = dynamic_cast<GainPlugin*> (effects.getPlugin (trackId, 0))->gain;
        gain->echo = true;
        std::vector<TrackEffects::ParameterTouch> heard;
        effects.onParameterTouched = [&heard] (const TrackEffects::ParameterTouch& t) { heard.push_back (t); };

        gain->beginChangeGesture();
        gain->setValueNotifyingHost (0.8f);
        gain->endChangeGesture();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

        using Kind = TrackEffects::ParameterTouch::Kind;
        const auto is = [&] (size_t i, Kind k, float v)
        {
            return heard[i].kind == k && heard[i].trackId == trackId && heard[i].slotId == slot && heard[i].parameterId == "gain"
                && heard[i].value == v;
        };
        // The echo from setValue counts as a change too, so the hand's move is heard once or twice.
        check (heard.size() >= 3 && is (0, Kind::begin, 0.5f) && is (1, Kind::change, 0.8f) && is (heard.size() - 1, Kind::end, 0.8f),
               "effect automation: grabbing, turning and letting go of a parameter in its window is reported in order");

        heard.clear();
        auto automated = s;
        SessionEdits::setEnvelope (automated, trackId, AutomationTarget::effect (slot, "gain"), ramp);
        const auto followed = play (automated, 0, length, 512, &effects);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        check (heard.empty() && same (followed, expected (dry, ramp, 0)),
               "effect automation: values from an envelope are not reported as touches, even when the effect echoes them");

        effects.setTouched (trackId, slot, "gain", true);
        gain->value = 0.25f;
        const auto changes = gain->changes;
        auto halved = dry;
        halved.applyGain (0.5f);
        check (same (play (automated, 0, length, 512, &effects), halved) && gain->changes == changes,
               "effect automation: a touched parameter is held off its envelope");

        effects.setTouched (trackId, slot, "gain", false);
        check (same (play (automated, 0, length, 512, &effects), expected (dry, ramp, 0)), "effect automation: let go, it follows its envelope again");
    }

    // An envelope whose effect has gone is kept but unused; values stay in range; clearing removes it.
    {
        TrackEffects effects;
        effects.prepare (rate);
        effects.add (trackId, std::make_unique<GainPlugin>());
        auto orphaned = s;
        SessionEdits::setEnvelope (orphaned, trackId, AutomationTarget::effect (999, "gain"), { { { 0, 1.5f }, { 10, -2.0f } } });
        const auto& kept = orphaned.tracks[0].envelope (AutomationTarget::effect (999, "gain"));
        check (same (play (orphaned, 0, length, 512, &effects), dry) && kept.points.size() == 2
                   && kept.points[0].value == 1.0f && kept.points[1].value == 0.0f,
               "effect automation: an envelope for an effect not in the chain is kept (in range) and plays nothing");

        SessionEdits::setEnvelope (orphaned, trackId, AutomationTarget::effect (999, "gain"), {});
        check (orphaned.tracks[0].effectEnvelopes.empty() && ! orphaned.tracks[0].isAutomated(),
               "effect automation: clearing an effect envelope removes it");
    }

    // Slot ids: new ones count up, saved ones are kept, and they go into the chain's XML.
    {
        TrackEffects effects;
        effects.prepare (rate);
        const auto first = effects.add (trackId, std::make_unique<GainPlugin>());
        const auto saved = effects.add (trackId, std::make_unique<GainPlugin>(), 40);
        const auto next = effects.add (7, std::make_unique<GainPlugin>());
        const auto xml = effects.createXml (trackId);
        check (first == 1 && saved == 40 && next == 41 && effects.findSlot (trackId, 40) == effects.getPlugin (trackId, 1)
                   && xml != nullptr && xml->getChildElement (1)->getIntAttribute ("id") == 40,
               "effect automation: slots keep their ids, new ones don't repeat them, and the chain's XML has them");
    }

    // Session files keep effect envelopes exactly.
    {
        auto withEnvelopes = s;
        SessionEdits::setEnvelope (withEnvelopes, trackId, AutomationTarget::effect (3, "gain"), ramp);
        SessionEdits::setEnvelope (withEnvelopes, trackId, AutomationTarget::effect (4, "Mix:Wet"), { { { 0, 0.25f }, { 96000, 0.75f } } });
        const auto file = juce::File::createTempFile (".sbsession");
        const auto error = SessionFile::save (withEnvelopes, file);
        const auto loaded = SessionFile::load (file, formats);
        file.deleteFile();
        check (error.isEmpty() && loaded.error.isEmpty() && loaded.state.tracks.size() == 1
                   && loaded.state.tracks[0].effectEnvelopes == withEnvelopes.tracks[0].effectEnvelopes,
               "effect automation: effect envelopes come back from a session file exactly");
    }
}
