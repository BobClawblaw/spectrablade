// A constant envelope must play exactly as the setting it replaces, so mixes are compared bit for bit.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioEdits.h"
#include "../Source/SessionMixer.h"
#include "../Source/TrackEffects.h"
#include "TestPlugins.h"
#include "TestUtils.h"
#include "AutomationTests.h"

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

    void addClip (SessionState& s, SessionTrack& t, int channels, juce::int64 position, int frames, int seed)
    {
        SessionClip c;
        c.id = s.nextId++;
        c.audio = noise (channels, frames, seed);
        c.position = position;
        c.length = frames;
        t.clips.push_back (c);
    }

    /** A mono track and a stereo one, both sending to a bus: the first before its volume, the second after. */
    SessionState mix()
    {
        SessionState s;
        s.sampleRate = rate;
        auto& a = s.addTrack();
        a.pan = -0.3f;
        a.gainDb = -2.0f;
        addClip (s, a, 1, 0, length, 11);
        auto& b = s.addTrack();
        b.gainDb = 1.5f;
        addClip (s, b, 2, 200, 8000, 12);
        auto& bus = s.addBus();
        bus.gainDb = -4.0f;
        bus.pan = 0.25f;
        SessionEdits::setSend (s, s.tracks[0].id, { s.tracks[2].id, -3.0f, true });
        SessionEdits::setSend (s, s.tracks[1].id, { s.tracks[2].id, -6.0f, false });
        return s;
    }

    juce::AudioBuffer<float> play (const SessionState& s, juce::int64 start, int frames, int blockSize, SessionMixer::Inserts* inserts = nullptr)
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

    Envelope constant (float value)
    {
        Envelope e;
        e.add (0, value);
        return e;
    }

    void testEnvelopes()
    {
        Envelope e;
        e.add (3000, 0.0f);
        e.add (1000, -6.0f);
        check (e.points.size() == 2 && e.points[0].frame == 1000 && e.points[1].frame == 3000, "envelope: points are kept in time order");
        check (e.valueAt (0) == -6.0f && e.valueAt (1000) == -6.0f && e.valueAt (2000) == -3.0f && e.valueAt (3000) == 0.0f
                   && e.valueAt (100000) == 0.0f,
               "envelope: the first value before the first point, straight lines between, the last value after");

        // A step: two points at one frame.
        Envelope step;
        step.add (500, -10.0f);
        step.add (500, 2.0f);
        check (step.valueAt (499) == -10.0f && step.valueAt (500) == 2.0f, "envelope: two points at one frame make a step there");

        auto ok = true;

        for (auto* env : { &e, &step })
        {
            for (auto block : { 1, 7, 512, 6000 })
            {
                std::vector<float> values (6000);

                for (int p = 0; p < 6000; p += block)
                    env->fill (p, values.data() + p, juce::jmin (block, 6000 - p));

                for (int i = 0; i < 6000; ++i)
                    ok = ok && values[(size_t) i] == env->valueAt (i);
            }
        }

        check (ok, "envelope: filled block by block (1, 7, 512, 6000 frames), every value equals valueAt bit for bit");

        e.add (2000, 5.0f);
        e.move (1, 99999, 1.0f);
        check (e.points[1].frame == 3000 && e.points[1].value == 1.0f, "envelope: a point can't be moved past the next one");
        e.move (0, -50, 0.0f);
        check (e.points[0].frame == 0, "envelope: nor before the session start");
        e.remove (1);
        check (e.points.size() == 2, "envelope: a point can be removed");

        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        Envelope wild;
        wild.points = { { 900, 40.0f }, { -20, -200.0f }, { 400, -6.0f } };
        SessionEdits::setEnvelope (s, s.tracks[0].id, AutomationParameter::volume, wild);
        const auto& set = s.tracks[0].volumeEnvelope.points;
        check (set.size() == 3 && set[0] == AutomationPoint { 0, -60.0f } && set[1] == AutomationPoint { 400, -6.0f }
                   && set[2] == AutomationPoint { 900, 12.0f },
               "envelope: setting one sorts its points and keeps them in the fader's range and the session");
        SessionEdits::setEnvelope (s, s.tracks[0].id, AutomationParameter::pan, constant (-3.0f));
        check (s.tracks[0].panEnvelope.points[0].value == -1.0f, "envelope: pan points are kept between left and right");

        s.tracks[0].gainDb = -7.0f;
        s.tracks[0].volumeEnvelope = {};
        check (s.tracks[0].valueAt (AutomationParameter::volume, 1234) == -7.0f
                   && s.tracks[0].valueAt (AutomationParameter::pan, 1234) == -1.0f,
               "envelope: an empty envelope leaves the setting in charge");
    }

    void testFades()
    {
        SessionState s;
        s.sampleRate = rate;
        auto& t = s.addTrack();
        addClip (s, t, 1, 100, 6000, 1);
        const auto id = t.clips[0].id;

        SessionEdits::setClipFades (s, id, 7000, 500);
        check (s.tracks[0].clips[0].fadeIn == 6000 && s.tracks[0].clips[0].fadeOut == 0,
               "fades: a fade in longer than the clip is cut to it, and leaves no room for a fade out");
        SessionEdits::setClipFades (s, id, -5, 200);
        check (s.tracks[0].clips[0].fadeIn == 0 && s.tracks[0].clips[0].fadeOut == 200, "fades: a negative fade is no fade");
        SessionEdits::setClipFades (s, id, 4000, 4000);
        check (s.tracks[0].clips[0].fadeIn == 4000 && s.tracks[0].clips[0].fadeOut == 2000, "fades: the fade out gets what the fade in leaves");
        check (! SessionEdits::setClipFades (s, 9999, 10, 10), "fades: an unknown clip is refused");
    }

    void testSnapping()
    {
        SessionState s;
        s.sampleRate = rate;
        auto& t = s.addTrack();
        addClip (s, t, 1, 1000, 6000, 1);    // 1000 to 7000
        addClip (s, t, 1, 20000, 1000, 2);   // 20000 to 21000
        const auto first = s.tracks[0].clips[0].id;

        SessionSnap::Targets none;
        check (SessionSnap::snap (s, 1005, 10, none) == 1000 && SessionSnap::snap (s, 6992, 10, none) == 7000,
               "snapping: a frame near a clip's start or end lands on it");
        check (SessionSnap::snap (s, 1020, 10, none) == 1020, "snapping: one further than the tolerance stays put");
        check (SessionSnap::snap (s, 7, 10, none) == 0, "snapping: the session start is a target");

        SessionSnap::Targets ignoring;
        ignoring.ignoreClip = first;
        check (SessionSnap::snap (s, 1005, 10, ignoring) == 1005, "snapping: the clip being dragged is not a target");

        SessionSnap::Targets grid;
        grid.gridStep = 4800;
        check (SessionSnap::snap (s, 9603, 10, grid) == 9600, "snapping: the ruler's marks are targets");

        SessionSnap::Targets cursor;
        cursor.extra = { 15000, 1008 };
        check (SessionSnap::snap (s, 14995, 10, cursor) == 15000, "snapping: the cursor is a target");
        check (SessionSnap::snap (s, 1006, 10, cursor) == 1008, "snapping: the nearest target wins");

        check (SessionSnap::snapSpan (s, { 6995, 7495 }, 10, none) == 5, "snapping: a moved clip's start snaps to another's end");
        check (SessionSnap::snapSpan (s, { 18003, 19998 }, 10, ignoring) == 2, "snapping: or its end to another's start");
        check (SessionSnap::snapSpan (s, { 6996, 19999 }, 10, none) == 1, "snapping: when both edges could snap, the nearer one does");
        check (SessionSnap::snapSpan (s, { 30000, 31000 }, 10, ignoring) == 0, "snapping: nothing near, no shift");
    }

    void testMixing()
    {
        const auto s = mix();
        const auto plain = play (s, 0, length, length);

        // A constant envelope is the setting it replaces. Pan envelopes and a bus's volume are
        // bit for bit; a track's volume envelope is its own multiply, where the fader's gain is
        // fused into the add, so it matches to float rounding.
        {
            const auto automate = [] (SessionState state, bool trackVolume)
            {
                for (auto& t : state.tracks)
                {
                    t.panEnvelope = constant (t.pan);
                    t.pan = 0.9f;   // ignored while an envelope is in charge

                    if (t.isBus || trackVolume)
                    {
                        t.volumeEnvelope = constant (t.gainDb);
                        t.gainDb = 9.0f;
                    }
                }

                return state;
            };

            check (same (play (automate (s, false), 0, length, length), plain),
                   "automation: constant pan envelopes on tracks and a bus, and a constant bus volume envelope, play exactly as their settings");

            const auto w = worst (play (automate (s, true), 0, length, length), plain);
            check (w < 1.0e-7f, "automation: constant track volume envelopes too, to float rounding (worst " + juce::String (w, 10) + ")");
        }

        // A volume ramp, on a track alone, gives each frame the gain of the envelope there.
        SessionState one;
        one.sampleRate = rate;
        one.tracks.push_back (s.tracks[1]);
        one.tracks[0].sends.clear();
        one.tracks[0].gainDb = 0.0f;
        const auto unity = play (one, 0, length, length);

        auto ramp = one;
        ramp.tracks[0].volumeEnvelope.add (1000, 0.0f);
        ramp.tracks[0].volumeEnvelope.add (7000, -40.0f);
        const auto ramped = play (ramp, 0, length, length);
        float error = 0.0f;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < length; ++i)
                error = juce::jmax (error, std::abs (ramped.getSample (ch, i) - unity.getSample (ch, i)
                                                         * juce::Decibels::decibelsToGain (ramp.tracks[0].volumeEnvelope.valueAt (i))));

        check (error < 1.0e-7f, "automation: a volume ramp from 0 to -40 dB scales every frame by the envelope's gain there (worst "
                                     + juce::String (error, 10) + ")");

        // Panning a mono track hard left then hard right: all left, then all right.
        auto panned = s;
        panned.tracks.resize (1);
        panned.tracks[0].sends.clear();
        panned.tracks[0].panEnvelope.add (4000, -1.0f);
        panned.tracks[0].panEnvelope.add (4000, 1.0f);
        const auto sides = play (panned, 0, length, 512);
        float leak = 0.0f;

        for (int i = 0; i < length; ++i)
            leak = juce::jmax (leak, std::abs (sides.getSample (i < 4000 ? 1 : 0, i)));   // the side it isn't panned to

        check (leak < 1.0e-6f, "automation: a pan step from left to right moves a mono track across at the step");

        // Envelopes everywhere, played in blocks of any size, mix the same.
        auto busy = s;
        busy.tracks[0].volumeEnvelope.points = { { 500, 0.0f }, { 2500, -20.0f }, { 2500, 3.0f }, { 8000, -9.0f } };
        busy.tracks[0].panEnvelope.points = { { 0, -1.0f }, { 9000, 1.0f } };
        busy.tracks[1].volumeEnvelope.points = { { 3000, -6.0f }, { 3100, 0.0f } };
        busy.tracks[2].volumeEnvelope.points = { { 0, -30.0f }, { 6000, 0.0f } };
        busy.tracks[2].panEnvelope.points = { { 4000, 0.8f }, { 4001, -0.8f } };
        const auto whole = play (busy, 0, length, length);
        check (same (play (busy, 0, length, 1), whole) && same (play (busy, 0, length, 37), whole) && same (play (busy, 0, length, 512), whole),
               "automation: envelopes on tracks and a bus mix the same in 1, 37 and 512-frame blocks");

        // The pre-fader send does not follow the volume envelope; the post-fader one does.
        {
            auto quiet = s;
            quiet.tracks[0].volumeEnvelope = constant (-60.0f);
            quiet.tracks[1].volumeEnvelope = constant (-60.0f);
            auto fader = s;
            fader.tracks[0].gainDb = -60.0f;
            fader.tracks[1].gainDb = -60.0f;
            const auto w = worst (play (quiet, 0, length, 512), play (fader, 0, length, 512));
            check (w < 1.0e-7f, "automation: sends follow a volume envelope as they follow the fader: pre-fader ones not at all (worst "
                                    + juce::String (w, 10) + ")");
        }

        // With effects delaying the track and its bus, the envelope still acts at its own frames.
        {
            TrackEffects effects;
            effects.prepare (rate);
            effects.add (busy.tracks[0].id, std::make_unique<DelayPlugin> (300, 1.0f));
            effects.add (busy.tracks[2].id, std::make_unique<DelayPlugin> (500, 1.0f));
            check (same (play (busy, 0, length, 333, &effects), whole),
                   "automation: with 300 and 500 frames of effect latency, envelopes line up with the audio bit for bit");
            check (same (play (busy, 2500, 4000, 256, &effects), play (busy, 2500, 4000, 4000)),
                   "automation: playing from the middle of a ramp after priming matches");
        }
    }

    void testWriting()
    {
        SessionState s;
        s.sampleRate = rate;
        auto& t = s.addTrack();
        t.gainDb = -6.0f;
        const auto id = t.id;
        const auto track = [&s, id] { return *s.findTrack (id); };

        // Onto a track with no envelope: its setting holds before the pass and after the return.
        check (SessionEdits::writeAutomation (s, id, AutomationParameter::volume, { { 1000, -6.0f }, { 2000, -20.0f }, { 3000, -20.0f } }, 100),
               "writing automation: a pass is written");
        const auto written = track();
        check (written.valueAt (AutomationParameter::volume, 500) == -6.0f && written.valueAt (AutomationParameter::volume, 1500) == -13.0f
                   && written.valueAt (AutomationParameter::volume, 2500) == -20.0f && written.valueAt (AutomationParameter::volume, 3050) == -13.0f
                   && written.valueAt (AutomationParameter::volume, 3100) == -6.0f && written.valueAt (AutomationParameter::volume, 9000) == -6.0f
                   && written.panEnvelope.isEmpty(),
               "writing automation: the setting holds before the pass, the pass plays, and it goes back to the setting over the return");

        // Over a ramp: outside the pass and its return the envelope plays as it did.
        SessionEdits::setEnvelope (s, id, AutomationParameter::volume, { { { 0, 0.0f }, { 10000, -40.0f } } });
        const auto before = track();
        SessionEdits::writeAutomation (s, id, AutomationParameter::volume, { { 2000, -8.0f }, { 3000, 0.0f }, { 4000, -16.0f } }, 500);
        const auto after = track();
        auto outside = 0.0f;

        for (juce::int64 f = 0; f < 12000; f += 50)
            if (f < 2000 || f >= 4500)
                outside = juce::jmax (outside, std::abs (after.valueAt (AutomationParameter::volume, f) - before.valueAt (AutomationParameter::volume, f)));

        check (outside < 1.0e-5f && after.valueAt (AutomationParameter::volume, 3000) == 0.0f && after.volumeEnvelope.points.size() == 7,
               "writing automation: over a ramp, the envelope outside the pass is unchanged (worst " + juce::String (outside, 7) + " dB)");

        // Values are kept in range, and a pass with no points writes nothing.
        SessionEdits::writeAutomation (s, id, AutomationParameter::pan, { { 100, -3.0f }, { 200, 2.0f } }, 0);
        check (track().valueAt (AutomationParameter::pan, 100) == -1.0f && track().panEnvelope.points[2].value == 1.0f
                   && ! SessionEdits::writeAutomation (s, id, AutomationParameter::pan, {}, 0),
               "writing automation: pan values are kept in range; an empty pass writes nothing");
    }

    void testCrossfades()
    {
        // A from 1000 to 5000, B from 4000 to 8000, and C inside B.
        SessionState s;
        s.sampleRate = rate;
        auto& t = s.addTrack();
        addClip (s, t, 1, 1000, 4000, 61);
        addClip (s, t, 1, 4000, 4000, 62);
        addClip (s, t, 1, 5000, 1000, 63);
        const auto a = t.clips[0].id, b = t.clips[1].id, c = t.clips[2].id;
        const auto clip = [&s] (juce::uint32 id) { const auto w = s.findClip (id); return s.tracks[(size_t) w->first].clips[(size_t) w->second]; };

        check (SessionEdits::crossfade (s, b) && clip (b).fadeIn == 1000 && clip (a).fadeOut == 1000 && clip (b).fadeOut == 0
                   && clip (c).fadeIn == 0 && clip (c).fadeOut == 0,
               "crossfades: a clip over another's end fades in as the other fades out, across the overlap; a clip inside it is left alone");

        SessionEdits::setClipFades (s, a, 0, 0);
        SessionEdits::setClipFades (s, b, 0, 0);
        check (SessionEdits::crossfade (s, a) && clip (a).fadeOut == 1000 && clip (b).fadeIn == 1000,
               "crossfades: the same from the earlier clip");
        check (! SessionEdits::crossfade (s, c) && clip (c).fadeIn == 0 && clip (c).fadeOut == 0,
               "crossfades: a clip inside another has nothing to crossfade with");

        // B moved between A and D, overlapping both: it crossfades with each.
        SessionEdits::trimClip (s, c, 5000, 5001);
        SessionEdits::moveClip (s, c, 0, 20000);
        addClip (s, s.tracks[0], 1, 6000, 4000, 64);
        const auto d = s.tracks[0].clips.back().id;
        SessionEdits::moveClip (s, b, 0, 4500);
        check (SessionEdits::crossfade (s, b) && clip (b).fadeIn == 500 && clip (b).fadeOut == 2500 && clip (d).fadeIn == 2500,
               "crossfades: a clip overlapping clips on both sides crossfades with each");

        // The mix through a crossfade: two clips of the same constant level stay at that level,
        // to within the curves' half-frame offset (sin (pi / 2N) for an N-frame overlap).
        for (const juce::int64 overlap : { 2, 7, 480, 4800 })
        {
            SessionState m;
            m.sampleRate = rate;
            auto& track = m.addTrack();

            for (const juce::int64 start : { (juce::int64) 0, 6000 - overlap })
            {
                juce::AudioBuffer<float> level (2, 6000);

                for (int ch = 0; ch < 2; ++ch)
                    juce::FloatVectorOperations::fill (level.getWritePointer (ch), 0.25f, 6000);

                SessionClip clipAtLevel;
                clipAtLevel.id = m.nextId++;
                clipAtLevel.audio = AudioEdits::fromBuffer (std::move (level));
                clipAtLevel.position = start;
                clipAtLevel.length = 6000;
                track.clips.push_back (clipAtLevel);
            }

            SessionEdits::crossfade (m, m.tracks[0].clips[1].id);
            const auto mix = play (m, 0, (int) (12000 - overlap), 512);
            auto deviation = 0.0f;

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < mix.getNumSamples(); ++i)
                    deviation = juce::jmax (deviation, std::abs (mix.getSample (ch, i) / 0.25f - 1.0f));

            const auto bound = std::sin (juce::MathConstants<float>::pi / (2.0f * (float) overlap));
            check (deviation <= bound + 1.0e-6f,
                   "crossfades: through a " + juce::String (overlap) + "-frame crossfade the level stays within "
                       + juce::String (bound, 6) + " of steady (worst " + juce::String (deviation, 6) + ")");
        }
    }

    void testFile (juce::AudioFormatManager& formats)
    {
        SessionState s;
        s.sampleRate = rate;
        auto& t = s.addTrack();
        t.volumeEnvelope.points = { { 0, -6.5f }, { 48000, 3.25f }, { 48000, -60.0f } };
        t.panEnvelope.points = { { 1200, -0.5f } };
        auto& bus = s.addBus();
        bus.panEnvelope.points = { { 7, 1.0f }, { 96000, -1.0f } };

        const auto file = juce::File::createTempFile (".sbsession");
        const auto error = SessionFile::save (s, file);
        const auto loaded = SessionFile::load (file, formats);
        file.deleteFile();

        check (error.isEmpty() && loaded.error.isEmpty() && loaded.state.tracks.size() == 2
                   && loaded.state.tracks[0].volumeEnvelope == s.tracks[0].volumeEnvelope
                   && loaded.state.tracks[0].panEnvelope == s.tracks[0].panEnvelope
                   && loaded.state.tracks[1].panEnvelope == s.tracks[1].panEnvelope
                   && loaded.state.tracks[1].volumeEnvelope.isEmpty(),
               "automation: envelopes on a track and a bus come back from a session file exactly");
    }
}

void runAutomationTests (juce::AudioFormatManager& formats)
{
    testEnvelopes();
    testFades();
    testSnapping();
    testCrossfades();
    testWriting();
    testMixing();
    testFile (formats);
}
