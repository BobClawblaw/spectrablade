#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/BuiltInEffects.h"
#include "../Source/TrackEffects.h"
#include "../Source/RackView.h"
#include "../Source/MultitrackView.h"
#include "TestUtils.h"
#include "BuiltInEffectsTests.h"

namespace
{
    using TestUtils::check;
    using TestUtils::runUntil;
    using Equalizer::Band;
    using Equalizer::BandType;
    using BuiltInEffects::ParametricEqProcessor;

    constexpr double rate = 48000.0;

    juce::AudioBuffer<float> noise (int channels, int frames, int seed)
    {
        juce::Random random (seed);
        juce::AudioBuffer<float> b (channels, frames);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, 0.5f * (random.nextFloat() * 2.0f - 1.0f));

        return b;
    }

    /** Plays the buffer through the processor in blocks of 1, 7, 512 and 3,000 frames in turn. */
    juce::AudioBuffer<float> play (juce::AudioProcessor& p, const juce::AudioBuffer<float>& in)
    {
        juce::AudioBuffer<float> out (in);
        juce::MidiBuffer midi;
        const int sizes[] = { 1, 7, 512, 3000 };
        int pos = 0;

        for (int k = 0; pos < out.getNumSamples(); ++k)
        {
            const auto n = juce::jmin (sizes[k % 4], out.getNumSamples() - pos);
            juce::AudioBuffer<float> block (out.getArrayOfWritePointers(), out.getNumChannels(), pos, n);
            p.processBlock (block, midi);
            pos += n;
        }

        return out;
    }

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    bool identical (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
            return false;

        for (int ch = 0; ch < a.getNumChannels(); ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
                if (! juce::exactlyEqual (a.getSample (ch, i), b.getSample (ch, i)))
                    return false;

        return true;
    }

    void setUp (ParametricEqProcessor& eq)
    {
        eq.setBand (0, true, { BandType::highPass, 80.0, 0.0, 0.7071067811865476 });
        eq.setBand (2, true, { BandType::peak, 1000.0, 6.0, 2.0 });
        eq.setBand (4, true, { BandType::highShelf, 8000.0, -3.0, 0.7071067811865476 });
    }
}

