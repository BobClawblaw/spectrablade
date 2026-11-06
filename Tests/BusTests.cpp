// Delay compensation through buses must be exact, so mixes are compared bit for bit.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/SessionMixer.h"
#include "../Source/TrackEffects.h"
#include "../Source/TrackTemplate.h"
#include "TestPlugins.h"
#include "TestUtils.h"
#include "BusTests.h"

namespace
{
    using TestUtils::check;
    using TestPlugins::DelayPlugin;

    constexpr double rate = 48000.0;
    constexpr int length = 9000;

    AudioDocument::Snapshot noise (int channels, int frames, int seed)
    {
        juce::Random r (seed);
        juce::AudioBuffer<float> b (channels, frames);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, r.nextFloat() * 0.5f - 0.25f);

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, frames } };
        return std::make_shared<const AudioSnapshot> (channels, std::move (segments));
    }

    void addNoiseClip (SessionState& s, SessionTrack& t, int channels, juce::int64 position, int seed)
    {
        SessionClip c;
        c.id = s.nextId++;
        c.audio = noise (channels, 6000, seed);
        c.position = position;
        c.length = c.audio->getNumFrames();
        c.fadeIn = 50;
        c.fadeOut = 300;
        t.clips.push_back (c);
    }

    /** Tracks A (mono) and B (stereo); bus X gets A after its volume and B before its; bus Y gets B after. */
    SessionState routed()
    {
        SessionState s;
        s.sampleRate = rate;
        auto& a = s.addTrack();
        a.pan = -0.4f;
        a.gainDb = -2.0f;
        addNoiseClip (s, a, 1, 40, 3);
        auto& b = s.addTrack();
        b.gainDb = 1.5f;
        addNoiseClip (s, b, 2, 900, 4);
        auto& x = s.addBus();
        x.gainDb = -3.0f;
        x.pan = 0.3f;
        auto& y = s.addBus();
        y.gainDb = -1.0f;

        SessionEdits::setSend (s, s.tracks[0].id, { s.tracks[2].id, 0.0f, false });
        SessionEdits::setSend (s, s.tracks[1].id, { s.tracks[2].id, -6.0f, true });
        SessionEdits::setSend (s, s.tracks[1].id, { s.tracks[3].id, -10.0f, false });
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

    /** Track index alone, at gainDb, with no sends. */
    juce::AudioBuffer<float> alone (const SessionState& s, int index, float gainDb)
    {
        SessionState one;
        one.sampleRate = s.sampleRate;
        one.tracks.push_back (s.tracks[(size_t) index]);
        one.tracks[0].gainDb = gainDb;
        one.tracks[0].sends.clear();
        return play (one, 0, length, length, nullptr);
    }

    float gain (float db)                        { return juce::Decibels::decibelsToGain (db); }

    /** out += src * left/right. */
    void add (juce::AudioBuffer<float>& out, const juce::AudioBuffer<float>& src, float left, float right)
    {
        out.addFrom (0, 0, src, 0, 0, out.getNumSamples(), left);
        out.addFrom (1, 0, src, 1, 0, out.getNumSamples(), right);
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
        return worst (a, b) == 0.0f;
    }

    juce::AudioBuffer<float> slice (const juce::AudioBuffer<float>& b, int start, int frames)
    {
        juce::AudioBuffer<float> out (2, frames);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, 0, b, ch, start, frames);
        return out;
    }
}

