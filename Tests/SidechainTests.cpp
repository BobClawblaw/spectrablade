// Latency compensation of sidechains must be exact, so mixes are compared bit for bit.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/SessionMixer.h"
#include "../Source/TrackEffects.h"
#include "../Source/BuiltInEffects.h"
#include "../Source/Bounce.h"
#include "TestPlugins.h"
#include "TestUtils.h"
#include "SidechainTests.h"

namespace
{
    using TestUtils::check;
    using TestPlugins::DelayPlugin;
    using TestPlugins::SidechainPlugin;

    constexpr double rate = 48000.0;
    constexpr int length = 9000;

    juce::AudioBuffer<float> noiseBuffer (int channels, int frames, int seed, float level = 0.5f)
    {
        juce::Random r (seed);
        juce::AudioBuffer<float> b (channels, frames);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, (r.nextFloat() - 0.5f) * level);

        return b;
    }

    AudioDocument::Snapshot noise (int channels, int frames, int seed, float level = 0.5f)
    {
        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (noiseBuffer (channels, frames, seed, level)), 0, frames } };
        return std::make_shared<const AudioSnapshot> (channels, std::move (segments));
    }

    void addClip (SessionState& s, SessionTrack& t, AudioDocument::Snapshot audio, juce::int64 position)
    {
        SessionClip c;
        c.id = s.nextId++;
        c.audio = std::move (audio);
        c.position = position;
        c.length = c.audio->getNumFrames();
        c.fadeIn = 50;
        c.fadeOut = 300;
        t.clips.push_back (c);
    }

    /** Key K (tracks[0]), listener L (tracks[1]), and bus Y (tracks[2]) that L sends to. */
    SessionState keyed()
    {
        SessionState s;
        s.sampleRate = rate;
        auto& k = s.addTrack();
        k.name = "Key";
        k.gainDb = -3.0f;
        k.pan = 0.2f;
        addClip (s, k, noise (2, 6000, 11), 300);
        auto& l = s.addTrack();
        l.name = "Listener";
        l.gainDb = -1.5f;
        addClip (s, l, noise (2, 6000, 12), 100);
        s.addBus().name = "Y";
        SessionEdits::setSend (s, s.tracks[1].id, { s.tracks[2].id, -6.0f, false });
        return s;
    }

    juce::AudioBuffer<float> play (const SessionState& s, juce::int64 start, int frames, int blockSize, SessionMixer::Inserts* inserts)
    {
        juce::AudioBuffer<float> out (2, frames);
        SessionMixer::Buffers buffers;
        buffers.reserve (512);

        if (inserts != nullptr)
            SessionMixer::prime (s, start, buffers, *inserts);

        for (int p = 0; p < frames; p += blockSize)
            SessionMixer::render (s, start + p, out, p, juce::jmin (blockSize, frames - p), buffers, inserts);

        return out;
    }

    /** Track index alone at its own volume, with no sends: what it plays into the mix. */
    juce::AudioBuffer<float> alone (const SessionState& s, int index)
    {
        SessionState one;
        one.sampleRate = s.sampleRate;
        one.tracks.push_back (s.tracks[(size_t) index]);
        one.tracks[0].sends.clear();
        one.tracks[0].sidechains.clear();
        return play (one, 0, length, length, nullptr);
    }

    float worst (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        float w = 0.0f;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
                w = juce::jmax (w, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));

        return w;
    }

    bool same (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        if (a.getNumSamples() != b.getNumSamples())
            return false;

        for (int ch = 0; ch < 2; ++ch)
            if (std::memcmp (a.getReadPointer (ch), b.getReadPointer (ch), sizeof (float) * (size_t) a.getNumSamples()) != 0)
                return false;

        return true;
    }

    juce::AudioBuffer<float> slice (const juce::AudioBuffer<float>& b, int start, int frames)
    {
        juce::AudioBuffer<float> out (2, frames);

        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, 0, b, ch, start, frames);

        return out;
    }

    float gain (float db)    { return juce::Decibels::decibelsToGain (db); }

    /** The chains of the latency check: L gets a delay (before) then the sidechain plug-in, K a
        delay, and Y a delay then a sidechain plug-in too; all latencies scaled by on (0 or 1). */
    void addChains (TrackEffects& effects, SessionState& s, int on)
    {
        const auto k = s.tracks[0].id, l = s.tracks[1].id, y = s.tracks[2].id;
        effects.add (l, std::make_unique<DelayPlugin> (120 * on, 1.0f));
        const auto lSlot = effects.add (l, std::make_unique<SidechainPlugin> (250 * on));
        effects.add (k, std::make_unique<DelayPlugin> (300 * on, 1.0f));
        effects.add (y, std::make_unique<DelayPlugin> (500 * on, 1.0f));
        const auto ySlot = effects.add (y, std::make_unique<SidechainPlugin> (80 * on));
        SessionEdits::setSidechain (s, l, lSlot, k);
        SessionEdits::setSidechain (s, y, ySlot, k);
    }
}

