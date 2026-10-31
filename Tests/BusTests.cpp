// Delay compensation through buses must be exact, so mixes are compared bit for bit.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/SessionMixer.h"
#include "../Source/TrackEffects.h"
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
