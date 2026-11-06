// What scrubbing a session plays is compared bit for bit with scrubbing the same audio as a file.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/MultitrackView.h"
#include "../Source/Surround.h"
#include "../Source/TrackEffects.h"
#include "TestPlugins.h"
#include "TestUtils.h"
#include "SessionScrubTests.h"

namespace
{
    using TestUtils::check;
    using TestPlugins::GainPlugin;

    constexpr double rate = 48000.0;
    constexpr int block = 256;

    /** A tone of `hz` at 0.5 on the first channel, 0.25 on the second, and so on down, `seconds` long. */
    AudioDocument::Snapshot tone (int channels, double hz, double seconds)
    {
        juce::AudioBuffer<float> b (channels, (int) (seconds * rate));

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (ch, i, (float) (0.5 / (1 + ch) * std::sin (juce::MathConstants<double>::twoPi * hz * (1.0 + 0.1 * ch) * i / rate)));

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, (juce::int64) (seconds * rate) } };
        return std::make_shared<const AudioSnapshot> (channels, std::move (segments));
    }

    AudioDocument::Snapshot fromBuffer (juce::AudioBuffer<float> b)
    {
        const auto frames = (juce::int64) b.getNumSamples();
        const auto channels = b.getNumChannels();
        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, frames } };
        return std::make_shared<const AudioSnapshot> (channels, std::move (segments));
    }

    /** A session of one track playing `audio` from frame 0, with no fades. */
    SessionState oneClip (AudioDocument::Snapshot audio)
    {
        SessionState s;
        s.sampleRate = rate;
        s.surround = audio->getNumChannels() == Surround::fiveOneChannels;
        auto& t = s.addTrack();
        t.fiveOne = s.surround;
        SessionClip c;
        c.id = s.nextId++;
        c.audio = std::move (audio);
        c.length = c.audio->getNumFrames();
        t.clips.push_back (c);
        return s;
    }

    /** The target for each block: dragged forward at normal speed for a second, back at one and a
        half times for half a second, then ahead fast, from 2 s. */
    juce::int64 targetAt (int done)
    {
        const auto second = (int) rate;

        if (done < second)
            return 2 * second + done;

        if (done < second + second / 2)
            return 3 * second - (juce::int64) (1.5 * (done - second));

        return (juce::int64) (2.25 * second) + 3 * (done - second - second / 2);
    }

    constexpr int frames = 2 * (int) rate;

    /** Scrubbing a file's audio over the moves of targetAt. */
    juce::AudioBuffer<float> scrubFile (const AudioSnapshot& audio)
    {
        Scrubber s;
        s.prepare (block);
        s.start (targetAt (0));
        juce::AudioBuffer<float> out (2, frames);
        out.clear();

        for (int done = 0; done < frames; done += block)
        {
            s.moveTo (targetAt (done));
            s.render (audio, rate, rate, out, done, juce::jmin (block, frames - done));
        }

        return out;
    }

    /** Scrubbing a session's mix the same way, as the player does it when stopped. */
    juce::AudioBuffer<float> scrubSession (const SessionState& state, SessionMixer::Inserts& inserts)
    {
        Session session;
        session.reset (state, {});
        SessionSource source (session, inserts);
        source.prepareToPlay (block, rate);
        Scrubber s;
        s.prepare (block);
        s.start (targetAt (0));
        juce::AudioBuffer<float> out (2, frames);
        out.clear();

        for (int done = 0; done < frames; done += block)
        {
            s.moveTo (targetAt (done));
            juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, done, juce::jmin (block, frames - done));
            source.scrub (s, rate, view, view.getNumSamples());
        }

        return out;
    }

    bool same (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < 2; ++ch)
            if (std::memcmp (a.getReadPointer (ch), b.getReadPointer (ch), sizeof (float) * (size_t) a.getNumSamples()) != 0)
                return false;

        return true;
    }

    float worst (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, float scale = 1.0f)
    {
        float most = 0.0f;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
                most = juce::jmax (most, std::abs (a.getSample (ch, i) - scale * b.getSample (ch, i)));

        return most;
    }

    void testMix()
    {
        const auto audio = tone (2, 440.0, 6.0);
        const auto s = oneClip (audio);
        const auto id = s.tracks[0].id;
        const auto file = scrubFile (*audio);
        TrackEffects none;
        none.prepare (rate);

        // One clip alone: what scrubbing the file of it gives.
        const auto plain = scrubSession (s, none);

        // Its effects (a gain that would halve it) are left out; its volume (-6 dB) and a muted
        // track beside it are heard as they are.
        TrackEffects effects;
        effects.prepare (rate);
        effects.add (id, std::make_unique<GainPlugin>());
        dynamic_cast<GainPlugin*> (effects.getPlugin (id, 0))->gain->value = 0.25f;
        auto quieter = s;
        quieter.tracks[0].gainDb = -6.0f;
        auto& muted = quieter.addTrack();
        muted.mute = true;
        SessionClip loud;
        loud.id = quieter.nextId++;
        loud.audio = tone (2, 1000.0, 6.0);
        loud.length = loud.audio->getNumFrames();
        muted.clips.push_back (loud);
        const auto withEffects = scrubSession (quieter, effects);
        const auto g = juce::Decibels::decibelsToGain (-6.0f);
        const auto off = worst (withEffects, file, g);

        check (same (plain, file) && file.getMagnitude (0, 0, frames) > 0.4f && off < 1.0e-6f,
               "scrubbing a session: a track's clip scrubs bit for bit as the file of it would (forward, back and fast ahead, in "
               "blocks of 256); with -6 dB of volume it is that much quieter (within " + juce::String (off, 9) + "), a muted track "
               "beside it isn't heard, and its effects are left out");

        // A frozen track scrubs its frozen audio; a 5.1 master is folded down.
        auto frozen = s;
        const auto frozenAudio = tone (2, 330.0, 6.0);
        SessionEdits::freezeTrack (frozen, id, juce::File(), frozenAudio);
        const auto wide = tone (6, 300.0, 6.0);
        juce::AudioBuffer<float> all (6, (int) wide->getNumFrames()), folded (2, (int) wide->getNumFrames());
        wide->read (all, 0, 0, all.getNumSamples());
        Surround::foldDown (all.getArrayOfReadPointers(), folded.getWritePointer (0), folded.getWritePointer (1), all.getNumSamples());

        check (same (scrubSession (frozen, none), scrubFile (*frozenAudio)) && same (scrubSession (oneClip (wide), none), scrubFile (*fromBuffer (folded))),
               "scrubbing a session: a frozen track scrubs its frozen audio, and a 5.1 track in a 5.1 master scrubs folded down, "
               "each bit for bit as a file of it would");
    }

    void testPlayer()
    {
        // The player scrubs the session while it is stopped and showing it, and only then.
        const auto audio = tone (2, 440.0, 6.0);
        AudioDocument document;
        Session session;
        session.reset (oneClip (audio), {});
        TrackEffects effects;
        effects.prepare (rate);
        DocumentPlayerProcessor player (document, session, effects);
        player.setMode (DocumentPlayerProcessor::Mode::session);
        player.prepareToPlay (rate, block);
        juce::AudioBuffer<float> buffer (2, block), out (2, frames);
        juce::MidiBuffer midi;
        auto& scrubber = player.getScrubber();
        scrubber.start (targetAt (0));

        for (int done = 0; done < frames; done += block)
        {
            scrubber.moveTo (targetAt (done));
            player.processBlock (buffer, midi);

            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, done, buffer, ch, 0, block);
        }

        scrubber.stop();
        player.setMode (DocumentPlayerProcessor::Mode::document);
        scrubber.start (0);
        scrubber.moveTo (48000);
        float inDocument = 0.0f;

        for (int i = 0; i < 20; ++i)
        {
            player.processBlock (buffer, midi);
            inDocument = juce::jmax (inDocument, buffer.getMagnitude (0, block));
        }

        scrubber.stop();
        player.releaseResources();

        check (same (out, scrubFile (*audio)) && inDocument == 0.0f,
               "scrubbing a session: the player, stopped and showing the session, plays the scrubbed mix bit for bit; showing the "
               "(empty) file instead, it scrubs that and the session isn't heard");
    }

    juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> at, juce::Point<float> down, bool dragged)
    {
        const auto now = juce::Time::getCurrentTime();
        return { juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys::leftButtonModifier, juce::MouseInputSource::defaultPressure,
                 juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                 juce::MouseInputSource::defaultTiltY, &c, &c, now, down, now, 1, dragged };
    }

    void testView (AudioEngine& engine)
    {
        auto& player = engine.getPlayer();
        const auto mode = player.getMode();
        player.setMode (DocumentPlayerProcessor::Mode::session);
        engine.session.reset (oneClip (tone (2, 440.0, 10.0)), {});
        MultitrackView view (engine);
        view.setSize (MultitrackView::headerWidth + 1000 + 12, MultitrackView::rulerHeight + 3 * MultitrackView::trackHeight + 12);
        view.zoomToFit();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        auto& scrubber = player.getScrubber();

        // Down on the time ruler and dragged: it scrubs, the cursor following; let go, it stops.
        const auto rulerY = (float) MultitrackView::timeRulerHeight / 2.0f;
        const juce::Point<float> from ((float) MultitrackView::headerWidth + 200.0f, rulerY), to ((float) MultitrackView::headerWidth + 400.0f, rulerY);
        view.mouseDown (mouseAt (view, from, from, false));
        const auto startedOn = scrubber.isOn();
        const auto startCursor = view.getCursor();
        view.mouseDrag (mouseAt (view, to, from, true));
        const auto movedCursor = view.getCursor();
        view.mouseUp (mouseAt (view, to, from, true));
        const auto stopped = ! scrubber.isOn();
        const auto noSelection = view.getTimeSelection().isEmpty();

        // The marker lane below it still selects a range.
        const auto laneY = (float) (MultitrackView::timeRulerHeight + MultitrackView::markerLaneHeight / 2);
        view.mouseDown (mouseAt (view, from.withY (laneY), from.withY (laneY), false));
        const auto laneScrubs = scrubber.isOn();
        view.mouseDrag (mouseAt (view, to.withY (laneY), from.withY (laneY), true));
        view.mouseUp (mouseAt (view, to.withY (laneY), from.withY (laneY), true));
        const auto selected = ! view.getTimeSelection().isEmpty();

        engine.session.reset (SessionState(), {});
        player.setMode (mode);

        check (startedOn && startCursor > 0 && movedCursor > startCursor && stopped && noSelection && ! laneScrubs && selected,
               "scrubbing a session: a drag along the multitrack view's time ruler while stopped scrubs, the cursor following the mouse "
               "(from " + juce::String ((double) startCursor / rate, 2) + " s to " + juce::String ((double) movedCursor / rate, 2)
               + " s), and stops when let go; a drag in the marker lane below it still selects a range");
    }
}