void runBuiltInEffectsTests (AudioEngine& engine)
{
    // Created by description, through the app's own format manager.
    const auto descriptions = BuiltInEffects::getDescriptions();
    juce::String error;
    auto instance = descriptions.size() == 17 ? engine.pluginFormatManager.createPluginInstance (descriptions[0], rate, 512, error) : nullptr;
    auto* eq = dynamic_cast<ParametricEqProcessor*> (instance.get());
    check (eq != nullptr && error.isEmpty() && eq->getPluginDescription().fileOrIdentifier == BuiltInEffects::parametricEqId
               && eq->getPluginDescription().pluginFormatName == BuiltInEffects::formatName && eq->getActiveBands (rate).empty(),
           "built-in parametric eq: created from its description by the app's format manager, flat at first");

    if (eq == nullptr)
        return;

    // Block by block against the offline EQ over a whole file.
    setUp (*eq);
    eq->setPlayConfigDetails (2, 2, rate, 512);
    eq->prepareToPlay (rate, 3000);
    const auto input = noise (2, 100000, 7);
    const auto live = play (*eq, input);
    const auto bands = eq->getActiveBands (rate);
    const auto offline = readAll (*Equalizer::apply (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (input)), 0, 100000, rate, bands));
    check (bands.size() == 3 && identical (live, offline),
           "built-in parametric eq: three bands played in blocks of 1, 7, 512 and 3,000 frames give the offline EQ's samples bit for bit");

    // Its state, saved and loaded into a new one, plays the same.
    juce::MemoryBlock state;
    eq->getStateInformation (state);
    ParametricEqProcessor copy;
    copy.setStateInformation (state.getData(), (int) state.getSize());
    copy.setPlayConfigDetails (2, 2, rate, 512);
    copy.prepareToPlay (rate, 3000);
    auto sameBands = true;

    for (int i = 0; i < ParametricEqProcessor::numBands; ++i)
    {
        const auto a = eq->getBands()[(size_t) i], b = copy.getBands()[(size_t) i];
        sameBands = sameBands && a.first == b.first && a.second.type == b.second.type && juce::exactlyEqual (a.second.frequency, b.second.frequency)
                 && juce::exactlyEqual (a.second.gainDb, b.second.gainDb) && juce::exactlyEqual (a.second.q, b.second.q);
    }

    check (sameBands && identical (play (copy, input), offline), "built-in parametric eq: its state restores every band exactly, and plays the same");

    // Switching a band off passes the audio through it; switching it back starts it afresh.
    eq->reset();
    eq->setBand (0, false, eq->getBands()[0].second);
    eq->setBand (2, false, eq->getBands()[2].second);
    eq->setBand (4, false, eq->getBands()[4].second);
    const auto passed = play (*eq, input);
    eq->setBand (2, true, { BandType::peak, 1000.0, 6.0, 2.0 });
    const auto back = play (*eq, input);
    const auto onlyBell = readAll (*Equalizer::apply (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (input)), 0, 100000, rate,
                                                      { eq->getActiveBands (rate) }));
    check (identical (passed, input) && eq->getActiveBands (rate).size() == 1 && identical (back, onlyBell),
           "built-in parametric eq: every band off passes the audio bit for bit; a band switched back on plays as if from the start");

    // Mono, and a band at Nyquist for the rate is passed by.
    ParametricEqProcessor mono;
    juce::AudioProcessor::BusesLayout monoLayout;
    monoLayout.inputBuses.add (juce::AudioChannelSet::mono());
    monoLayout.outputBuses.add (juce::AudioChannelSet::mono());
    const auto monoOk = mono.setBusesLayout (monoLayout);
    mono.setBand (1, true, { BandType::peak, 15000.0, 6.0, 1.0 });
    mono.prepareToPlay (22050.0, 512);
    const auto monoIn = noise (1, 5000, 3);
    check (monoOk && mono.getActiveBands (22050.0).empty() && identical (play (mono, monoIn), monoIn),
           "built-in parametric eq: runs mono; a 15 kHz band at 22.05 kHz is past Nyquist and passes the audio unchanged");

    // The plug-in list takes it once however often it is added, and the scanner has nothing to scan for it.
    juce::KnownPluginList list;
    BuiltInEffects::addTo (list);
    BuiltInEffects::addTo (list);
    auto* format = [&]
    {
        for (auto* f : engine.pluginFormatManager.getFormats())
            if (f->getName() == BuiltInEffects::formatName)
                return f;

        return (juce::AudioPluginFormat*) nullptr;
    }();
    check (list.getNumTypes() == 17 && format != nullptr
               && ! format->canScanForPlugins() && format->searchPathsForPlugins ({}, true, false).isEmpty(),
           "built-in effects: the seventeen listed once each however often added; their format has nothing to scan");

    // Its window: the EQ panel on the parameters, both ways.
    {
        ParametricEqProcessor shownEq;
        std::unique_ptr<juce::AudioProcessorEditor> made (shownEq.createEditorAndMakeActive());
        auto* editor = dynamic_cast<BuiltInEffects::ParametricEqEditor*> (made.get());
        auto gestures = 0;

        struct Gestures final : juce::AudioProcessorParameter::Listener
        {
            int& count;
            explicit Gestures (int& c) : count (c) {}
            void parameterValueChanged (int, float) override {}
            void parameterGestureChanged (int, bool starting) override   { count += starting ? 1 : 0; }
        } listener (gestures);

        shownEq.gainParameter (2).addListener (&listener);
        auto panelToEq = false, eqToPanel = false, untouched = true;

        if (editor != nullptr)
        {
            auto& row = editor->getPanel().getRow (2);
            row.gain.setValue (4.5, juce::sendNotificationSync);
            row.q.setValue (3.0, juce::sendNotificationSync);
            const auto band = shownEq.getBands()[2].second;
            panelToEq = shownEq.getBands()[2].first && juce::exactlyEqual (band.gainDb, 4.5) && std::abs (band.q - 3.0) < 1.0e-5
                     && shownEq.getActiveBands (rate).size() == 1 && gestures == 2;

            for (int i = 0; i < ParametricEqProcessor::numBands; ++i)
                untouched = untouched && (i == 2 || juce::exactlyEqual (shownEq.getBands()[(size_t) i].second.gainDb, 0.0));

            shownEq.setBand (0, true, { BandType::highPass, 120.0, 0.0, 0.7071067811865476 });
            editor->refresh();
            const auto& s = editor->getPanel().getSettings()[0];
            eqToPanel = s.band.type == BandType::highPass && std::abs (s.band.frequency - 120.0) < 1.0e-3
                     && ! editor->getPanel().getRow (0).gain.isEnabled() && std::abs (editor->getPanel().getRow (0).frequency.getValue() - 120.0) < 0.05;
        }

        shownEq.gainParameter (2).removeListener (&listener);
        check (editor != nullptr && panelToEq && untouched && eqToPanel,
               "built-in parametric eq window: the EQ panel; its controls set that band's parameters, each move one gesture, and leave the other bands; a band changed from outside shows on the panel");

        // A host deleting the editor tells the processor, as the plug-in windows do: it holds the
        // editor as its active one until then, and makes another once told.
        const auto wasActive = shownEq.getActiveEditor() == made.get();
        shownEq.editorBeingDeleted (made.get());
        const auto letGo = shownEq.getActiveEditor() == nullptr;
        made.reset();
        std::unique_ptr<juce::AudioProcessorEditor> again (shownEq.createEditorAndMakeActive());
        check (wasActive && letGo && dynamic_cast<BuiltInEffects::ParametricEqEditor*> (again.get()) != nullptr,
               "built-in parametric eq window: the processor holds the editor as active until told it has gone, then makes another");
        shownEq.editorBeingDeleted (again.get());
        again.reset();
    }

    // The compressor, live, against the offline one: noise stepping between -30 and -6 dB.
    {
        auto steps = noise (2, 96000, 11);

        for (int i = 0; i < steps.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch)
                steps.setSample (ch, i, steps.getSample (ch, i) * ((i / 12000) % 2 == 0 ? 0.06f : 1.0f));

        BuiltInEffects::CompressorProcessor comp;
        BuiltInEffects::setExactly (*comp.threshold, -30.0f);
        BuiltInEffects::setExactly (*comp.ratio, 6.0f);
        BuiltInEffects::setExactly (*comp.attack, 5.0f);
        BuiltInEffects::setExactly (*comp.release, 80.0f);
        BuiltInEffects::setExactly (*comp.makeup, 3.0f);
        comp.setPlayConfigDetails (2, 2, rate, 512);
        comp.prepareToPlay (rate, 3000);
        const auto liveComp = play (comp, steps);
        const auto offlineComp = readAll (*Dynamics::compress (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (steps)), 0, 96000, rate, comp.getSettings()));

        juce::MemoryBlock compState;
        comp.getStateInformation (compState);
        BuiltInEffects::CompressorProcessor compCopy;
        compCopy.setStateInformation (compState.getData(), (int) compState.getSize());
        const auto a = comp.getSettings(), b = compCopy.getSettings();
        const auto compRestored = juce::exactlyEqual (a.thresholdDb, b.thresholdDb) && juce::exactlyEqual (a.ratio, b.ratio) && juce::exactlyEqual (a.kneeDb, b.kneeDb)
                               && juce::exactlyEqual (a.attackMs, b.attackMs) && juce::exactlyEqual (a.releaseMs, b.releaseMs) && juce::exactlyEqual (a.makeupDb, b.makeupDb);
        check (identical (liveComp, offlineComp) && ! identical (liveComp, steps) && compRestored && comp.getLatencySamples() == 0,
               "built-in compressor: played in blocks of 1, 7, 512 and 3,000 frames it gives Compressor's samples bit for bit; its state restores every setting exactly; no latency");
    }

    // The limiter, live: a look-ahead late, exactly the offline limiter.
    {
        auto hot = noise (2, 48000, 5);
        hot.applyGain (6.0f);   // peaks up to about +9.5 dBFS

        BuiltInEffects::LimiterProcessor lim;
        BuiltInEffects::setExactly (*lim.ceiling, -1.0f);
        BuiltInEffects::setExactly (*lim.gain, 2.0f);
        lim.setPlayConfigDetails (2, 2, rate, 512);
        lim.prepareToPlay (rate, 3000);
        const auto ahead = lim.getLatencySamples();
        juce::AudioBuffer<float> padded (2, 48000 + ahead);
        padded.clear();

        for (int ch = 0; ch < 2; ++ch)
            padded.copyFrom (ch, 0, hot, ch, 0, 48000);

        const auto liveLim = play (lim, padded);
        const auto offlineLim = readAll (*Dynamics::limit (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (hot)), 0, 48000, rate, lim.getSettings()));
        auto lateExactly = true, leadingSilence = true;
        float loudest = 0.0f;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < padded.getNumSamples(); ++i)
            {
                loudest = juce::jmax (loudest, std::abs (liveLim.getSample (ch, i)));

                if (i < ahead)
                    leadingSilence = leadingSilence && juce::exactlyEqual (liveLim.getSample (ch, i), 0.0f);
                else
                    lateExactly = lateExactly && juce::exactlyEqual (liveLim.getSample (ch, i), offlineLim.getSample (ch, i - ahead));
            }

        check (ahead == 240 && leadingSilence && lateExactly && (double) loudest <= Dynamics::ceilingGain (lim.getSettings().ceilingDb) && std::abs (lim.getSettings().ceilingDb + 1.0) < 1.0e-5,
               "built-in limiter: reports its 5 ms look-ahead (240 frames at 48 kHz) as latency, and its output that late is Limiter's bit for bit, never above the ceiling it holds (-1 dB to 1e-5)");

        // On a track, the chain's latency counts it, so the track is read ahead by that much.
        TrackEffects effects;
        effects.prepare (rate);
        effects.add (7, std::make_unique<BuiltInEffects::LimiterProcessor>());
        effects.add (7, std::make_unique<BuiltInEffects::CompressorProcessor>());
        check (effects.getChainLatency (7) == 240, "built-in limiter on a track: the chain reports its 240 frames of latency (the compressor adds none)");
    }

    // Chorus, flanger and phaser, live, against the offline effects, with settings away from the defaults.
    {
        const auto in = noise (2, 96000, 21);
        const auto offlineOf = [&in] (const auto& settings)
        {
            const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));

            if constexpr (std::is_same_v<std::decay_t<decltype (settings)>, Modulation::Phaser>)
                return readAll (*Modulation::phaser (*snap, 0, 96000, rate, settings));
            else
                return readAll (*Modulation::sweptDelay (*snap, 0, 96000, rate, settings));
        };
        const auto prepared = [] (juce::AudioProcessor& p)
        {
            p.setPlayConfigDetails (2, 2, rate, 512);
            p.prepareToPlay (rate, 3000);
        };

        BuiltInEffects::SweptDelayProcessor chorus (BuiltInEffects::SweptDelayProcessor::Kind::chorus);
        chorus.setSettings ({ 1.5, 12.0, 7.0, 0.2, 40.0, 120.0 });
        prepared (chorus);
        BuiltInEffects::SweptDelayProcessor flanger (BuiltInEffects::SweptDelayProcessor::Kind::flanger);
        prepared (flanger);
        BuiltInEffects::PhaserProcessor phaser;
        phaser.setSettings ({ 0.7, 200.0, 4000.0, 8, -0.5, 60.0, 45.0 });
        prepared (phaser);

        const auto chorusLive = play (chorus, in), flangerLive = play (flanger, in), phaserLive = play (phaser, in);
        const auto f = flanger.getSettings(), fd = Modulation::flangerDefaults();
        const auto flangerStarts = std::abs (f.delayMs - fd.delayMs) < 1.0e-5 && std::abs (f.feedback - fd.feedback) < 1.0e-6;

        juce::MemoryBlock phaserState;
        phaser.getStateInformation (phaserState);
        BuiltInEffects::PhaserProcessor phaserCopy;
        phaserCopy.setStateInformation (phaserState.getData(), (int) phaserState.getSize());
        const auto a = phaser.getSettings(), b = phaserCopy.getSettings();
        const auto restored = juce::exactlyEqual (a.rateHz, b.rateHz) && juce::exactlyEqual (a.lowHz, b.lowHz) && juce::exactlyEqual (a.highHz, b.highHz)
                           && a.stages == b.stages && a.stages == 8 && juce::exactlyEqual (a.feedback, b.feedback)
                           && juce::exactlyEqual (a.mixPercent, b.mixPercent) && juce::exactlyEqual (a.spreadDegrees, b.spreadDegrees);

        check (identical (chorusLive, offlineOf (chorus.getSettings())) && identical (flangerLive, offlineOf (flanger.getSettings()))
                   && identical (phaserLive, offlineOf (phaser.getSettings())) && ! identical (chorusLive, in) && flangerStarts && restored
                   && chorus.getLatencySamples() == 0 && phaser.getLatencySamples() == 0,
               "built-in chorus, flanger and phaser: played in blocks of 1, 7, 512 and 3,000 frames, each gives the offline effect's samples bit for bit; "
               "the flanger starts at its own defaults; the phaser's state (8 stages) restores exactly; no latency");
    }

    // The reverb, live, against the offline one over the whole file.
    {
        const auto in = noise (2, 96000, 23);
        BuiltInEffects::ReverbProcessor reverb;
        reverb.setSettings ({ 35.0, 1.5, 80.0, 45.0, 70.0, 40.0 });
        reverb.setPlayConfigDetails (2, 2, rate, 512);
        reverb.prepareToPlay (rate, 3000);
        const auto reverbLive = play (reverb, in);
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        const auto reverbOffline = readAll (*RoomReverb::apply (*snap, 0, 96000, rate, reverb.getSettings()));
        const auto s = reverb.getSettings();
        check (identical (reverbLive, reverbOffline) && ! identical (reverbLive, in) && reverb.getLatencySamples() == 0
                   && std::abs (reverb.getTailLengthSeconds() - (s.preDelayMs / 1000.0 + s.decaySeconds)) < 1.0e-12,
               "built-in reverb: played in blocks of 1, 7, 512 and 3,000 frames it gives the offline reverb's samples bit for bit; no latency; "
               "its tail is the pre-delay plus the decay");
    }

    // The convolution reverb, live: a block late, exactly the offline one; its impulse kept in its state.
    {
        const auto in = noise (2, 30000, 25);
        Convolver::Impulse impulse { noise (2, 4000, 26), rate, "room" };
        BuiltInEffects::ConvolutionProcessor conv;
        conv.setPlayConfigDetails (2, 2, rate, 512);
        conv.prepareToPlay (rate, 3000);
        BuiltInEffects::setExactly (*conv.mix, 45.0f);
        conv.setImpulse (impulse);
        const auto convAhead = conv.getLatencySamples();
        juce::AudioBuffer<float> padded (2, 30000 + convAhead);
        padded.clear();

        for (int ch = 0; ch < 2; ++ch)
            padded.copyFrom (ch, 0, in, ch, 0, 30000);

        const auto convLive = play (conv, padded);
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        const auto convOffline = readAll (*Convolver::apply (*snap, 0, 30000, rate, impulse, 45.0));
        auto lateExactly = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 30000; ++i)
                lateExactly = lateExactly && juce::exactlyEqual (convLive.getSample (ch, i + convAhead), convOffline.getSample (ch, i));

        juce::MemoryBlock convState;
        conv.getStateInformation (convState);
        BuiltInEffects::ConvolutionProcessor convCopy;
        convCopy.setStateInformation (convState.getData(), (int) convState.getSize());
        const auto& restoredImpulse = convCopy.getImpulse();
        auto samplesKept = restoredImpulse.samples.getNumChannels() == 2 && restoredImpulse.samples.getNumSamples() == 4000;

        for (int ch = 0; ch < 2 && samplesKept; ++ch)
            for (int i = 0; i < 4000; ++i)
                samplesKept = samplesKept && juce::exactlyEqual (restoredImpulse.samples.getSample (ch, i), impulse.samples.getSample (ch, i));

        // With no impulse, the sound passes through a block late.
        BuiltInEffects::ConvolutionProcessor empty;
        empty.setPlayConfigDetails (2, 2, rate, 512);
        empty.prepareToPlay (rate, 512);
        const auto emptyOut = play (empty, padded);
        auto through = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 30000; ++i)
                through = through && juce::exactlyEqual (emptyOut.getSample (ch, i + convAhead), in.getSample (ch, i));

        check (convAhead == Convolver::blockSize && lateExactly && samplesKept && restoredImpulse.name == "room" && juce::exactlyEqual (convCopy.mix->get(), conv.mix->get())
                   && through && std::abs (conv.getTailLengthSeconds() - 4000.0 / rate) < 1.0e-12,
               "built-in convolution reverb: reports 512 frames of latency and, that late, gives Convolver's samples bit for bit; its state keeps the impulse's "
               "samples and name and the mix exactly; with no impulse the sound passes through 512 frames late; its tail is the impulse's length");
    }

    // Added through the rack view, the new effect asks for its window at once; through a track's
    // FX menu, the same for the track's new effect.
    {
        engine.clearRack();
        RackView rackView (engine);
        auto edited = -1, done = 0;
        rackView.onEditSlot = [&] (int slot) { edited = slot; ++done; };
        rackView.addEffect (descriptions[0]);
        runUntil ([&] { return done > 0; }, 10000);
        rackView.addEffect (descriptions[0]);
        runUntil ([&] { return done > 1; }, 10000);
        check (done == 2 && edited == 1 && engine.getNumRackSlots() == 2,
               "a new rack effect asks for its window as soon as it is in (the second added, slot 1)");

        SessionState session;
        session.sampleRate = rate;
        const auto trackId = session.addTrack().id;
        engine.session.reset (session, {});
        MultitrackView view (engine);
        juce::AudioPluginInstance* opened = nullptr;
        view.onEditPlugin = [&] (juce::AudioPluginInstance& p) { opened = &p; };
        view.addTrackEffect (trackId, descriptions[0]);
        runUntil ([&] { return opened != nullptr; }, 10000);
        check (opened != nullptr && opened == engine.getTrackEffects().getPlugin (trackId, 0),
               "a new track effect asks for its window as soon as it is in");
        engine.session.reset ({}, {});
    }

    // In the master rack: added by description, set, saved and restored with its bands.
    engine.clearRack();
    auto added = false;
    engine.addPlugin (descriptions[0], [&] (const juce::String& e) { error = e; added = true; });
    runUntil ([&] { return added; }, 10000);
    auto* inRack = engine.getNumRackSlots() == 1 ? dynamic_cast<ParametricEqProcessor*> (engine.getRackPlugin (0)) : nullptr;

    if (inRack != nullptr)
        setUp (*inRack);

    const auto rackXml = engine.createRackXml();
    engine.clearRack();
    auto loaded = false;
    engine.loadRackXml (*rackXml, [&] (const juce::String& e) { error += e; loaded = true; });
    runUntil ([&] { return loaded; }, 10000);
    auto* restored = engine.getNumRackSlots() == 1 ? dynamic_cast<ParametricEqProcessor*> (engine.getRackPlugin (0)) : nullptr;
    check (added && inRack != nullptr && loaded && error.isEmpty() && restored != nullptr && restored->getActiveBands (rate).size() == 3
               && juce::exactlyEqual (restored->getBands()[2].second.gainDb, 6.0),
           "built-in parametric eq in the rack: added by description, and saved and restored with its bands");
    engine.clearRack();

    // Preview: the dialogs' settings, set on the live effects, read back exactly, at the ends of
    // the offline ranges as well as the defaults.
    {
        // A skewed range can't give every float, so each is held to the nearest it gives.
        const auto near = [] (double held, double wanted) { return std::abs (held - wanted) <= 1.0e-6 * juce::jmax (1.0, std::abs (wanted)); };
        auto exactCompressor = true;

        for (const auto& s : { Dynamics::Compressor {}, Dynamics::Compressor { -80.0, 100.0, 0.0, 0.01, 5000.0, -48.0 },
                               Dynamics::Compressor { -12.5, 2.5, 24.0, 1000.0, 1.0, 48.0 }, Dynamics::Compressor { -30.0, 6.0, 3.0, 5.0, 80.0, 3.0 } })
        {
            BuiltInEffects::CompressorProcessor c;
            c.setSettings (s);
            const auto g = c.getSettings();
            exactCompressor = exactCompressor && near (g.thresholdDb, s.thresholdDb) && near (g.ratio, s.ratio) && near (g.kneeDb, s.kneeDb)
                           && near (g.attackMs, s.attackMs) && near (g.releaseMs, s.releaseMs) && near (g.makeupDb, s.makeupDb);
        }

        auto exactLimiter = true;

        for (const auto& s : { Dynamics::Limiter {}, Dynamics::Limiter { -48.0, -48.0, 1.0, 0.1, false }, Dynamics::Limiter { 48.0, 0.0, 5000.0, 50.0, false },
                               Dynamics::Limiter { 6.0, -0.5, 120.0, 20.0, false } })
        {
            BuiltInEffects::LimiterProcessor l (s.lookaheadMs);
            l.setSettings (s);
            l.setPlayConfigDetails (2, 2, rate, 512);
            l.prepareToPlay (rate, 512);
            const auto g = l.getSettings();
            exactLimiter = exactLimiter && near (g.inputGainDb, s.inputGainDb) && near (g.ceilingDb, s.ceilingDb) && near (g.releaseMs, s.releaseMs)
                        && juce::exactlyEqual (g.lookaheadMs, s.lookaheadMs)
                        && l.getLatencySamples() == Dynamics::lookaheadFrames (s, rate);
        }

        check (exactCompressor && exactLimiter,
               "built-in effects for preview: the compressor and limiter take the default, extreme and typical dialog settings to within a millionth "
               "(the nearest their ranges hold); the limiter takes each look-ahead (0.1 to 50 ms) as given, its latency as many frames as Limiter's");
    }

    {
        const auto slotsBefore = engine.getNumRackSlots();
        engine.setPreviewEffect (std::make_unique<BuiltInEffects::CompressorProcessor>());
        const auto first = dynamic_cast<BuiltInEffects::CompressorProcessor*> (engine.getPreviewEffect()) != nullptr;
        engine.setPreviewEffect (std::make_unique<BuiltInEffects::LimiterProcessor> (20.0));
        auto* lim = dynamic_cast<BuiltInEffects::LimiterProcessor*> (engine.getPreviewEffect());
        const auto swapped = lim != nullptr && lim->getSampleRate() > 0.0;
        const auto savedRack = engine.createRackXml();
        const auto notInRack = engine.getNumRackSlots() == slotsBefore && savedRack != nullptr && savedRack->getNumChildElements() == slotsBefore;
        engine.setPreviewEffect (nullptr);
        check (first && swapped && notInRack && engine.getPreviewEffect() == nullptr,
               "preview slot: an effect put in front of the rack is replaced by the next (given the engine's rate) and cleared; "
               "it is never a rack slot or saved with the rack");
    }
}
