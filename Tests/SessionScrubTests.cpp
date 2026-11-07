// What scrubbing a session plays is compared bit for bit with scrubbing the same audio as a file.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/BuiltInEffects.h"
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
    using TestPlugins::DelayPlugin;

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
    juce::AudioBuffer<float> scrubSession (const SessionState& state, SessionMixer::Inserts& inserts, bool effects = true)
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
            source.scrub (s, rate, view, view.getNumSamples(), effects);
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

        // Its effects (a gain that halves it) heard, or left out; its volume (-6 dB) and a muted
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
        const auto withoutEffects = scrubSession (quieter, effects, false);
        const auto g = juce::Decibels::decibelsToGain (-6.0f);
        const auto off = juce::jmax (worst (withEffects, file, 0.5f * g), worst (withoutEffects, file, g));

        check (same (plain, file) && file.getMagnitude (0, 0, frames) > 0.4f && off < 1.0e-6f,
               "scrubbing a session: a track's clip scrubs bit for bit as the file of it would (forward, back and fast ahead, in "
               "blocks of 256); with -6 dB of volume it is that much quieter and its effect (a gain of a half) is heard, or left out "
               "when asked (within " + juce::String (off, 9) + "), and a muted track beside it isn't heard");

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

namespace
{
    std::unique_ptr<juce::AudioPluginInstance> builtIn (const char* id)
    {
        static juce::AudioPluginFormatManager formats;

        if (formats.getNumFormats() == 0)
            formats.addFormat (BuiltInEffects::createFormat());

        for (auto& d : BuiltInEffects::getDescriptions())
            if (d.fileOrIdentifier == id)
            {
                juce::String error;
                return formats.createPluginInstance (d, rate, TrackEffects::maxBlock, error);
            }

        return {};
    }

    /** A parametric EQ (a 9 dB bell at 1 kHz) on the track: a filter, with memory. */
    void addEq (TrackEffects& effects, juce::uint32 id)
    {
        effects.prepare (rate);
        effects.add (id, builtIn (BuiltInEffects::parametricEqId));
        BuiltInEffects::setExactly (dynamic_cast<BuiltInEffects::ParametricEqProcessor&> (*effects.getPlugin (id, 0)).gainParameter (2), 9.0f);
    }

    /** The session's mix played from frame `from` on (silence before), through fresh effects. */
    juce::AudioBuffer<float> played (const SessionState& s, juce::int64 from, SessionMixer::Inserts& inserts)
    {
        juce::AudioBuffer<float> out (2, (int) s.getLength());
        out.clear();
        SessionMixer::Buffers buffers;
        buffers.reserve (512);
        buffers.surroundChannel = -1;
        buffers.interfaceChannel = -1;
        SessionMixer::prime (s, from, buffers, inserts);

        for (auto p = from; p < out.getNumSamples(); p += 512)
            SessionMixer::render (s, p, out, (int) p, (int) juce::jmin ((juce::int64) 512, out.getNumSamples() - p), buffers, &inserts);

        return out;
    }

    void testEffects()
    {
        // Noise through an EQ: scrubbed, against scrubbing the file of the mix as played from where
        // the scrub first primed its effects (the first window, 1028 frames before the head, less
        // the warm-up).
        juce::Random r (9);
        juce::AudioBuffer<float> raw (2, (int) (6.0 * rate));

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < raw.getNumSamples(); ++i)
                raw.setSample (ch, i, (r.nextFloat() - 0.5f) * 0.6f);

