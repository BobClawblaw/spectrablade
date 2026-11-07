// What the effects make of each pair is compared bit for bit with the same effects on a stereo track.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/AudioFileIO.h"
#include "../Source/BuiltInEffects.h"
#include "../Source/Dynamics.h"
#include "../Source/OfflineRender.h"
#include "../Source/RackCopy.h"
#include "../Source/SessionMixer.h"
#include "../Source/TrackEffects.h"
#include "TestPlugins.h"
#include "TestUtils.h"
#include "SurroundEffectsTests.h"

namespace
{
    using TestUtils::check;
    using TestPlugins::DelayPlugin;
    using TestPlugins::GainPlugin;

    constexpr double rate = 48000.0;
    constexpr int frames = 6000;

    AudioDocument::Snapshot noise (int channels, int length, int seed)
    {
        juce::Random r (seed);
        juce::AudioBuffer<float> b (channels, length);

        // Each channel at its own level, so dynamics on one pair act differently from another's.
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < length; ++i)
                b.setSample (ch, i, (r.nextFloat() - 0.5f) * (0.2f + 0.25f * (float) ch));

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, length } };
        return std::make_shared<const AudioSnapshot> (channels, std::move (segments));
    }

    /** Channels [first, first + count) of a snapshot as a snapshot of their own. */
    AudioDocument::Snapshot channelsOf (const AudioSnapshot& audio, int first, int count)
    {
        juce::AudioBuffer<float> all (audio.getNumChannels(), (int) audio.getNumFrames()), part (count, (int) audio.getNumFrames());
        audio.read (all, 0, 0, all.getNumSamples());

        for (int ch = 0; ch < count; ++ch)
            part.copyFrom (ch, 0, all, first + ch, 0, all.getNumSamples());

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (part)), 0, audio.getNumFrames() } };
        return std::make_shared<const AudioSnapshot> (count, std::move (segments));
    }

    /** One track playing a clip at frame 300 with no fades: a 5.1 track in a 5.1 master, or with
        stereo audio a stereo track in a stereo master. */
    SessionState session (AudioDocument::Snapshot audio)
    {
        SessionState s;
        s.sampleRate = rate;
        s.surround = audio->getNumChannels() == 6;
        auto& t = s.addTrack();
        t.fiveOne = s.surround;
        SessionClip c;
        c.id = s.nextId++;
        c.audio = std::move (audio);
        c.position = 300;
        c.length = c.audio->getNumFrames();
        t.clips.push_back (c);
        return s;
    }

    juce::AudioBuffer<float> render (const SessionState& s, int blockSize, SessionMixer::Inserts* inserts)
    {
        const auto channels = s.surround ? 6 : 2;
        juce::AudioBuffer<float> out (channels, frames);
        SessionMixer::Buffers buffers;
        buffers.reserve (blockSize);
        buffers.surroundChannel = 2;
        buffers.interfaceChannel = -1;

        if (inserts != nullptr)
        {
            std::set<juce::uint32> wide;

            for (auto& t : s.tracks)
                if (t.fiveOne)
                    wide.insert (t.id);

            inserts->setWideTracks (wide);
            SessionMixer::prime (s, 0, buffers, *inserts);
        }

        for (int p = 0; p < frames; p += blockSize)
            SessionMixer::render (s, p, out, p, juce::jmin (blockSize, frames - p), buffers, inserts);

        return out;
    }

    bool same (const juce::AudioBuffer<float>& a, int chA, const juce::AudioBuffer<float>& b, int chB)
    {
        return std::memcmp (a.getReadPointer (chA), b.getReadPointer (chB), sizeof (float) * (size_t) a.getNumSamples()) == 0;
    }

    bool silent (const juce::AudioBuffer<float>& b, int ch)
    {
        return b.getMagnitude (ch, 0, b.getNumSamples()) == 0.0f;
    }

    std::unique_ptr<juce::AudioPluginInstance> make (juce::AudioPluginFormatManager& formats, const char* id)
    {
        for (auto& d : BuiltInEffects::getDescriptions())
            if (d.fileOrIdentifier == id)
            {
                juce::String error;
                return formats.createPluginInstance (d, rate, TrackEffects::maxBlock, error);
            }

        return {};
    }

    /** A compressor (hard) and a limiter (pushed into its ceiling), set as given. */
    void setUp (juce::AudioPluginInstance& compressor, juce::AudioPluginInstance& limiter, float threshold)
    {
        auto& c = dynamic_cast<BuiltInEffects::CompressorProcessor&> (compressor);
        BuiltInEffects::setExactly (*c.threshold, threshold);
        BuiltInEffects::setExactly (*c.ratio, 6.0f);
        BuiltInEffects::setExactly (*c.makeup, 6.0f);
        auto& l = dynamic_cast<BuiltInEffects::LimiterProcessor&> (limiter);
        BuiltInEffects::setExactly (*l.gain, 9.0f);
    }

    /** A parametric EQ (a 9 dB bell at 1 kHz) and a phaser: built-in effects that don't link channels. */
    void setUpPairs (juce::AudioPluginInstance& eq)
    {
        BuiltInEffects::setExactly (dynamic_cast<BuiltInEffects::ParametricEqProcessor&> (eq).gainParameter (2), 9.0f);
    }

    void testBuiltIns (AudioEngine& engine)
    {
        juce::AudioPluginFormatManager formats;
        formats.addFormat (BuiltInEffects::createFormat());
        const auto audio = noise (6, 4000, 41);
        const auto s = session (audio);
        const auto id = s.tracks[0].id;

        // The EQ is on the track before it is a 5.1 track, the phaser added after; the EQ's gain is
        // changed once its copies are there.
        TrackEffects effects;
        effects.prepare (rate);
        effects.makeCopy = [&formats] (const juce::PluginDescription& d)
        {
            juce::String error;
            return formats.createPluginInstance (d, rate, TrackEffects::maxBlock, error);
        };
        effects.add (id, make (formats, BuiltInEffects::parametricEqId));
        const auto failed = effects.setWideTracks ({ id });
        effects.add (id, make (formats, BuiltInEffects::phaserId));
        setUpPairs (*effects.getPlugin (id, 0));
        const auto copies = effects.getNumCopies (id, 0) == 2 && effects.getNumCopies (id, 1) == 2 && failed.isEmpty();
        const auto out = render (s, 97, &effects);

        // Each pair against the same two effects, fresh, on a stereo track holding that pair.
        auto pairsSame = true;
        float loudest = 0.0f;

        for (int pair = 0; pair < 3; ++pair)
        {
            const auto stereo = session (channelsOf (*audio, 2 * pair, 2));
            TrackEffects alone;
            alone.prepare (rate);
            alone.add (stereo.tracks[0].id, make (formats, BuiltInEffects::parametricEqId));
            alone.add (stereo.tracks[0].id, make (formats, BuiltInEffects::phaserId));
            setUpPairs (*alone.getPlugin (stereo.tracks[0].id, 0));
            const auto want = render (stereo, 97, &alone);
            pairsSame = pairsSame && same (out, 2 * pair, want, 0) && same (out, 2 * pair + 1, want, 1) && ! silent (want, 0);
        }

        auto changed = false;

        for (int ch = 0; ch < 6; ++ch)
            changed = changed || ! same (out, ch, render (s, 97, nullptr), ch);

        check (copies && pairsSame && changed,
               "5.1 track effects: a parametric EQ and a phaser on a 5.1 track run on L R, C LFE and Ls Rs as three stereo copies, "
               "each pair bit for bit what the same effects make of it on a stereo track (blocks of 97); copies are made when the "
               "track becomes 5.1 and for effects added after, and follow settings changed later");

        // The compressor and the limiter link the six: they hear them as one and turn them all down
        // together, as compressing and then limiting the six as one file does.
        {
            TrackEffects dynamics;
            dynamics.prepare (rate);
            dynamics.makeCopy = effects.makeCopy;
            dynamics.setWideTracks ({ id });
            dynamics.add (id, make (formats, BuiltInEffects::compressorId));
            dynamics.add (id, make (formats, BuiltInEffects::limiterId));
            setUp (*dynamics.getPlugin (id, 0), *dynamics.getPlugin (id, 1), -24.0f);
            const auto linked = render (s, 97, &dynamics);
            const auto dry = render (s, 97, nullptr);
            const auto& compressor = dynamic_cast<BuiltInEffects::CompressorProcessor&> (*dynamics.getPlugin (id, 0));
            const auto& limiter = dynamic_cast<BuiltInEffects::LimiterProcessor&> (*dynamics.getPlugin (id, 1));
            const auto drySnap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (dry));
            const auto compressed = Dynamics::compress (*drySnap, 0, frames, rate, compressor.getSettings());
            const auto limited = Dynamics::limit (*compressed, 0, frames, rate, limiter.getSettings());
            juce::AudioBuffer<float> want (6, frames);
            limited->read (want, 0, 0, frames);
            float worstOff = 0.0f;
            double spread = 0.0;

            for (int i = 0; i < frames; ++i)
            {
                // One gain on every channel: each one's output over its input is the same.
                double low = 1.0e9, high = -1.0e9;

                for (int ch = 0; ch < 6; ++ch)
                {
                    worstOff = juce::jmax (worstOff, std::abs (linked.getSample (ch, i) - want.getSample (ch, i)));

                    if (std::abs (dry.getSample (ch, i)) > 1.0e-3f)
                    {
                        const auto g = (double) linked.getSample (ch, i) / dry.getSample (ch, i);
                        low = juce::jmin (low, g);
                        high = juce::jmax (high, g);
                    }
                }

                if (high >= low)
                    spread = juce::jmax (spread, high - low);
            }

            for (int ch = 0; ch < 6; ++ch)
                loudest = juce::jmax (loudest, linked.getMagnitude (ch, 0, frames));

            check (worstOff == 0.0f && spread < 1.0e-5 && loudest <= juce::Decibels::decibelsToGain (-1.0f) + 1.0e-6f && dynamics.getChainLatency (id) == 240,
                   "5.1 track effects: a compressor and a limiter (240 frames of look-ahead) on a 5.1 track link the six channels, "
                   "bit for bit what compressing and then limiting the six as one file gives, in time (blocks of 97): every channel "
                   "gets the same gain each frame (within " + juce::String (spread, 7) + "), the loudest at "
                   + juce::String (juce::Decibels::gainToDecibels (loudest), 2) + " dBFS under a -1 dB ceiling");

            // With Sidechain on, the compressor is left to the pairs: each hears the key, not the six.
            auto keyed = make (formats, BuiltInEffects::compressorId);
            auto& c = dynamic_cast<BuiltInEffects::CompressorProcessor&> (*keyed);
            c.prepareToPlay (rate, 97);
            juce::AudioBuffer<float> six (6, 97);
            six.clear();
            const auto linksPlain = c.processLinked (six);
            *c.sidechain = true;
            const auto linksKeyed = c.processLinked (six);
            auto eq = make (formats, BuiltInEffects::parametricEqId);
            const auto linksEq = dynamic_cast<BuiltInEffects::BuiltInEffect&> (*eq).processLinked (six);

            check (linksPlain && ! linksKeyed && ! linksEq,
                   "5.1 track effects: a compressor with Sidechain on, and effects other than the compressor and limiter (a parametric "
                   "EQ), don't link the six; they run on the three pairs");
        }

        // A mixdown makes its own copies from the saved chain and gives the same six channels.
        std::map<juce::uint32, std::unique_ptr<juce::XmlElement>> chains;
        chains[id] = effects.createXml (id);
        TrackEffects offline;
        const auto errors = OfflineRender::loadTrackEffects (chains, formats, rate, offline);
        juce::AudioBuffer<float> mix;
        const auto mixed = OfflineRender::mixSession (s, { 0, frames }, mix, &offline, {});
        auto mixSame = mixed && errors.isEmpty() && mix.getNumChannels() == 6;

        for (int ch = 0; mixSame && ch < 6; ++ch)
            mixSame = same (mix, ch, out, ch);

        check (mixSame && offline.getNumCopies (id, 0) == 2 && offline.getNumCopies (id, 1) == 2,
               "5.1 track effects: a mixdown makes the copies for its own plug-ins and gives the six channels as playback does, bit for bit");

        // The engine makes them when a track becomes 5.1 (and drops them when it stops being one).
        auto wide = s;
        engine.session.reset (wide, {});
        engine.clearTrackEffects();
        auto added = false;
        juce::PluginDescription compressor;

        for (auto& d : BuiltInEffects::getDescriptions())
            if (d.fileOrIdentifier == BuiltInEffects::compressorId)
                compressor = d;

        engine.addTrackPlugin (id, compressor, [&added] (const juce::String& error) { added = error.isEmpty(); });

        for (int i = 0; i < 50 && ! added; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);

        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        const auto onWide = engine.getTrackEffects().getNumCopies (id, 0);
        auto stereoNow = *engine.session.getState();
        stereoNow.tracks[0].fiveOne = false;
        engine.session.apply (stereoNow, "Stereo Track");
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        const auto onStereo = engine.getTrackEffects().getNumCopies (id, 0);
        engine.session.undo();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        const auto backOnWide = engine.getTrackEffects().getNumCopies (id, 0);
        engine.clearTrackEffects();
        engine.session.reset ({}, {});

        check (added && onWide == 2 && onStereo == 0 && backOnWide == 2,
               "5.1 track effects: the app's engine gives an effect added to a 5.1 track its two copies, drops them when the track "
               "is made stereo, and makes them again when that is undone");
    }

    void testFollowing()
    {
        const auto audio = noise (6, 4000, 42);
        auto s = session (audio);
        const auto id = s.tracks[0].id;
        const auto dry = render (s, 512, nullptr);

        // A gain on an envelope: the copies take its value at every 64-frame step, as it does.
        {
            TrackEffects effects;
            effects.prepare (rate);
            effects.makeCopy = [] (const juce::PluginDescription&) { return std::make_unique<GainPlugin>(); };
            const auto slot = effects.add (id, std::make_unique<GainPlugin>());
            auto automated = s;
            const Envelope ramp { { { 500, 0.0f }, { 5000, 1.0f } } };
            SessionEdits::setEnvelope (automated, id, AutomationTarget::effect (slot, "gain"), ramp);
            const auto out = render (automated, 37, &effects);
            auto follows = true;

            for (int i = 0; i < frames; ++i)
            {
                const auto g = 2.0f * ramp.valueAt ((i / TrackEffects::automationStep) * TrackEffects::automationStep);

                for (int ch = 0; ch < 6; ++ch)
                    follows = follows && out.getSample (ch, i) == dry.getSample (ch, i) * g;
            }

            check (follows, "5.1 track effects: an effect's automation moves all three copies together, step by step (bit for bit, blocks of 37)");
        }

        // State that isn't a parameter (this delay's gain) reaches the copies when they are made,
        // and again when refreshed (as closing the effect's window does), not before.
        {
            TrackEffects effects;
            effects.prepare (rate);
            effects.makeCopy = [] (const juce::PluginDescription&) { return std::make_unique<DelayPlugin> (240, 1.0f); };
            effects.add (id, std::make_unique<DelayPlugin> (240, 0.5f));
            const auto ratio = [&dry] (const juce::AudioBuffer<float>& out, int ch)
            {
                const auto i = 1500;
                return out.getSample (ch, i) / dry.getSample (ch, i);
            };
            const auto halved = render (s, 512, &effects);
            auto* original = dynamic_cast<DelayPlugin*> (effects.getPlugin (id, 0));
            original->gain = 0.25f;
            const auto before = render (s, 512, &effects);
            effects.refreshCopies (*original);
            const auto after = render (s, 512, &effects);
            auto inTime = true;

            for (int ch = 0; ch < 6; ++ch)
                for (int i = 0; i < frames; ++i)
                    inTime = inTime && after.getSample (ch, i) == dry.getSample (ch, i) * 0.25f;

            check (ratio (halved, 2) == 0.5f && ratio (halved, 5) == 0.5f && ratio (before, 0) == 0.25f && ratio (before, 2) == 0.5f && inTime,
                   "5.1 track effects: copies are made with the effect's whole state, keep it until refreshed, then take its new state; "
                   "with 240 frames of latency all six stay in time");
        }

        // A plug-in that can't be opened again: named, its other pairs silent; a stereo track again drops the copies.
        {
            TrackEffects effects;
            effects.prepare (rate);
            effects.makeCopy = [] (const juce::PluginDescription&) { return std::unique_ptr<juce::AudioPluginInstance>(); };
            effects.add (id, std::make_unique<GainPlugin>());
            const auto failed = effects.setWideTracks ({ id });
            const auto out = render (s, 512, &effects);
            auto stereo = s;
            stereo.tracks[0].fiveOne = false;
            stereo.surround = false;
            TrackEffects later;
            later.prepare (rate);
            later.makeCopy = [] (const juce::PluginDescription&) { return std::make_unique<GainPlugin>(); };
            later.add (id, std::make_unique<GainPlugin>());
            later.setWideTracks ({ id });
            const auto made = later.getNumCopies (id, 0);
            later.setWideTracks ({});

            check (failed == juce::StringArray ("Test Gain") && same (out, 0, dry, 0) && same (out, 1, dry, 1) && silent (out, 2) && silent (out, 3)
                       && silent (out, 4) && silent (out, 5) && made == 2 && later.getNumCopies (id, 0) == 0,
                   "5.1 track effects: an effect that can't be opened twice more is named, and its C LFE and Ls Rs play silence through it; "
                   "a track no longer 5.1 loses its copies");
        }
    }
}