void runSidechainTests()
{
    // The key, after its volume, goes into the listener's effect: L plays its own sound plus K's.
    {
        auto s = keyed();
        s.tracks[1].sends.clear();
        const auto dry = play (s, 0, length, 512, nullptr);
        TrackEffects effects;
        effects.prepare (rate);
        const auto slot = effects.add (s.tracks[1].id, std::make_unique<SidechainPlugin> (0));
        const auto set = SessionEdits::setSidechain (s, s.tracks[1].id, slot, s.tracks[0].id);

        auto expected = dry;
        const auto key = alone (s, 0);
        expected.addFrom (0, 0, key, 0, 0, length, gain (-1.5f));
        expected.addFrom (1, 0, key, 1, 0, length, gain (-1.5f));
        const auto w = worst (play (s, 0, length, 512, &effects), expected);
        check (set && effects.hasSidechain (s.tracks[1].id, 0) && w < 1.0e-6f,
               "sidechain: the key track's output, after its volume, goes into the listener's effect (worst " + juce::String (w, 9) + ")");

        // A key that doesn't play feeds silence; with no key the effect hears silence too.
        auto muted = s;
        muted.tracks[0].mute = true;
        auto none = s;
        SessionEdits::setSidechain (none, none.tracks[1].id, slot, 0);
        check (same (play (muted, 0, length, 512, &effects), play (muted, 0, length, 512, nullptr))
                   && same (play (none, 0, length, 512, &effects), dry) && none.tracks[1].sidechains.empty(),
               "sidechain: a muted key feeds silence, and so does none");

        // A mono sidechain input gets the average of the key's sides.
        TrackEffects mono;
        mono.prepare (rate);
        auto m = keyed();
        m.tracks[1].sends.clear();
        const auto monoSlot = mono.add (m.tracks[1].id, std::make_unique<SidechainPlugin> (0, true));
        SessionEdits::setSidechain (m, m.tracks[1].id, monoSlot, m.tracks[0].id);
        auto monoExpected = dry;

        for (int ch = 0; ch < 2; ++ch)
        {
            monoExpected.addFrom (ch, 0, key, 0, 0, length, 0.5f * gain (-1.5f));
            monoExpected.addFrom (ch, 0, key, 1, 0, length, 0.5f * gain (-1.5f));
        }

        const auto wm = worst (play (m, 0, length, 512, &mono), monoExpected);
        check (wm < 1.0e-6f, "sidechain: a mono sidechain input gets the key's two sides averaged (worst " + juce::String (wm, 9) + ")");
    }

    // Latency: 120 frames before L's sidechain effect and 250 in it, 300 on K, and 500 before Y's
    // (a bus) and 80 in it; everything lines up as with none.
    {
        auto s = keyed(), z = keyed();
        TrackEffects latent, none;
        latent.prepare (rate);
        none.prepare (rate);
        addChains (latent, s, 1);
        addChains (none, z, 0);
        const auto reference = play (z, 0, length, 512, &none);
        const auto heard = reference.getMagnitude (0, length) > 0.1f;
        check (heard && same (play (s, 0, length, 333, &latent), reference) && same (play (s, 0, length, 2900, &latent), reference),
               "sidechain: with 120 frames of latency before a track's sidechain effect, 250 in it, 300 on the key, and 580 on a bus "
               "with a sidechain effect, the mix matches the one with none, bit for bit, in 333- and 2900-frame blocks");
        check (same (play (s, 2500, 4000, 256, &latent), slice (reference, 2500, 4000)),
               "sidechain: playing from 2500 after priming matches it bit for bit too");
    }

    // The built-in compressor: Sidechain on takes the level from its sidechain input.
    {
        BuiltInEffects::CompressorProcessor c;
        c.enableAllBuses();
        c.setSettings ({ -30.0, 8.0, 0.0, 1.0, 2.0, 0.0 });
        c.prepareToPlay (rate, 4096);
        juce::AudioBuffer<float> b (4, 4096);
        const auto input = noiseBuffer (2, 4096, 5, 0.1f), key = noiseBuffer (2, 4096, 6, 1.6f);

        auto run = [&] (bool sidechain)
        {
            c.sidechain->setValueNotifyingHost (sidechain ? 1.0f : 0.0f);
            c.reset();

            for (int ch = 0; ch < 2; ++ch)
            {
                b.copyFrom (ch, 0, input, ch, 0, 4096);
                b.copyFrom (2 + ch, 0, key, ch, 0, 2048);
                b.clear (2 + ch, 2048, 2048);   // the key stops half way
            }

            juce::MidiBuffer midi;
            c.processBlock (b, midi);
        };

        // What the core gives for each frame's level, from the key or from the input itself.
        auto expect = [&] (bool fromKey)
        {
            Dynamics::CompressorCore core;
            core.set (c.getSettings(), rate);
            juce::AudioBuffer<float> out (2, 4096);

            for (int i = 0; i < 4096; ++i)
            {
                double level = 0.0;

                for (int ch = 0; ch < 2; ++ch)
                    level = juce::jmax (level, (double) std::abs (fromKey ? (i < 2048 ? key.getSample (ch, i) : 0.0f) : input.getSample (ch, i)));

                const auto factor = core.next (level);

                for (int ch = 0; ch < 2; ++ch)
                    out.setSample (ch, i, (float) (input.getSample (ch, i) * factor));
            }

            return out;
        };

        run (true);
        const auto keyedOut = slice (b, 0, 4096);
        const auto keyedSame = same (keyedOut, expect (true));
        const auto mainRms = [&input] (int from) { return input.getRMSLevel (0, from, 1024); };
        const auto reductionDb = juce::Decibels::gainToDecibels (keyedOut.getRMSLevel (0, 1024, 1024) / mainRms (1024));
        const auto afterDb = juce::Decibels::gainToDecibels (keyedOut.getRMSLevel (0, 3072, 1024) / mainRms (3072));
        run (false);
        const auto ownSame = same (slice (b, 0, 4096), expect (false));
        check (keyedSame && ownSame && reductionDb < -15.0f && afterDb > -0.5f,
               "compressor: with Sidechain on it follows the key, frame for frame as Dynamics gives (" + juce::String (reductionDb, 1)
                   + " dB while the key plays, " + juce::String (afterDb, 2) + " dB after), and off its own input");

        // Without its sidechain bus turned on (a rack, or offline), Sidechain on hears silence there.
        BuiltInEffects::CompressorProcessor plain;
        TrackEffects effects;
        effects.prepare (rate);
        effects.add (1, std::make_unique<BuiltInEffects::CompressorProcessor>());
        effects.add (1, std::make_unique<DelayPlugin> (0, 1.0f));
        check (plain.getTotalNumInputChannels() == 2 && effects.hasSidechain (1, 0) && ! effects.hasSidechain (1, 1),
               "compressor: its sidechain input is off until a host turns it on; on a track it takes one, a plug-in without one doesn't");
    }

    // The compressor on a session track, keyed by another: the listener comes down while the key plays.
    {
        auto s = keyed();
        s.tracks[1].sends.clear();
        s.tracks[0].clips.clear();
        addClip (s, s.tracks[0], noise (2, 3000, 21, 1.6f), 0);
        TrackEffects effects;
        effects.prepare (rate);
        auto compressor = std::make_unique<BuiltInEffects::CompressorProcessor>();
        compressor->setSettings ({ -30.0, 8.0, 0.0, 1.0, 50.0, 0.0 });
        compressor->sidechain->setValueNotifyingHost (1.0f);
        const auto settings = compressor->getSettings();
        const auto slot = effects.add (s.tracks[1].id, std::move (compressor));
        SessionEdits::setSidechain (s, s.tracks[1].id, slot, s.tracks[0].id);
        const auto mixed = play (s, 0, length, 512, &effects);

        // By hand: L after its pan, times the core's factor for the key's level, times its volume; plus K.
        const auto key = alone (s, 0);
        auto lPanned = s;
        lPanned.tracks.erase (lPanned.tracks.begin());
        lPanned.tracks.erase (lPanned.tracks.begin() + 1);
        lPanned.tracks[0].gainDb = 0.0f;
        lPanned.tracks[0].sidechains.clear();
        const auto lDry = play (lPanned, 0, length, length, nullptr);
        Dynamics::CompressorCore core;
        core.set (settings, rate);
        auto expected = key;

        for (int i = 0; i < length; ++i)
        {
            const auto factor = core.next (juce::jmax (std::abs (key.getSample (0, i)), std::abs (key.getSample (1, i))));

            for (int ch = 0; ch < 2; ++ch)
                expected.addSample (ch, i, (float) (lDry.getSample (ch, i) * factor) * gain (-1.5f));
        }

        const auto w = worst (mixed, expected);
        check (w < 1.0e-6f, "compressor: on a session track keyed by another, it comes down with the key's level, as worked out by hand (worst "
                                + juce::String (w, 9) + ")");
    }

    // Edits: no loops, through ducking too; keys are tracks; deleting the key; files.
    {
        auto s = keyed();
        const auto k = s.tracks[0].id, l = s.tracks[1].id, y = s.tracks[2].id;
        auto& third = s.addTrack();
        const auto m = third.id;
        auto& folder = s.addFolder();
        const auto f = folder.id;
        const auto ok = SessionEdits::setSidechain (s, l, 7, k) && SessionEdits::setSidechain (s, m, 8, l) && SessionEdits::setSidechain (s, y, 9, m);
        const auto refused = ! SessionEdits::setSidechain (s, l, 7, k)                // no change
                             && ! SessionEdits::setSidechain (s, k, 10, m)            // K -> M -> L -> K
                             && ! SessionEdits::setSidechain (s, l, 11, l)            // itself
                             && ! SessionEdits::setSidechain (s, l, 11, y)            // a bus
                             && ! SessionEdits::setSidechain (s, l, 11, f)            // a folder
                             && ! SessionEdits::setSidechain (s, f, 11, k)            // on a folder
                             && ! SessionEdits::setDucking (s, k, { m, -12.0f, -40.0f, 10.0f, 300.0f });   // K ducking under M, which listens to K
        const auto chain = s.listensTo (*s.findTrack (y), k) && ! s.listensTo (*s.findTrack (k), y);

        // Session files keep them, by the key's place.
        auto dir = juce::File::createTempFile ("sidechain");
        dir.createDirectory();
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        auto noClips = s;

        for (auto& t : noClips.tracks)
            t.clips.clear();

        const auto target = dir.getChildFile ("keys.sbsession");
        const auto saved = SessionFile::save (noClips, target).isEmpty();
        const auto loaded = SessionFile::load (target, formats).state;
        const auto& lt = loaded.tracks;
        const auto file = saved && lt.size() == 5 && lt[1].sidechainKey (7) == lt[0].id && lt[3].sidechainKey (8) == lt[1].id
                          && lt[2].sidechainKey (9) == lt[3].id && lt[0].sidechains.empty();
        dir.deleteRecursively();

        auto removed = s;
        SessionEdits::removeTrack (removed, 0);
        check (ok && refused && chain && file && removed.findTrack (l)->sidechains.empty() && removed.findTrack (m)->sidechainKey (8) == l,
               "sidechain: set on tracks and buses; refused for a loop of any length (through ducking too), itself, a bus or folder as "
               "the key, a folder, or no change; kept in session files; deleting the key removes it");
    }

    // Freezing and bouncing a listener bring its key along, heard by the effect but not in the render.
    {
        auto s = keyed();
        s.tracks[1].sends.clear();
        TrackEffects effects;
        effects.prepare (rate);
        effects.add (s.tracks[0].id, std::make_unique<DelayPlugin> (300, 1.0f));
        const auto slot = effects.add (s.tracks[1].id, std::make_unique<SidechainPlugin> (64));
        SessionEdits::setSidechain (s, s.tracks[1].id, slot, s.tracks[0].id);

        const auto key = alone (s, 0);
        auto lAlone = alone (s, 1);
        auto bounceExpected = lAlone;
        bounceExpected.addFrom (0, 0, key, 0, 0, length, gain (-1.5f));
        bounceExpected.addFrom (1, 0, key, 1, 0, length, gain (-1.5f));
        const auto isolated = Bounce::isolate (s, s.tracks[1].id);
        const auto wb = worst (play (isolated, 0, length, 512, &effects), bounceExpected);

        auto zeroGain = s;
        zeroGain.tracks[1].gainDb = 0.0f;
        auto freezeExpected = alone (zeroGain, 1);
        freezeExpected.addFrom (0, 0, key, 0, 0, length);
        freezeExpected.addFrom (1, 0, key, 1, 0, length);
        const auto frozen = SessionEdits::frozenSource (s, s.tracks[1].id);
        const auto wf = worst (play (frozen, 0, length, 512, &effects), freezeExpected);
        const auto* copy = frozen.findTrack (s.tracks[0].id);
        check (frozen.tracks.size() == 2 && copy != nullptr && copy->deviceOutput == 0 && copy->sends.empty() && wb < 1.0e-6f && wf < 1.0e-6f,
               "sidechain: a bounce and a freeze of the listener hear its key (brought along, playing out of an interface pair so it isn't "
               "in the render), 300 and 64 frames of latency lined up (worst " + juce::String (juce::jmax (wb, wf), 9) + ")");

        // A ducked track's bounce ducks too: it is what the track plays in the session.
        auto d = keyed();
        d.tracks[1].sends.clear();
        SessionEdits::setDucking (d, d.tracks[1].id, { d.tracks[0].id, -20.0f, -40.0f, 5.0f, 100.0f });
        auto inMix = play (d, 0, length, 512, nullptr);
        const auto keyPart = alone (d, 0);
        inMix.addFrom (0, 0, keyPart, 0, 0, length, -1.0f);
        inMix.addFrom (1, 0, keyPart, 1, 0, length, -1.0f);
        const auto bounced = play (Bounce::isolate (d, d.tracks[1].id), 0, length, 512, nullptr);
        const auto wd = worst (bounced, inMix);
        const auto undAlone = alone (d, 1);
        check (wd < 1.0e-6f && worst (bounced, undAlone) > 0.01f,
               "bounce: a ducked track's bounce ducks under its key as in the session (worst " + juce::String (wd, 9) + ")");
    }
}