namespace
{
    void testCost()
    {
        // 32 stereo tracks, each with a clip at a different place, and two buses: the time a
        // second of scrubbing takes, dragged at four times normal speed (the most it reads).
        SessionState s;
        s.sampleRate = rate;
        const auto busA = s.addBus().id, busB = s.addBus().id;

        for (int t = 0; t < 32; ++t)
        {
            auto& track = s.addTrack();
            track.gainDb = -12.0f;
            track.pan = (float) (t % 5 - 2) * 0.3f;
            track.outputBusId = t % 3 == 0 ? busA : t % 3 == 1 ? busB : 0;
            SessionClip c;
            c.id = s.nextId++;
            c.audio = tone (2, 200.0 + 20.0 * t, 20.0);
            c.position = 4800 * t;
            c.length = c.audio->getNumFrames();
            c.fadeIn = c.fadeOut = 2400;
            track.clips.push_back (c);
        }

        TrackEffects none;
        none.prepare (rate);
        Session session;
        session.reset (s, {});
        SessionSource source (session, none);
        source.prepareToPlay (block, rate);
        Scrubber scrubber;
        scrubber.prepare (block);
        scrubber.start (0);
        juce::AudioBuffer<float> out (2, block);
        juce::int64 target = 0;
        const auto began = juce::Time::getMillisecondCounterHiRes();

        for (int done = 0; done < (int) rate; done += block)
        {
            target += 4 * block;
            scrubber.moveTo (target);
            out.clear();
            source.scrub (scrubber, rate, out, block);
        }

        const auto seconds = (juce::Time::getMillisecondCounterHiRes() - began) / 1000.0;

        check (seconds < 0.5,
               "scrubbing a session: a second of scrubbing 32 tracks and 2 buses at four times normal speed takes "
               + juce::String (seconds * 1000.0, 1) + " ms to work out (" + juce::String (100.0 * seconds, 1) + "% of real time)");
    }
}

void runSessionScrubTests (AudioEngine& engine)
{
    testCost();
    testMix();
    testPlayer();
    testView (engine);
}