void runBusTests()
{
    const auto s = routed();
    const auto aPost = alone (s, 0, -2.0f), bPre = alone (s, 1, 0.0f), bPost = alone (s, 1, 1.5f);
    const auto [xl, xr] = SessionMixer::panGains (0.3f, 2);
    const auto x = gain (-3.0f), y = gain (-1.0f);

    // The routing, against the same sums made by hand.
    {
        juce::AudioBuffer<float> expected (2, length);
        expected.clear();
        add (expected, aPost, 1.0f, 1.0f);
        add (expected, bPost, 1.0f, 1.0f);
        add (expected, aPost, x * xl, x * xr);                                        // A after its volume, to X
        add (expected, bPre, gain (-6.0f) * x * xl, gain (-6.0f) * x * xr);           // B before its volume, to X
        add (expected, bPost, gain (-10.0f) * y, gain (-10.0f) * y);                  // B after its volume, to Y

        const auto w = worst (play (s, 0, length, 512, nullptr), expected);
        check (w < 1.0e-6f, "buses: pre- and post-fader sends, bus volume and pan add up (worst " + juce::String (w, 9) + ")");

        auto mutedX = s;
        mutedX.tracks[2].mute = true;
        juce::AudioBuffer<float> noX (2, length);
        noX.clear();
        add (noX, aPost, 1.0f, 1.0f);
        add (noX, bPost, 1.0f, 1.0f);
        add (noX, bPost, gain (-10.0f) * y, gain (-10.0f) * y);
        const auto wm = worst (play (mutedX, 0, length, 512, nullptr), noX);
        check (wm < 1.0e-6f, "buses: a muted bus plays nothing (worst " + juce::String (wm, 9) + ")");
    }

    // Solo: a soloed bus brings its tracks; a soloed track brings its buses.
    {
        auto soloX = s;
        soloX.tracks[2].solo = true;
        juce::AudioBuffer<float> expected (2, length);
        expected.clear();
        add (expected, aPost, 1.0f, 1.0f);
        add (expected, bPost, 1.0f, 1.0f);
        add (expected, aPost, x * xl, x * xr);
        add (expected, bPre, gain (-6.0f) * x * xl, gain (-6.0f) * x * xr);
        const auto w1 = worst (play (soloX, 0, length, 512, nullptr), expected);
        check (w1 < 1.0e-6f, "buses: soloing a bus plays it and the tracks feeding it, not other buses");

        auto soloA = s;
        soloA.tracks[0].solo = true;
        expected.clear();
        add (expected, aPost, 1.0f, 1.0f);
        add (expected, aPost, x * xl, x * xr);
        const auto w2 = worst (play (soloA, 0, length, 512, nullptr), expected);
        check (w2 < 1.0e-6f, "buses: soloing a track plays it and the buses it feeds, and nothing else");

        // Solo safe: with A soloed, a solo-safe B plays, with its send into X (which A's solo
        // brings); a solo-safe Y adds what B sends it; a solo-safe bus alone hears nothing, as
        // nothing playing feeds it.
        auto safeB = soloA;
        safeB.tracks[1].soloSafe = true;
        add (expected, bPost, 1.0f, 1.0f);
        add (expected, bPre, gain (-6.0f) * x * xl, gain (-6.0f) * x * xr);
        const auto w3 = worst (play (safeB, 0, length, 512, nullptr), expected);

        auto safeBY = safeB;
        safeBY.tracks[3].soloSafe = true;
        add (expected, bPost, gain (-10.0f) * y, gain (-10.0f) * y);
        const auto w4 = worst (play (safeBY, 0, length, 512, nullptr), expected);

        auto safeY = soloA;
        safeY.tracks[3].soloSafe = true;
        const auto w5 = worst (play (safeY, 0, length, 512, nullptr), play (soloA, 0, length, 512, nullptr));

        auto mutedSafe = safeB;
        mutedSafe.tracks[1].mute = true;
        auto unsoloed = s;
        unsoloed.tracks[1].soloSafe = true;
        check (w3 < 1.0e-6f && w4 < 1.0e-6f && juce::exactlyEqual (w5, 0.0f)
                   && same (play (mutedSafe, 0, length, 512, nullptr), play (soloA, 0, length, 512, nullptr))
                   && same (play (unsoloed, 0, length, 512, nullptr), play (s, 0, length, 512, nullptr)),
               "buses: solo safe tracks and buses play beside a solo (a bus only what plays into it); muted still mutes; no effect without a solo (worst "
                   + juce::String (juce::jmax (w3, w4), 9) + " against sums made by hand)");
    }

    // Polarity: a track's output (with what it sends) turned upside down, bit for bit; a bus's too.
    {
        const auto negated = [] (juce::AudioBuffer<float> b)
        {
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                juce::FloatVectorOperations::negate (b.getWritePointer (ch), b.getReadPointer (ch), b.getNumSamples());

            return b;
        };

        auto onlyA = s;
        onlyA.tracks[1].mute = true;
        auto invertedA = onlyA;
        invertedA.tracks[0].invertPolarity = true;
        auto onlyB = s;
        onlyB.tracks[0].mute = true;
        auto invertedB = onlyB;
        invertedB.tracks[1].invertPolarity = true;
        const auto trackFlipped = same (play (invertedA, 0, length, 512, nullptr), negated (play (onlyA, 0, length, 512, nullptr)))
                                  && same (play (invertedB, 0, length, 512, nullptr), negated (play (onlyB, 0, length, 512, nullptr)));

        // A inverted and X inverted: A's direct sound upside down, its send through X the right way up.
        auto twice = invertedA;
        twice.tracks[2].invertPolarity = true;
        juce::AudioBuffer<float> expected (2, length);
        expected.clear();
        add (expected, aPost, -1.0f, -1.0f);
        add (expected, aPost, x * xl, x * xr);
        const auto w = worst (play (twice, 0, length, 512, nullptr), expected);

        auto frozenFrom = s;
        frozenFrom.tracks[0].invertPolarity = true;
        const auto source = SessionEdits::frozenSource (frozenFrom, frozenFrom.tracks[0].id);
        check (trackFlipped && w < 1.0e-6f && source.tracks.size() == 1 && ! source.tracks[0].invertPolarity,
               "polarity: an inverted track sounds (and sends, before and after its volume) exactly upside down; an inverted bus flips what reaches it; "
               "freezing renders without it (worst " + juce::String (w, 9) + " against sums made by hand)");
    }

    // Delay compensation through buses: effects on A (300), X (500) and Y (64), none on B.
    {
        const auto dry = play (s, 0, length, length, nullptr);
        TrackEffects effects;
        effects.prepare (rate);
        effects.add (s.tracks[0].id, std::make_unique<DelayPlugin> (300, 1.0f));
        effects.add (s.tracks[2].id, std::make_unique<DelayPlugin> (500, 1.0f));
        effects.add (s.tracks[3].id, std::make_unique<DelayPlugin> (64, 1.0f));

        check (same (play (s, 0, length, 333, &effects), dry),
               "buses: tracks and buses with 300, 500 and 64 frames of latency line up bit for bit");
        check (same (play (s, 0, length, 2900, &effects), dry), "buses: the same in 2900-frame blocks");
        check (same (play (s, 2500, 4000, 256, &effects), slice (dry, 2500, 4000)),
               "buses: playing from 2500 after priming matches the dry mix bit for bit");

        // A bus effect that does something: X at half level.
        dynamic_cast<DelayPlugin*> (effects.getPlugin (s.tracks[2].id, 0))->gain = 0.5f;
        auto halfX = s;
        halfX.tracks[2].gainDb += juce::Decibels::gainToDecibels (0.5f);
        const auto w = worst (play (s, 0, length, 512, &effects), play (halfX, 0, length, length, nullptr));
        check (w < 1.0e-6f, "buses: a bus plays through its effects (worst " + juce::String (w, 9) + ")");
    }

    // Edits.
    {
        auto e = s;
        check (! SessionEdits::setSend (e, e.tracks[2].id, { e.tracks[3].id, 0.0f, false })
                   && ! SessionEdits::setSend (e, e.tracks[0].id, { e.tracks[1].id, 0.0f, false }),
               "buses: only tracks send, and only to buses");

        SessionEdits::setSend (e, e.tracks[0].id, { e.tracks[2].id, -4.0f, true });
        check (e.tracks[0].sends.size() == 1 && e.tracks[0].sends[0].gainDb == -4.0f && e.tracks[0].sends[0].preFader,
               "buses: setting an existing send changes it");

        const auto clipId = e.tracks[0].clips[0].id;
        SessionEdits::moveClip (e, clipId, 2, 1234);
        const auto where = e.findClip (clipId);
        check (where.has_value() && where->first == 0 && e.tracks[0].clips[0].position == 1234,
               "buses: a clip dragged onto a bus only moves in time");

        SessionEdits::addClip (e, 3, {}, noise (1, 100, 9), 0);
        check (e.tracks.size() == 5 && ! e.tracks[4].isBus && e.tracks[4].clips.size() == 1 && e.tracks[4].name == "Track 3",
               "buses: audio added on a bus goes to a new track");

        SessionEdits::removeTrack (e, 2);
        check (e.tracks[0].sends.empty() && e.tracks[1].sends.size() == 1, "buses: deleting a bus removes the sends to it");
        check (e.addBus().name == "Bus 3", "buses: buses are numbered on their own, skipping names in use");
    }

    // Outputs: A plays into Y in place of the mix, after its volume; its send to X stays.
    {
        auto out = s;
        const auto changed = SessionEdits::setTrackOutput (out, out.tracks[0].id, out.tracks[3].id);
        juce::AudioBuffer<float> expected (2, length);
        expected.clear();
        add (expected, bPost, 1.0f, 1.0f);
        add (expected, aPost, y, y);                                                  // A through Y, not the mix
        add (expected, aPost, x * xl, x * xr);
        add (expected, bPre, gain (-6.0f) * x * xl, gain (-6.0f) * x * xr);
        add (expected, bPost, gain (-10.0f) * y, gain (-10.0f) * y);
        const auto w = worst (play (out, 0, length, 512, nullptr), expected);
        check (changed && w < 1.0e-6f,
               "outputs: a track played into a bus is heard through it and not in the mix, its sends as they were (worst " + juce::String (w, 9) + ")");

        auto mutedY = out;
        mutedY.tracks[3].mute = true;
        juce::AudioBuffer<float> noY (2, length);
        noY.clear();
        add (noY, bPost, 1.0f, 1.0f);
        add (noY, aPost, x * xl, x * xr);
        add (noY, bPre, gain (-6.0f) * x * xl, gain (-6.0f) * x * xr);
        const auto wm = worst (play (mutedY, 0, length, 512, nullptr), noY);

        auto soloY = out;
        soloY.tracks[3].solo = true;
        juce::AudioBuffer<float> fromY (2, length);
        fromY.clear();
        add (fromY, aPost, y, y);
        add (fromY, bPost, 1.0f, 1.0f);
        add (fromY, bPost, gain (-10.0f) * y, gain (-10.0f) * y);
        const auto ws = worst (play (soloY, 0, length, 512, nullptr), fromY);

        auto soloA = out;
        soloA.tracks[0].solo = true;
        juce::AudioBuffer<float> fromA (2, length);
        fromA.clear();
        add (fromA, aPost, y, y);
        add (fromA, aPost, x * xl, x * xr);
        const auto wa = worst (play (soloA, 0, length, 512, nullptr), fromA);
        check (wm < 1.0e-6f && ws < 1.0e-6f && wa < 1.0e-6f,
               "outputs: muting the bus silences the track played into it (but not its sends); soloing the bus brings that track; "
               "soloing the track brings its bus (worst " + juce::String (juce::jmax (wm, ws, wa), 9) + ")");

        const auto dry = play (out, 0, length, length, nullptr);
        TrackEffects effects;
        effects.prepare (rate);
        effects.add (out.tracks[0].id, std::make_unique<DelayPlugin> (300, 1.0f));
        effects.add (out.tracks[2].id, std::make_unique<DelayPlugin> (500, 1.0f));
        effects.add (out.tracks[3].id, std::make_unique<DelayPlugin> (64, 1.0f));
        check (same (play (out, 0, length, 333, &effects), dry) && same (play (out, 2500, 4000, 256, &effects), slice (dry, 2500, 4000)),
               "outputs: with 300, 500 and 64 frames of latency on the track and both buses, the routed mix lines up bit for bit, from the start and after priming");

        auto e = out;
        const auto refused = ! SessionEdits::setTrackOutput (e, e.tracks[2].id, e.tracks[2].id)       // a bus into itself
                             && ! SessionEdits::setTrackOutput (e, e.tracks[1].id, e.tracks[0].id)    // to a track
                             && ! SessionEdits::setTrackOutput (e, e.tracks[0].id, e.tracks[3].id)    // already so
                             && ! SessionEdits::setTrackOutput (e, e.tracks[1].id, 9999);
        auto back = e;
        const auto toMix = SessionEdits::setTrackOutput (back, back.tracks[0].id, 0) && back.tracks[0].outputBusId == 0;
        const auto frozen = SessionEdits::frozenSource (e, e.tracks[0].id);
        SessionEdits::removeTrack (e, 3);
        check (refused && toMix && e.tracks[0].outputBusId == 0 && frozen.tracks.size() == 1 && frozen.tracks[0].outputBusId == 0,
               "outputs: only into a bus, not itself; back to the mix with 0; deleting the bus sends it to the mix; freezing renders without it");

        auto dir = juce::File::createTempFile ("output");
        dir.createDirectory();
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        auto noClips = out;

        for (auto& t : noClips.tracks)
            t.clips.clear();

        const auto target = dir.getChildFile ("output.sbsession");
        const auto saved = SessionFile::save (noClips, target).isEmpty();
        const auto loaded = SessionFile::load (target, formats);
        const auto& lt = loaded.state.tracks;
        const auto file = saved && lt.size() == 4 && lt[0].outputBusId == lt[3].id && lt[1].outputBusId == 0;
        dir.deleteRecursively();

        const auto xml = TrackTemplate::create (out, out.tracks[0].id, nullptr);
        auto other = s;
        const auto added = TrackTemplate::add (other, *xml);
        const auto* made = other.findTrack (added.trackId);
        SessionState noBuses;
        noBuses.sampleRate = rate;
        const auto missing = TrackTemplate::add (noBuses, *xml);
        check (file && made != nullptr && made->outputBusId == other.tracks[3].id && missing.missingBuses.contains ("Bus 2")
                   && noBuses.findTrack (missing.trackId)->outputBusId == 0,
               "outputs: kept in session files; a track template keeps it by the bus's name, and says when there is no such bus");
    }

    // Bus into bus: X plays into Y in place of the mix; C plays straight into the mix.
    {
        auto chain = s;
        auto& c = chain.addTrack();
        addNoiseClip (chain, c, 2, 2000, 5);
        const auto cMix = alone (chain, 4, 0.0f);
        const auto routedOk = SessionEdits::setTrackOutput (chain, chain.tracks[2].id, chain.tracks[3].id);

        // X's input (A after its volume, B before its) through X's volume and pan, then Y's volume.
        juce::AudioBuffer<float> fromX (2, length);
        fromX.clear();
        add (fromX, aPost, x * xl * y, x * xr * y);
        add (fromX, bPre, gain (-6.0f) * x * xl * y, gain (-6.0f) * x * xr * y);

        juce::AudioBuffer<float> expected (2, length);
        expected.clear();
        add (expected, aPost, 1.0f, 1.0f);
        add (expected, bPost, 1.0f, 1.0f);
        add (expected, fromX, 1.0f, 1.0f);
        add (expected, bPost, gain (-10.0f) * y, gain (-10.0f) * y);
        auto withC = expected;
        add (withC, cMix, 1.0f, 1.0f);
        const auto w = worst (play (chain, 0, length, 512, nullptr), withC);

        // The same with the buses the other way round in the list.
        auto swapped = chain;
        std::swap (swapped.tracks[2], swapped.tracks[3]);
        const auto wSwapped = worst (play (swapped, 0, length, 333, nullptr), withC);
        check (routedOk && w < 1.0e-6f && wSwapped < 1.0e-6f,
               "bus into bus: a bus played into another is heard through both, in either order in the list (worst "
               + juce::String (juce::jmax (w, wSwapped), 9) + ")");

        auto mutedY = chain;
        mutedY.tracks[3].mute = true;
        juce::AudioBuffer<float> noY (2, length);
        noY.clear();
        add (noY, aPost, 1.0f, 1.0f);
        add (noY, bPost, 1.0f, 1.0f);
        add (noY, cMix, 1.0f, 1.0f);
        const auto wm = worst (play (mutedY, 0, length, 512, nullptr), noY);

        // Soloing either bus brings the other and what feeds them, and leaves C out.
        auto soloX = chain;
        soloX.tracks[2].solo = true;
        auto soloY = chain;
        soloY.tracks[3].solo = true;
        const auto wx = worst (play (soloX, 0, length, 512, nullptr), expected);
        const auto wy = worst (play (soloY, 0, length, 512, nullptr), expected);
        check (wm < 1.0e-6f && wx < 1.0e-6f && wy < 1.0e-6f,
               "bus into bus: muting the second bus silences the first; soloing either brings the other and the tracks feeding them (worst "
               + juce::String (juce::jmax (wm, wx, wy), 9) + ")");

        const auto dry = play (chain, 0, length, length, nullptr);
        TrackEffects effects;
        effects.prepare (rate);
        effects.add (chain.tracks[0].id, std::make_unique<DelayPlugin> (300, 1.0f));
        effects.add (chain.tracks[2].id, std::make_unique<DelayPlugin> (500, 1.0f));
        effects.add (chain.tracks[3].id, std::make_unique<DelayPlugin> (64, 1.0f));
        check (same (play (chain, 0, length, 333, &effects), dry) && same (play (chain, 0, length, 2900, &effects), dry)
                   && same (play (chain, 2500, 4000, 256, &effects), slice (dry, 2500, 4000)),
               "bus into bus: with 300, 500 and 64 frames of latency on a track and both buses (564 along the chain), the mix lines up bit for bit, "
               "in 333- and 2900-frame blocks and after priming");

        // Loops of any length are refused; Z after Y is fine.
        auto e = chain;
        const auto& z = e.addBus();
        const auto zId = z.id, xId = e.tracks[2].id, yId = e.tracks[3].id;
        const auto twoLoop = ! SessionEdits::setTrackOutput (e, yId, xId);
        const auto toZ = SessionEdits::setTrackOutput (e, yId, zId);
        const auto threeLoop = ! SessionEdits::setTrackOutput (e, zId, xId);
        const auto viaTrack = ! SessionEdits::setTrackOutput (e, zId, e.tracks[0].id);
        auto removed = e;
        SessionEdits::removeTrack (removed, 3);   // Y
        check (twoLoop && toZ && threeLoop && viaTrack && e.outputBusOf (*e.findTrack (yId)) == e.findTrack (zId)
                   && removed.findTrack (xId)->outputBusId == 0,
               "bus into bus: loops of two and three buses refused, a chain of three allowed; deleting the bus played into sends the first to the mix");

        auto dir = juce::File::createTempFile ("buschain");
        dir.createDirectory();
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        auto noClips = chain;

        for (auto& t : noClips.tracks)
            t.clips.clear();

        const auto target = dir.getChildFile ("chain.sbsession");
        const auto saved = SessionFile::save (noClips, target).isEmpty();
        const auto reloaded = SessionFile::load (target, formats);
        const auto& lt = reloaded.state.tracks;
        const auto file = saved && lt.size() == 5 && lt[2].outputBusId == lt[3].id && lt[3].outputBusId == 0;
        dir.deleteRecursively();

        const auto xml = TrackTemplate::create (chain, chain.tracks[2].id, nullptr);
        auto other = s;
        const auto* made = other.findTrack (TrackTemplate::add (other, *xml).trackId);
        check (file && made != nullptr && made->isBus && made->outputBusId == other.tracks[3].id,
               "bus into bus: kept in session files, and a bus's template keeps its output by the bus's name");
    }

    // Session files keep buses and sends, pointing at the right bus.
    {
        auto dir = juce::File::createTempFile ("bus");
        dir.createDirectory();
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        auto noClips = s;
        for (auto& t : noClips.tracks)
            t.clips.clear();

        const auto target = dir.getChildFile ("bus.sbsession");
        const auto error = SessionFile::save (noClips, target);
        const auto loaded = SessionFile::load (target, formats);
        const auto& t = loaded.state.tracks;
        const auto ok = error.isEmpty() && t.size() == 4 && ! t[0].isBus && t[2].isBus && t[3].isBus && t[2].name == "Bus 1"
                        && t[0].sends.size() == 1 && t[0].sends[0].busId == t[2].id && ! t[0].sends[0].preFader
                        && t[1].sends.size() == 2 && t[1].sends[0].busId == t[2].id && t[1].sends[0].gainDb == -6.0f
                        && t[1].sends[0].preFader && t[1].sends[1].busId == t[3].id && t[2].pan == 0.3f;
        check (ok, "session file: buses and sends round-trip");
        dir.deleteRecursively();
    }
}