namespace
{
    void testFreezing (AudioEngine& engine)
    {
        juce::AudioPluginFormatManager formats;
        formats.addFormat (BuiltInEffects::createFormat());
        const auto audio = noise (6, 4000, 43);
        auto s = session (audio);
        const auto id = s.tracks[0].id;
        s.tracks[0].gainDb = -4.5f;
        s.tracks[0].invertPolarity = true;

        TrackEffects effects;
        effects.prepare (rate);
        effects.makeCopy = [&formats] (const juce::PluginDescription& d)
        {
            juce::String error;
            return formats.createPluginInstance (d, rate, TrackEffects::maxBlock, error);
        };
        effects.setWideTracks ({ id });
        effects.add (id, make (formats, BuiltInEffects::compressorId));
        effects.add (id, make (formats, BuiltInEffects::limiterId));
        setUp (*effects.getPlugin (id, 0), *effects.getPlugin (id, 1), -24.0f);
        const auto live = render (s, 97, &effects);
        auto stereoMaster = s;
        stereoMaster.surround = false;
        const auto liveFolded = render (stereoMaster, 97, &effects);

        // Freezing as the app does: the track alone, rendered by a mixdown with its effects loaded afresh.
        const auto alone = SessionEdits::frozenSource (s, id);
        std::map<juce::uint32, std::unique_ptr<juce::XmlElement>> chains;
        chains[id] = effects.createXml (id);
        TrackEffects offline;
        OfflineRender::loadTrackEffects (chains, formats, rate, offline);
        juce::AudioBuffer<float> rendered;
        const auto mixed = OfflineRender::mixSession (alone, { 0, frames }, rendered, &offline, {});
        const auto frozenAudio = AudioEdits::fromBuffer (std::move (rendered));
        auto frozen = s;
        const auto froze = SessionEdits::freezeTrack (frozen, id, juce::File(), frozenAudio);

        // Played frozen, with no effects to run, in a 5.1 master and folded down in a stereo one.
        const auto played = render (frozen, 97, nullptr);
        auto frozenStereo = frozen;
        frozenStereo.surround = false;
        const auto playedFolded = render (frozenStereo, 97, nullptr);
        auto exact = mixed && froze && alone.surround && frozenAudio->getNumChannels() == 6;

        for (int ch = 0; ch < 6; ++ch)
            exact = exact && same (played, ch, live, ch);

        check (exact && same (playedFolded, 0, liveFolded, 0) && same (playedFolded, 1, liveFolded, 1),
               "5.1 track freezing: a 5.1 track with a compressor and limiter, -4.5 dB and its polarity inverted, freezes to six channels "
               "and plays them frozen as it did live, bit for bit, in a 5.1 master and folded down in a stereo one");

        // Frozen audio of two channels (a file swapped by hand) plays to L and R, as a stereo clip on it would.
        auto two = s;
        SessionEdits::freezeTrack (two, id, juce::File(), channelsOf (*frozenAudio, 0, 2));
        const auto twoPlayed = render (two, 97, nullptr);
        auto lr = true;

        for (int ch = 0; ch < 2; ++ch)
            lr = lr && same (twoPlayed, ch, played, ch);

        check (lr && silent (twoPlayed, 2) && silent (twoPlayed, 3) && silent (twoPlayed, 4) && silent (twoPlayed, 5),
               "5.1 track freezing: two-channel frozen audio on a 5.1 track plays to L and R alone");

        // The session file keeps it frozen, with its six channels; unfrozen, it plays its effects again.
        const juce::TemporaryFile wav (".wav"), file (".sbsession");
        const auto saveError = AudioFileIO::save (*frozenAudio, rate, wav.getFile(), 32, engine.formatManager);
        auto saved = s;
        SessionEdits::freezeTrack (saved, id, wav.getFile(), frozenAudio);
        const auto sessionError = SessionFile::save (saved, wav.getFile().getSiblingFile (file.getFile().getFileName()), nullptr);
        const auto loaded = SessionFile::load (wav.getFile().getSiblingFile (file.getFile().getFileName()), engine.formatManager);
        wav.getFile().getSiblingFile (file.getFile().getFileName()).deleteFile();
        const auto* back = loaded.state.findTrack (id);
        auto thawed = frozen;
        const auto unfroze = SessionEdits::unfreezeTrack (thawed, id);
        const auto again = render (thawed, 97, &effects);
        auto liveAgain = unfroze;

        for (int ch = 0; ch < 6; ++ch)
            liveAgain = liveAgain && same (again, ch, live, ch);

        check (saveError.isEmpty() && sessionError.isEmpty() && back != nullptr && back->fiveOne && back->isFrozen()
                   && back->frozenAudio->getNumChannels() == 6 && liveAgain,
               "5.1 track freezing: the session file keeps it frozen with its six channels; unfrozen, it plays through its effects as before");
    }
}