        const auto s = oneClip (fromBuffer (raw));
        const auto id = s.tracks[0].id;
        TrackEffects eq, reference;
        addEq (eq, id);
        addEq (reference, id);
        const auto primedAt = targetAt (0) - (juce::int64) (Scrubber::fastest * block + 4.0) - SessionSource::scrubWarmUp;
        const auto file = scrubFile (*fromBuffer (played (s, primedAt, reference)));
        const auto scrubbed = scrubSession (s, eq);
        const auto dry = scrubFile (*fromBuffer (juce::AudioBuffer<float> (raw)));
        const auto second = (int) rate;
        float forward = 0.0f;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < second; ++i)
                forward = juce::jmax (forward, std::abs (scrubbed.getSample (ch, i) - file.getSample (ch, i)));

        const auto all = worst (scrubbed, file), effect = worst (dry, file);

        check (forward < 1.0e-6f && all < 1.0e-3f && effect > 0.1f,
               "scrubbing a session with effects: through an EQ, dragging forward sounds as the mix played from where the scrub "
               "began (within " + juce::String (forward, 9) + "); back and fast ahead, with the EQ warmed up on the 4096 frames "
               "before each window, within " + juce::String (all, 9) + " of it, where the EQ itself changes the sound by "
               + juce::String (effect, 2));

        // An effect with latency (a 240-frame delay, made up for) scrubs in time, bit for bit as without it.
        TrackEffects delayed, none;
        delayed.prepare (rate);
        none.prepare (rate);
        delayed.add (id, std::make_unique<DelayPlugin> (240, 1.0f));
        check (same (scrubSession (s, delayed), scrubSession (s, none)),
               "scrubbing a session with effects: an effect with 240 frames of latency scrubs in time, forward and back, bit for bit "
               "as with no effect");

        // An edit while scrubbing is heard from then on: what was kept is rendered again.
        {
            TrackEffects gain;
            gain.prepare (rate);
            gain.add (id, std::make_unique<GainPlugin>());
            Session session;
            session.reset (s, {});
            SessionSource source (session, gain);
            source.prepareToPlay (block, rate);
            auto quieter = s;
            quieter.tracks[0].gainDb = -6.0f;
            const auto expected = scrubSession (quieter, gain);
            Scrubber sc;
            sc.prepare (block);
            sc.start (targetAt (0));
            juce::AudioBuffer<float> out (2, frames);
            out.clear();
            const auto changeAt = 40 * block;

            for (int done = 0; done < frames; done += block)
            {
                if (done == changeAt)
                    session.apply (quieter, "Volume");

                sc.moveTo (targetAt (done));
                juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, done, block);
                source.scrub (sc, rate, view, block);
            }

            float after = 0.0f, before = 0.0f;

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < frames; ++i)
                    (i < changeAt ? before : after) = juce::jmax (i < changeAt ? before : after, std::abs (out.getSample (ch, i) - expected.getSample (ch, i)));

            check (after < 1.0e-6f && before > 0.01f,
                   "scrubbing a session with effects: a volume change made while scrubbing forward is heard from the next block on "
                   "(within " + juce::String (after, 9) + " of scrubbing it changed from the start), not only in what is rendered after");
        }

        // Playing after a scrub starts the effects afresh, as playing without one does.
        {
            TrackEffects scrubbedEq, freshEq;
            addEq (scrubbedEq, id);
            addEq (freshEq, id);
            Session session;
            session.reset (s, {});
            SessionSource source (session, scrubbedEq), fresh (session, freshEq);
            source.prepareToPlay (block, rate);
            fresh.prepareToPlay (block, rate);
            Scrubber sc;
            sc.prepare (block);
            sc.start (targetAt (0));
            juce::AudioBuffer<float> buffer (2, block), a (2, 4 * block), b (2, 4 * block);

            for (int done = 0; done < 20 * block; done += block)
            {
                sc.moveTo (targetAt (done));
                buffer.clear();
                source.scrub (sc, rate, buffer, block);
            }

            for (auto* src : { &source, &fresh })
            {
                auto& into = src == &source ? a : b;
                src->setNextReadPosition (150000);

                for (int done = 0; done < 4 * block; done += block)
                {
                    juce::AudioSourceChannelInfo info (&into, done, block);
                    src->getNextAudioBlock (info);
                }
            }

            check (same (a, b) && a.getMagnitude (0, 0, 4 * block) > 0.01f,
                   "scrubbing a session with effects: playing after a scrub sounds bit for bit as playing without one, the effects "
                   "started afresh");
        }
    }

    void testEffectsCost()
    {
        // 16 tracks, each with an EQ and a compressor: a second of scrubbing at four times normal
        // speed forward, then back (each block's window primed and warmed up again).
        SessionState s;
        s.sampleRate = rate;
        TrackEffects effects;
        effects.prepare (rate);

        for (int t = 0; t < 16; ++t)
        {
            auto& track = s.addTrack();
            track.gainDb = -12.0f;
            SessionClip c;
            c.id = s.nextId++;
            c.audio = tone (2, 200.0 + 20.0 * t, 20.0);
            c.length = c.audio->getNumFrames();
            track.clips.push_back (c);
            effects.add (track.id, builtIn (BuiltInEffects::parametricEqId));
            effects.add (track.id, builtIn (BuiltInEffects::compressorId));
        }

        Session session;
        session.reset (s, {});
        SessionSource source (session, effects);
        source.prepareToPlay (block, rate);
        juce::AudioBuffer<float> out (2, block);
        double seconds[2] {};

        for (int way = 0; way < 2; ++way)
        {
            Scrubber scrubber;
            scrubber.prepare (block);
            juce::int64 target = way == 0 ? 0 : 8 * (juce::int64) rate;
            scrubber.start (target);
            const auto began = juce::Time::getMillisecondCounterHiRes();

            for (int done = 0; done < (int) rate; done += block)
            {
                target += (way == 0 ? 4 : -4) * block;
                scrubber.moveTo (target);
                out.clear();
                source.scrub (scrubber, rate, out, block);
            }

            seconds[way] = (juce::Time::getMillisecondCounterHiRes() - began) / 1000.0;
        }

        check (seconds[0] < 0.5 && seconds[1] < 1.0,
               "scrubbing a session with effects: a second of scrubbing 16 tracks, each through an EQ and a compressor, at four "
               "times normal speed takes " + juce::String (seconds[0] * 1000.0, 1) + " ms forward and "
               + juce::String (seconds[1] * 1000.0, 1) + " ms back (the effects warmed up again for each block's window)");
    }
}

void runSessionScrubTests (AudioEngine& engine)
{
    testEffects();
    testEffectsCost();
    testCost();
    testMix();
    testPlayer();
    testView (engine);
}
