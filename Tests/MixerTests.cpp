// Levels are checked against what the mixer itself computes.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/MixerView.h"
#include "TestUtils.h"
#include "MixerTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    AudioDocument::Snapshot constant (int channels, int frames, float value)
    {
        juce::AudioBuffer<float> b (channels, frames);

        for (int ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::fill (b.getWritePointer (ch), value * (float) (ch + 1), frames);

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, frames } };
        return std::make_shared<const AudioSnapshot> (channels, std::move (segments));
    }

    /** A mono track at -6 dB panned left a little, a stereo track sending to a bus, the bus, and
        a muted track. */
    SessionState mixSession()
    {
        SessionState s;
        s.sampleRate = rate;
        auto& mono = s.addTrack();
        mono.gainDb = -6.0f;
        mono.pan = -0.3f;
        auto& stereo = s.addTrack();
        stereo.pan = 0.5f;
        auto& bus = s.addBus();
        bus.gainDb = -3.0f;
        const auto busId = bus.id;
        s.addTrack().mute = true;
        SessionEdits::setSend (s, s.tracks[1].id, { busId, -10.0f, false });
        SessionEdits::addClip (s, 0, juce::File ("/takes/mono.wav"), constant (1, 9000, 0.5f), 0);
        SessionEdits::addClip (s, 1, juce::File ("/takes/stereo.wav"), constant (2, 9000, 0.2f), 0);
        SessionEdits::addClip (s, 3, juce::File ("/takes/muted.wav"), constant (1, 9000, 0.9f), 0);
        return s;
    }

    void testLevels()
    {
        const auto s = mixSession();
        SessionMixer::Levels levels;
        SessionMixer::Buffers buffers;
        buffers.levels = &levels;
        juce::AudioBuffer<float> out (2, 4096);
        SessionMixer::render (s, 100, out, 0, 4096, buffers);

        // What the mixer does, step by step: pan into the track, then its volume.
        const auto [monoLeft, monoRight] = SessionMixer::panGains (-0.3f, 1);
        const auto monoVolume = juce::Decibels::decibelsToGain (-6.0f, -150.0f);
        const auto [stereoLeft, stereoRight] = SessionMixer::panGains (0.5f, 2);
        const auto sendGain = juce::Decibels::decibelsToGain (-10.0f, -150.0f);
        const auto [busLeft, busRight] = SessionMixer::panGains (0.0f, 2);
        const auto busVolume = juce::Decibels::decibelsToGain (-3.0f, -150.0f);

        const auto mono = levels.take (0), stereo = levels.take (1), bus = levels.take (2), muted = levels.take (3);
        check (mono.first == (0.5f * monoLeft) * monoVolume && mono.second == (0.5f * monoRight) * monoVolume,
               "meters: a mono track's peaks are its level after its pan and volume (" + juce::String (mono.first, 4) + ", "
                   + juce::String (mono.second, 4) + ")");
        check (stereo.first == 0.2f * stereoLeft && stereo.second == 0.4f * stereoRight,
               "meters: a stereo track's peaks follow its balance, per side");
        check (bus.first == ((0.2f * stereoLeft) * sendGain) * (busVolume * busLeft) && bus.second == ((0.4f * stereoRight) * sendGain) * (busVolume * busRight),
               "meters: a bus's peaks are what its sends bring, after its volume");
        check (muted.first == 0.0f && muted.second == 0.0f && levels.take (0).first == 0.0f,
               "meters: a muted track shows nothing, and taking the peaks starts them again from 0");

        // Automated: the peak is the largest level the envelope gives.
        auto automated = s;
        automated.tracks[0].volumeEnvelope.points = { { 0, -20.0f }, { 4196, 0.0f } };
        SessionMixer::Levels ramped;
        SessionMixer::Buffers rampBuffers;
        rampBuffers.levels = &ramped;
        SessionMixer::render (automated, 100, out, 0, 4096, rampBuffers);
        const auto peak = ramped.take (0).first;
        check (peak > (0.5f * monoLeft) * 0.95f && peak <= 0.5f * monoLeft,
               "meters: under a volume envelope, the peak follows the envelope (" + juce::String (peak, 4) + ")");

        SessionMixer::Levels outside;
        outside.raise (SessionMixer::Levels::maxTracks, 1.0f, 1.0f);
        check (outside.take (SessionMixer::Levels::maxTracks).first == 0.0f && outside.take (-1).first == 0.0f,
               "meters: a track past the last metered place is ignored");
    }

    void testMixerView (AudioEngine& engine)
    {
        engine.session.reset (mixSession(), {});
        MixerView mixer (engine);
        mixer.setSize (MixerView::stripWidth * 6, 400);
        const auto state = engine.session.getState();
        const auto mono = state->tracks[0].id, stereo = state->tracks[1].id, bus = state->tracks[2].id;

        check (mixer.getNumStrips() == 4 && mixer.findFader (mono) != nullptr && mixer.findFader (bus) != nullptr
                   && mixer.findFader (12345) == nullptr && juce::exactlyEqual ((float) mixer.findFader (mono)->getValue(), -6.0f)
                   && juce::exactlyEqual (mixer.findPan (mono)->getValue(), -30.0) && mixer.findMute (state->tracks[3].id)->getToggleState(),
               "mixer: a strip for each track and bus, showing its volume, pan and mute");

        mixer.findFader (stereo)->setValue (-4.5, juce::sendNotificationSync);
        check (juce::exactlyEqual (engine.session.getState()->tracks[1].gainDb, -4.5f) && engine.session.getUndoDescription() == "Track Volume",
               "mixer: moving a fader sets the track's volume, as an undo step");

        mixer.findPan (bus)->setValue (40.0, juce::sendNotificationSync);
        mixer.findSolo (stereo)->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        const auto after = engine.session.getState();
        check (juce::exactlyEqual (after->tracks[2].pan, 0.4f) && after->tracks[1].solo && engine.session.getUndoDescription() == "Solo",
               "mixer: pan and solo work on buses and tracks alike");

        engine.session.undo();
        engine.session.undo();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (! mixer.findSolo (stereo)->getToggleState() && juce::exactlyEqual (mixer.findPan (bus)->getValue(), 0.0),
               "mixer: undo shows on the strips");

        // Meters: the player's levels, falling back once the sound stops.
        auto& levels = engine.getPlayer().getLevels();

        for (int i = 0; i < SessionMixer::Levels::maxTracks; ++i)
            levels.take (i);

        levels.raise (1, 0.5f, 0.25f);
        mixer.updateMeters();
        const auto shown = mixer.getShownLevel (stereo, 0), shownRight = mixer.getShownLevel (stereo, 1);
        mixer.updateMeters();
        const auto fallen = mixer.getShownLevel (stereo, 0);
        check (shown == 0.5f && shownRight == 0.25f && fallen < shown && fallen > 0.25f && mixer.getShownLevel (mono, 0) == 0.0f,
               "mixer: a strip's meter shows its track's peak, then falls back (" + juce::String (fallen, 3) + ")");

        // An automated volume can't be moved here; a new track gets a strip.
        auto next = *engine.session.getState();
        next.tracks[0].volumeEnvelope.points = { { 0, -12.0f } };
        next.addTrack();
        engine.session.apply (std::move (next), "Test");
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (! mixer.findFader (mono)->isEnabled() && mixer.findFader (stereo)->isEnabled() && mixer.getNumStrips() == 5,
               "mixer: an automated volume follows its envelope; a new track gets a strip");

        engine.session.reset ({}, {});
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        check (mixer.getNumStrips() == 0, "mixer: an empty session has no strips");
    }
}

void runMixerTests (AudioEngine& engine)
{
    testLevels();
    testMixerView (engine);
}