namespace
{
    juce::AudioBuffer<float> through (juce::AudioProcessor& p, juce::AudioBuffer<float> audio, int blockSize)
    {
        p.setPlayConfigDetails (2, 2, rate, blockSize);
        p.prepareToPlay (rate, blockSize);
        juce::MidiBuffer midi;

        for (int at = 0; at < audio.getNumSamples(); at += blockSize)
        {
            juce::AudioBuffer<float> view (audio.getArrayOfWritePointers(), audio.getNumChannels(), at, juce::jmin (blockSize, audio.getNumSamples() - at));
            p.processBlock (view, midi);
        }

        return audio;
    }

    juce::AudioBuffer<float> read (const AudioSnapshot& a)
    {
        juce::AudioBuffer<float> b (a.getNumChannels(), (int) a.getNumFrames());
        a.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    /** A rack of a compressor and a limiter, set as setUp does, as AudioEngine::createRackXml writes it. */
    std::unique_ptr<juce::XmlElement> rackXml (juce::AudioPluginFormatManager& formats)
    {
        auto xml = std::make_unique<juce::XmlElement> ("SPECTRABLADE_RACK");
        auto compressor = make (formats, BuiltInEffects::compressorId), limiter = make (formats, BuiltInEffects::limiterId);
        setUp (*compressor, *limiter, -24.0f);

        for (auto* p : { compressor.get(), limiter.get() })
        {
            auto* slot = xml->createNewChildElement ("SLOT");
            slot->setAttribute ("bypassed", false);
            slot->addChildElement (p->getPluginDescription().createXml().release());
            juce::MemoryBlock state;
            p->getStateInformation (state);
            slot->createNewChildElement ("STATE")->addTextElement (state.toBase64Encoding());
        }

        return xml;
    }

    void testMasterRack()
    {
        juce::AudioPluginFormatManager formats;
        formats.addFormat (BuiltInEffects::createFormat());
        const auto wide = read (*noise (6, 20000, 44));

        // A copy of a rack slot: as the slot's own plug-in would play the pair, following its settings and its bypass.
        {
            juce::AudioProcessorGraph graph;
            auto node = graph.addNode (make (formats, BuiltInEffects::compressorId));
            RackCopy copy (node, make (formats, BuiltInEffects::compressorId));
            auto reference = make (formats, BuiltInEffects::compressorId);
            setUp (dynamic_cast<juce::AudioPluginInstance&> (*node->getProcessor()), *make (formats, BuiltInEffects::limiterId), -30.0f);
            setUp (*reference, *make (formats, BuiltInEffects::limiterId), -30.0f);
            const auto pair = channelsOf (*noise (6, 20000, 44), 2, 2);
            const auto got = through (copy, read (*pair), 512), want = through (*reference, read (*pair), 512);
            node->setBypassed (true);
            const auto bypassed = through (copy, read (*pair), 512);
            node->setBypassed (false);

            auto limiterNode = graph.addNode (make (formats, BuiltInEffects::limiterId));
            RackCopy limiterCopy (limiterNode, make (formats, BuiltInEffects::limiterId));
            through (limiterCopy, read (*pair), 512);

            check (same (got, 0, want, 0) && same (got, 1, want, 1) && ! same (got, 0, read (*pair), 0)
                       && same (bypassed, 0, read (*pair), 0) && same (bypassed, 1, read (*pair), 1) && limiterCopy.getLatencySamples() == 240,
                   "master rack copies: a copy of a rack slot plays a pair as the slot's own plug-in would, with the settings set on "
                   "the slot after the copy was made, bit for bit; bypassed with the slot, it passes the pair as it is; it reports "
                   "the copy's latency once prepared (240 frames for the limiter)");
        }

        // Applying the rack to a 5.1 file: each pair through it on its own, as a stereo file of that pair would be.
        {
            const auto rack = rackXml (formats);
            auto audio = wide;
            const auto error = OfflineRender::processThroughRack (*rack, formats, rate, audio, {});
            auto pairs = error.isEmpty() && audio.getNumChannels() == 6;

            for (int pair = 0; pair < 3; ++pair)
            {
                juce::AudioBuffer<float> two (2, wide.getNumSamples());
                two.copyFrom (0, 0, wide, 2 * pair, 0, wide.getNumSamples());
                two.copyFrom (1, 0, wide, 2 * pair + 1, 0, wide.getNumSamples());
                OfflineRender::processThroughRack (*rack, formats, rate, two, {});
                pairs = pairs && same (audio, 2 * pair, two, 0) && same (audio, 2 * pair + 1, two, 1) && ! silent (audio, 2 * pair + 1);
            }

            juce::AudioBuffer<float> three (3, wide.getNumSamples()), last (1, wide.getNumSamples());

            for (int ch = 0; ch < 3; ++ch)
                three.copyFrom (ch, 0, wide, ch, 0, wide.getNumSamples());

            last.copyFrom (0, 0, wide, 2, 0, wide.getNumSamples());
            OfflineRender::processThroughRack (*rack, formats, rate, three, {});
            OfflineRender::processThroughRack (*rack, formats, rate, last, {});

            check (pairs && same (three, 2, last, 0),
                   "master rack on a 5.1 file: applying the rack (a compressor and a limiter) gives each pair, L R, C LFE and Ls Rs, "
                   "bit for bit what it gives a stereo file of that pair, where it used to fold them into L and R and leave the rest "
                   "silent; a three-channel file's last channel goes through on its own, as a mono file");
        }
    }
}

void runSurroundEffectsTests (AudioEngine& engine)
{
    testMasterRack();
    testBuiltIns (engine);
    testFollowing();
    testFreezing (engine);
}

void runMasterRackDeviceTests (AudioEngine& engine)
{
    // Played to separate outputs, a 5.1 master's C LFE and Ls Rs each get a copy of every rack slot.
    juce::PluginDescription compressor;

    for (auto& d : BuiltInEffects::getDescriptions())
        if (d.fileOrIdentifier == BuiltInEffects::compressorId)
            compressor = d;

    auto& player = engine.getPlayer();
    const auto mode = player.getMode();
    const auto wasSeparate = engine.getSurroundOutputs();
    SessionState s;
    s.sampleRate = rate;
    s.surround = true;
    player.setMode (DocumentPlayerProcessor::Mode::session);
    engine.session.reset (s, {});
    engine.clearRack();
    engine.setSurroundOutputs (true);
    engine.useDeviceOutputs();
    auto added = false;
    engine.addPlugin (compressor, [&added] (const juce::String& error) { added = error.isEmpty(); });
    TestUtils::runUntil ([&] { return added; }, 5000);
    const auto separate = engine.getNumRackCopies();
    const auto from = engine.getOutputPair() + 2;
    const auto routed = [&]
    {
        auto all = true;

        for (int ch = from; ch < from + 4; ++ch)
            all = all && engine.playsThroughRackCopies (ch) && ! engine.playsOutOf (ch);

        return all;
    };
    TestUtils::runUntil (routed, 5000);
    const auto throughCopies = routed();
    auto second = false;
    engine.addPlugin (compressor, [&second] (const juce::String& error) { second = error.isEmpty(); });
    TestUtils::runUntil ([&] { return second; }, 5000);
    const auto two = engine.getNumRackCopies();
    engine.removeRackSlot (0);
    const auto afterRemove = engine.getNumRackCopies();
    engine.removeRackSlot (0);
    const auto straight = engine.playsOutOf (from) && engine.playsOutOf (from + 3) && ! engine.playsThroughRackCopies (from);
    engine.setSurroundOutputs (false);
    const auto folded = engine.getNumRackCopies();
    engine.setSurroundOutputs (true);
    auto stereo = s;
    stereo.surround = false;
    engine.session.reset (stereo, {});
    engine.useDeviceOutputs();
    TestUtils::runUntil ([&] { return engine.getNumRackCopies() == 0; }, 2000);
    const auto stereoMaster = engine.getNumRackCopies();
    engine.clearRack();
    engine.setSurroundOutputs (wasSeparate);
    engine.session.reset (SessionState(), {});
    player.setMode (mode);

    check (added && second && separate == 2 && throughCopies && two == 4 && afterRemove == 2 && straight && folded == 0 && stereoMaster == 0,
           "master rack copies: played to separate outputs, a 5.1 master's other two pairs get a copy of each rack slot (2 for "
           "one slot, 4 for two, 2 again when one is removed), and C, LFE, Ls and Rs reach outputs " + juce::String (from + 1) + " to "
           + juce::String (from + 4) + " of the " + juce::String (engine.getDeviceOutputChannelNames().size()) + "-output interface "
           "through them, not straight from the player; with an empty rack, straight; folded down, or with a stereo master, no copies");
}
