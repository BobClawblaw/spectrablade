#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/BuiltInEffects.h"
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
    auto instance = descriptions.size() == 1 ? engine.pluginFormatManager.createPluginInstance (descriptions[0], rate, 512, error) : nullptr;
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
    check (list.getNumTypes() == 1 && list.getTypes()[0].fileOrIdentifier == BuiltInEffects::parametricEqId && format != nullptr
               && ! format->canScanForPlugins() && format->searchPathsForPlugins ({}, true, false).isEmpty(),
           "built-in effects: listed once however often added; their format has nothing to scan");

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
        made.reset();
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
}
