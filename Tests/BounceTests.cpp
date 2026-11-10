#include <JuceHeader.h>
#include "../Source/AudioEdits.h"
#include "../Source/Bounce.h"
#include "../Source/OfflineRender.h"
#include "../Source/PhaseVocoder.h"
#include "../Source/Sequences.h"
#include "../Source/TimeStretch.h"
#include "../Source/TrackTemplate.h"
#include "TestUtils.h"
#include "BounceTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    AudioDocument::Snapshot ramp (int channels, int frames, float scale)
    {
        juce::AudioBuffer<float> b (channels, frames);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, scale * (float) (ch + 1) * std::sin ((float) i * 0.01f));

        return AudioEdits::fromBuffer (std::move (b));
    }

    juce::AudioBuffer<float> mix (const SessionState& s, juce::Range<juce::int64> range)
    {
        juce::AudioBuffer<float> out;
        OfflineRender::mixSession (s, range, out, nullptr, [] (float) { return true; });
        return out;
    }

    bool identical (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
            return false;

        for (int ch = 0; ch < a.getNumChannels(); ++ch)
            if (std::memcmp (a.getReadPointer (ch), b.getReadPointer (ch), sizeof (float) * (size_t) a.getNumSamples()) != 0)
                return false;

        return true;
    }

    float largestDifference (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        auto most = 0.0f;

        for (int ch = 0; ch < a.getNumChannels(); ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
                most = juce::jmax (most, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));

        return most;
    }

    /** A mono track panned left sending to a bus after its volume, a stereo track, the bus. */
    SessionState session (float guitarDb)
    {
        SessionState s;
        s.sampleRate = rate;
        auto& guitar = s.addTrack();
        guitar.name = "Guitar";
        guitar.pan = -0.3f;
        guitar.gainDb = guitarDb;
        s.addTrack().name = "Keys";
        auto& bus = s.addBus();
        bus.gainDb = -3.0f;
        const auto busId = bus.id;
        SessionEdits::setSend (s, s.tracks[0].id, { busId, -10.0f, false });
        SessionEdits::addClip (s, 0, juce::File ("/takes/guitar.wav"), ramp (1, 9000, 0.5f), 1000);
        SessionEdits::addClip (s, 1, juce::File ("/takes/keys.wav"), ramp (2, 12000, 0.2f), 0);
        return s;
    }
}

namespace
{
    // Frequency from the rising zero crossings between two sample indices.
    double frequencyOf (const float* x, int from, int to, double sampleRate)
    {
        std::vector<double> rises;

        for (int i = from + 1; i < to; ++i)
            if (x[i - 1] < 0.0f && x[i] >= 0.0f)
                rises.push_back ((double) (i - 1) + (double) x[i - 1] / (double) (x[i - 1] - x[i]));

        return rises.size() < 2 ? 0.0 : sampleRate * (double) (rises.size() - 1) / (rises.back() - rises.front());
    }

    void testTimeStretch()
    {
        constexpr int length = 96000;
        juce::AudioBuffer<float> tone (2, length);

        for (int i = 0; i < length; ++i)
        {
            const auto v = 0.5f * std::sin (juce::MathConstants<float>::twoPi * 440.0f * (float) i / (float) rate);
            tone.setSample (0, i, v);
            tone.setSample (1, i, -v);
        }

        const auto inputFrequency = frequencyOf (tone.getReadPointer (0), 24000, 72000, rate);
        auto lengthsRight = true, pitchKept = true, levelKept = true, inStep = true;
        juce::String found;

        for (auto ratio : { 0.5, 0.75, 1.5, 2.0 })
        {
            const auto out = TimeStretch::stretch (tone, ratio, rate);
            const auto n = out.getNumSamples();
            lengthsRight = lengthsRight && n == (int) std::llround (length * ratio);

            // The middle half, away from the ends.
            const auto f = frequencyOf (out.getReadPointer (0), n / 4, 3 * n / 4, rate);
            const auto rms = out.getRMSLevel (0, n / 4, n / 2);
            pitchKept = pitchKept && std::abs (f / inputFrequency - 1.0) < 0.005;
            levelKept = levelKept && std::abs (juce::Decibels::gainToDecibels (rms / (0.5f / std::sqrt (2.0f)))) < 1.0f;
            found << juce::String (ratio, 2) << "x: " << juce::String (f, 2) << " Hz, " << juce::String (juce::Decibels::gainToDecibels (rms), 2) << " dBFS; ";

            for (int i = 0; i < n && inStep; ++i)
                inStep = std::abs (out.getSample (1, i) + out.getSample (0, i)) < 1.0e-3f;
        }

        check (lengthsRight && pitchKept && levelKept && inStep,
               "time stretch: lengths from 50% to 200%, a 440 Hz tone's pitch within 0.5% and level within 1 dB, channels in step (opposite channels within 0.001 of opposite) ("
                   + found.trimEnd() + " input " + juce::String (inputFrequency, 2) + " Hz)");

        const auto started = juce::Time::getMillisecondCounterHiRes();
        juce::AudioBuffer<float> long10 (2, 480000);
        long10.clear();
        long10.copyFrom (0, 0, tone, 0, 0, length);
        const auto stretched = TimeStretch::stretch (long10, 1.5, rate);
        const auto ms = juce::Time::getMillisecondCounterHiRes() - started;
        check (stretched.getNumSamples() == 720000, "time stretch: 10 s of stereo to 15 s in " + juce::String (ms, 0) + " ms");

        // Progress as it goes, and cancelling part way.
        std::vector<float> heard;
        const auto whole = TimeStretch::stretch (tone, 1.5, rate, [&heard] (float p) { heard.push_back (p); return true; });
        auto calls = 0;
        const auto cancelled = TimeStretch::stretch (tone, 1.5, rate, [&calls] (float) { return ++calls < 3; });
        check (whole.getNumSamples() == 144000 && heard.size() > 1 && std::is_sorted (heard.begin(), heard.end()) && heard.front() >= 0.0f
                   && heard.back() < 1.0f && cancelled.getNumSamples() == 0 && calls == 3,
               "time stretch: progress rises from 0 towards 1 as it goes, and stopping it part way gives nothing");

        // Pitch shifts keep the length and move the tone by the ratio.
        auto shiftsRight = true;
        juce::String shifted;

        for (auto semitones : { -12.0, -5.0, 7.0, 12.0 })
        {
            const auto out = TimeStretch::shiftPitch (tone, semitones, rate);
            const auto n = out.getNumSamples();
            const auto f = frequencyOf (out.getReadPointer (0), n / 4, 3 * n / 4, rate);
            const auto wanted = inputFrequency * std::pow (2.0, semitones / 12.0);
            const auto rms = out.getRMSLevel (0, n / 4, n / 2);
            auto inverse = true;

            for (int i = 0; i < n && inverse; ++i)
                inverse = std::abs (out.getSample (1, i) + out.getSample (0, i)) < 1.0e-3f;

            shiftsRight = shiftsRight && n == length && std::abs (f / wanted - 1.0) < 0.005 && inverse
                          && std::abs (juce::Decibels::gainToDecibels (rms / (0.5f / std::sqrt (2.0f)))) < 1.0f;
            shifted << juce::String (semitones, 0) << ": " << juce::String (f, 2) << " Hz (" << juce::String (wanted, 2) << "), "
                    << juce::String (juce::Decibels::gainToDecibels (rms), 2) << " dBFS; ";
        }

        auto shiftCalls = 0;
        check (shiftsRight && TimeStretch::shiftPitch (tone, 5.0, rate, [&shiftCalls] (float) { return ++shiftCalls < 2; }).getNumSamples() == 0,
               "pitch shift: the same length, a 440 Hz tone within 0.5% of each shifted pitch and within 1 dB, channels in step (within 0.001), and cancelling ("
                   + shifted.trimEnd() + ")");

        // Attacks: a click every 250 ms comes out once each, where the stretch puts it (WSOLA, before,
        // doubled ten of 16 clicks at 200% and lost five at 50%).
        {
            juce::AudioBuffer<float> clicks (2, 192000);
            clicks.clear();
            std::vector<double> at;

            for (int t = 6000; t + 200 < clicks.getNumSamples(); t += 12000)
            {
                at.push_back (t);

                for (int i = 0; i < 96; ++i)
                {
                    const auto v = 0.8f * std::sin (juce::MathConstants<float>::twoPi * 3000.0f * (float) i / (float) rate) * std::exp (-(float) i / 24.0f);
                    clicks.setSample (0, i + t, v);
                    clicks.setSample (1, i + t, v);
                }
            }

            auto clicksKept = true;
            juce::String clickCounts;

            for (auto ratio : { 0.5, 0.75, 1.25, 1.5, 2.0 })
            {
                const auto out = TimeStretch::stretch (clicks, ratio, rate);
                const auto* x = out.getReadPointer (0);
                const auto loudest = out.getMagnitude (0, 0, out.getNumSamples());
                std::vector<int> onsets;
                auto above = false;

                for (int i = 0; i < out.getNumSamples(); ++i)
                {
                    const auto a = std::abs (x[i]) > 0.1f * loudest;

                    if (a && ! above && (onsets.empty() || i - onsets.back() > (int) (0.04 * rate)))
                        onsets.push_back (i);

                    above = a;
                }

                auto placed = onsets.size() == at.size();

                for (size_t k = 0; k < onsets.size() && placed; ++k)
                    placed = std::abs ((double) onsets[k] - at[k] * ratio) < 0.005 * rate;

                clicksKept = clicksKept && placed;
                clickCounts << juce::String (ratio, 2) << "x: " << (int) onsets.size() << "; ";
            }

            check (clicksKept, "time stretch: 16 clicks stay 16 from 50% to 200%, each within 5 ms of where the stretch puts it ("
                                   + clickCounts.trimEnd() + ")");
        }

        // On a clip: the part it plays, its fades scaled, and not when locked.
        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        const auto id = SessionEdits::addClip (s, 0, juce::File ("/takes/tone.wav"), AudioEdits::fromBuffer (juce::AudioBuffer<float> (tone)), 5000);
        SessionEdits::trimClip (s, id, 5000 + 1000, 5000 + 49000);   // plays 48000 frames
        SessionEdits::setClipFades (s, id, 4800, 2400);
        const auto audio = SessionEdits::stretchedAudio (s.tracks[0].clips[0], 1.5, rate);
        auto pitched = s;
        const auto up = SessionEdits::pitchedAudio (s.tracks[0].clips[0], 3.0, rate);
        const auto pitchedApplied = SessionEdits::stretchClip (pitched, id, juce::File ("/takes/tone Shifted.wav"), up);
        const auto& p = pitched.tracks[0].clips[0];
        check (pitchedApplied && up->getNumFrames() == 48000 && p.length == 48000 && p.fadeIn == 4800 && p.fadeOut == 2400 && p.position == 6000,
               "pitch shift: a clip keeps its place, length and fades");
        auto locked = s;
        SessionEdits::setClipLocked (locked, id, true);
        const auto refused = ! SessionEdits::stretchClip (locked, id, juce::File ("/takes/tone Stretched.wav"), audio);
        const auto applied = SessionEdits::stretchClip (s, id, juce::File ("/takes/tone Stretched.wav"), audio);
        const auto& c = s.tracks[0].clips[0];
        check (refused && applied && audio->getNumFrames() == 72000 && c.position == 6000 && c.offset == 0 && c.length == 72000
                   && c.fadeIn == 7200 && c.fadeOut == 3600 && c.audio == audio && c.source.getFileName() == "tone Stretched.wav",
               "time stretch: a clip plays its stretched part from where it starts, fades scaled with it, and a locked one is refused");

        // With takes: each processed alike from the part the clip plays, and kept, lined up.
        const auto noise = [] (int seed, int frameCount)
        {
            juce::Random r (seed);
            juce::AudioBuffer<float> b (2, frameCount);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < frameCount; ++i)
                    b.setSample (ch, i, r.nextFloat() - 0.5f);

            return AudioEdits::fromBuffer (std::move (b));
        };
        const auto frames = [] (const AudioSnapshot& a, juce::int64 from, int count)
        {
            juce::AudioBuffer<float> b (2, count);
            a.read (b, 0, from, count);
            return b;
        };
        const auto same = [] (const juce::AudioBuffer<float>& x, const juce::AudioBuffer<float>& y)
        {
            if (x.getNumSamples() != y.getNumSamples())
                return false;

            for (int ch = 0; ch < 2; ++ch)
                if (std::memcmp (x.getReadPointer (ch), y.getReadPointer (ch), sizeof (float) * (size_t) x.getNumSamples()) != 0)
                    return false;

            return true;
        };

        SessionState t;
        t.sampleRate = rate;
        t.addTrack();
        const auto takeA = noise (1, 60000), takeB = noise (2, 60000), takeC = noise (3, 40000);
        const auto tid = SessionEdits::addClip (t, 0, juce::File ("/takes/a.wav"), takeA, 5000);
        auto& tc = t.tracks[0].clips[0];
        tc.takes = { { juce::File ("/takes/a.wav"), takeA, 0 }, { juce::File ("/takes/b.wav"), takeB, 300 }, { juce::File ("/takes/c.wav"), takeC, -200 } };
        SessionEdits::trimClip (t, tid, 5000 + 1000, 5000 + 49000);       // plays take A's frames 1000 to 49000
        SessionEdits::chooseTake (t, tid, 1);                               // take B's 1300 to 49300
        const auto before = t.tracks[0].clips[0];
        const auto asA = SessionEdits::takeAsPlayed (before, 0), asC = SessionEdits::takeAsPlayed (before, 2), asB = SessionEdits::takeAsPlayed (before, 1);
        const auto lined = before.offset == 1300 && asA.offset == 0 && asA.length == 48000 && asA.takes.empty()
                           && same (frames (*asA.audio, 0, 48000), frames (*takeA, 1000, 48000))
                           && same (frames (*asC.audio, 0, 48000), frames (*takeC, 800, 48000))        // silence past take C's 40000 frames
                           && asC.audio->getNumFrames() == 48000 && asB.audio == before.audio && asB.offset == before.offset;

        std::vector<SessionClip::Take> processed (3);

        for (int i : { 0, 2 })
            processed[(size_t) i] = { juce::File ("/takes/x" + juce::String (i) + ".wav"), SessionEdits::stretchedAudio (SessionEdits::takeAsPlayed (before, i), 1.5, rate), 0 };

        const auto active = SessionEdits::stretchedAudio (before, 1.5, rate);
        auto mismatched = processed;
        mismatched[0].audio = noise (4, 100);
        auto refusedTakes = t;
        const auto refusedMismatch = ! SessionEdits::stretchClip (refusedTakes, tid, juce::File ("/takes/b x.wav"), active, false, mismatched);
        const auto keptTakes = SessionEdits::stretchClip (t, tid, juce::File ("/takes/b x.wav"), active, false, processed);
        const auto after = t.tracks[0].clips[0];
        auto chosen = t;
        const auto choseA = SessionEdits::chooseTake (chosen, tid, 0);
        const auto& ca = chosen.tracks[0].clips[0];
        check (lined && refusedMismatch && keptTakes && after.takes.size() == 3 && after.activeTake == 1 && after.length == 72000
                   && after.takes[1].audio == active && after.takes[1].source.getFileName() == "b x.wav"
                   && after.takes[0].shift == 0 && after.takes[2].shift == 0
                   && choseA && ca.position == 6000 && ca.length == 72000 && ca.offset == 0 && ca.audio == processed[0].audio,
               "takes: stretched, a clip keeps its three takes, each stretched alike from what the clip played of it (one shorter "
               "than the clip padded with silence), lined up so choosing another plays it in the same place and length; takes "
               "of other lengths are refused");

        // The Pitch Editor's result replaces the active take alone; the others play as they did.
        SessionState u;
        u.sampleRate = rate;
        u.addTrack();
        const auto uid = SessionEdits::addClip (u, 0, juce::File ("/takes/a.wav"), takeA, 5000);
        u.tracks[0].clips[0].takes = before.takes;
        u.tracks[0].clips[0].takes[0] = { juce::File ("/takes/a.wav"), takeA, 0 };
        SessionEdits::trimClip (u, uid, 5000 + 1000, 5000 + 49000);
        SessionEdits::chooseTake (u, uid, 1);
        const auto was = u.tracks[0].clips[0];
        const auto edited = noise (5, 48000);
        auto wrongLength = u;
        const auto refusedLength = ! SessionEdits::replaceActiveTake (wrongLength, uid, juce::File ("/takes/b Notes.wav"), noise (5, 47999));
        const auto replaced = SessionEdits::replaceActiveTake (u, uid, juce::File ("/takes/b Notes.wav"), edited);
        const auto now = u.tracks[0].clips[0];
        auto others = true;

        for (int i : { 0, 2 })
            others = others && same (frames (*SessionEdits::takeAsPlayed (now, i).audio, 0, 48000), frames (*SessionEdits::takeAsPlayed (was, i).audio, 0, 48000));

        auto toC = u;
        const auto choseC = SessionEdits::chooseTake (toC, uid, 2);
        const auto& cc = toC.tracks[0].clips[0];
        auto toCBefore = SessionState();
        toCBefore.sampleRate = rate;
        toCBefore.addTrack();
        toCBefore.tracks[0].clips.push_back (was);
        SessionEdits::chooseTake (toCBefore, was.id, 2);
        const auto& cb = toCBefore.tracks[0].clips[0];
        check (refusedLength && replaced && now.audio == edited && now.offset == 0 && now.takes.size() == 3 && now.takes[1].audio == edited
                   && now.position == was.position && now.length == was.length && others
                   && choseC && cc.position == cb.position && cc.length == cb.length && cc.audio == cb.audio && cc.offset == cb.offset,
               "takes: with notes moved, the active take is replaced and the other two play the same frames in the same place as before, "
               "also once chosen; audio of another length is refused");

        // Reversed, each take turns round alike and they stay lined up.
        SessionState v;
        v.sampleRate = rate;
        v.addTrack();
        v.tracks[0].clips.push_back (before);
        std::vector<SessionClip::Take> backwards (3);

        for (int i : { 0, 2 })
            backwards[(size_t) i] = { juce::File ("/takes/r" + juce::String (i) + ".wav"), SessionEdits::reversedAudio (SessionEdits::takeAsPlayed (before, i)), 0 };

        const auto reversedActive = SessionEdits::reversedAudio (before);
        auto wrong = v;
        auto shortTakes = backwards;
        shortTakes[2].audio = noise (6, 10);
        const auto refusedShort = ! SessionEdits::reverseClip (wrong, before.id, juce::File ("/takes/b r.wav"), reversedActive, shortTakes);
        const auto reversedKept = SessionEdits::reverseClip (v, before.id, juce::File ("/takes/b r.wav"), reversedActive, backwards);
        auto toA = v;
        const auto choseFirst = SessionEdits::chooseTake (toA, before.id, 0);
        const auto& ra = toA.tracks[0].clips[0];
        auto turned = true;
        const auto aPlayed = frames (*asA.audio, 0, 48000), aNow = frames (*ra.audio, 0, 48000);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 48000 && turned; ++i)
                turned = juce::exactlyEqual (aNow.getSample (ch, i), aPlayed.getSample (ch, 47999 - i));

        check (refusedShort && reversedKept && v.tracks[0].clips[0].takes.size() == 3 && v.tracks[0].clips[0].activeTake == 1
                   && choseFirst && ra.position == before.position && ra.length == 48000 && ra.offset == 0 && turned,
               "takes: reversed, a clip keeps its three takes, each what the clip played of it back to front, so choosing another "
               "plays it reversed in the same place; takes of other lengths are refused");
    }
}

namespace
{
    /** A 440 Hz tone at 0.5, as a snapshot: left, and right the inverse. */
    AudioDocument::Snapshot toneFile (int length)
    {
        juce::AudioBuffer<float> b (2, length);

        for (int i = 0; i < length; ++i)
        {
            const auto v = 0.5f * std::sin (juce::MathConstants<float>::twoPi * 440.0f * (float) i / (float) rate);
            b.setSample (0, i, v);
            b.setSample (1, i, -v);
        }

        return AudioEdits::fromBuffer (std::move (b));
    }

    juce::AudioBuffer<float> whole (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    /** The largest step from one sample to the next over [from, to) of a channel. */
    float largestStep (const juce::AudioBuffer<float>& b, int from, int to)
    {
        auto most = 0.0f;

        for (int i = juce::jmax (1, from); i < juce::jmin (to, b.getNumSamples()); ++i)
            most = juce::jmax (most, std::abs (b.getSample (0, i) - b.getSample (0, i - 1)));

        return most;
    }

    void testStretchInFiles()
    {
        // The edges keep their level: shortened, the start no longer fades in from silence, and
        // lengthened, the end no longer fades out.
        const auto tone = whole (*toneFile (48000));
        auto edgesKept = true;
        juce::String edges;

        for (auto ratio : { 0.5, 0.75, 1.5, 2.0 })
        {
            const auto out = TimeStretch::stretch (tone, ratio, rate);
            const auto n = out.getNumSamples();
            const auto first = out.getMagnitude (0, 0, 128), last = out.getMagnitude (0, n - 128, 128);
            edgesKept = edgesKept && first > 0.45f && last > 0.45f;
            edges << juce::String (ratio, 2) << "x: " << juce::String (first, 3) << " / " << juce::String (last, 3) << "; ";
        }

        check (edgesKept, "time stretch: the first and last 128 samples keep the tone's level (peaks " + edges.trimEnd() + " of 0.5)");

        // The middle second of a 3 s tone stretched to 150%: the audio either side is untouched,
        // the file is 0.5 s longer, and the joins don't click.
        const auto file = toneFile (144000);
        const auto original = whole (*file);
        const auto naturalStep = largestStep (original, 1, 144000);
        const auto stretched = TimeStretch::stretchRange (*file, 48000, 96000, 1.5, rate);
        const auto s = whole (*stretched);
        auto outsideKept = s.getNumSamples() == 168000;

        for (int ch = 0; ch < 2 && outsideKept; ++ch)
            for (int i = 0; i < 48000 && outsideKept; ++i)
                outsideKept = juce::exactlyEqual (s.getSample (ch, i), original.getSample (ch, i)) && juce::exactlyEqual (s.getSample (ch, 120000 + i), original.getSample (ch, 96000 + i));

        const auto joinStart = largestStep (s, 47900, 48600), joinEnd = largestStep (s, 119400, 120100);
        const auto stretchedPitch = frequencyOf (s.getReadPointer (0), 60000, 108000, rate);
        check (outsideKept && joinStart < 1.5f * naturalStep && joinEnd < 1.5f * naturalStep && std::abs (stretchedPitch / 440.0 - 1.0) < 0.005,
               "stretch in a file: the middle second of a 3 s tone at 150% makes it 3.5 s, the audio either side bit for bit as it was, "
               "the pitch kept (" + juce::String (stretchedPitch, 2) + " Hz), and no click at the joins (largest steps "
                   + juce::String (joinStart, 4) + " and " + juce::String (joinEnd, 4) + " against the tone's " + juce::String (naturalStep, 4) + ")");

        const auto shifted = TimeStretch::shiftRange (*file, 48000, 96000, 7.0, rate);
        const auto p = whole (*shifted);
        auto pitchOutside = p.getNumSamples() == 144000;

        for (int i = 0; i < 48000 && pitchOutside; ++i)
            pitchOutside = juce::exactlyEqual (p.getSample (0, i), original.getSample (0, i)) && juce::exactlyEqual (p.getSample (0, 96000 + i), original.getSample (0, 96000 + i));

        const auto shiftedPitch = frequencyOf (p.getReadPointer (0), 54000, 90000, rate), wanted = 440.0 * std::pow (2.0, 7.0 / 12.0);
        const auto pitchJoins = juce::jmax (largestStep (p, 47900, 48600), largestStep (p, 95400, 96100));
        check (pitchOutside && std::abs (shiftedPitch / wanted - 1.0) < 0.005 && pitchJoins < 1.5f * (float) (wanted / 440.0) * naturalStep,
               "shift pitch in a file: the middle second 7 semitones up (" + juce::String (shiftedPitch, 2) + " Hz for " + juce::String (wanted, 2)
                   + "), the same length, the audio either side bit for bit, and no click at the joins (largest step " + juce::String (pitchJoins, 4) + ")");

        // Markers keep their place in a stretched range; none is lost.
        const Markers markers { { 1000, 0, "before", -1 }, { 48000, 0, "at start", -1 }, { 60000, 0, "inside", -1 },
                                { 90000, 12000, "across end", -1 }, { 100000, 0, "after", -1 } };
        const auto moved = MarkerEdits::afterStretch (markers, 48000, 96000, 72000);
        check (moved.size() == 5 && moved[0].start == 1000 && moved[1].start == 48000 && moved[2].start == 66000 && moved[3].start == 111000
                   && moved[3].getEnd() == 126000 && moved[4].start == 124000,
               "stretch in a file: markers before it stay, those inside keep their place in it, those after move on, and a region across "
                   "its end grows with it");

        // As sequence steps: names, ranges, markers, refusals, nothing at 100% or 0, and stopping.
        using Sequences::Step;
        const Step stretch { "stretch", { { "percent", 150.0 } }, nullptr }, pitch { "pitch", { { "semitones", 2.0 } }, nullptr };
        const auto r = Sequences::apply (stretch, file, 48000, 96000, rate, markers, nullptr);
        const auto same = Sequences::apply ({ "stretch", { { "percent", 100.0 } }, nullptr }, file, 48000, 96000, rate, markers, nullptr);
        const auto unshifted = Sequences::apply ({ "pitch", { { "semitones", 0.0 } }, nullptr }, file, 48000, 96000, rate, markers, nullptr);
        auto calls = 0;
        const auto stopped = Sequences::apply (pitch, file, 48000, 96000, rate, markers, nullptr, [&calls] (float) { return ++calls < 2; });
        const auto tooLong = Sequences::problem ({ "stretch", { { "percent", 250.0 } }, nullptr }, rate);
        const auto tooHigh = Sequences::problem ({ "pitch", { { "semitones", 13.0 } }, nullptr }, rate);
        Sequences::Sequence both { "both", { stretch, { "gain", { { "db", -6.0 } }, nullptr } } };
        const auto run = Sequences::run (both, file, 48000, 96000, rate, {}, nullptr);
        const auto ran = whole (*run.snapshot);
        const auto gainOnStretched = std::abs (ran.getMagnitude (0, 60000, 48000) - 0.5f * juce::Decibels::decibelsToGain (-6.0f)) < 0.01f
                                     && std::abs (ran.getMagnitude (0, 0, 48000) - 0.5f) < 0.001f && std::abs (ran.getMagnitude (0, 121000, 47000) - 0.5f) < 0.001f;
        check (r.error.isEmpty() && r.snapshot != nullptr && r.snapshot->getNumFrames() == 168000 && r.start == 48000 && r.end == 120000
                   && r.markers == moved && same.snapshot == nullptr && unshifted.snapshot == nullptr && stopped.error == "Stopped."
                   && tooLong.isNotEmpty() && tooHigh.isNotEmpty() && stretch.name() == "Stretch to 150.0%" && pitch.name() == "Shift Pitch +2.00 Semitones"
                   && run.error.isEmpty() && run.end == 120000 && gainOnStretched,
               "stretch and shift pitch as sequence steps: the range and markers follow the new length (so a step after acts on the stretched "
               "part), nothing at 100% or 0 semitones, out-of-range settings refused, and stopping part way changes nothing (peaks after -6 dB: "
                   + juce::String (ran.getMagnitude (0, 60000, 48000), 4) + " inside, " + juce::String (ran.getMagnitude (0, 0, 48000), 4) + " and "
                   + juce::String (ran.getMagnitude (0, 121000, 47000), 4) + " outside; stopping: \"" + stopped.error + "\")");
    }
}

namespace
{
    void testFreeze()
    {
        // The guitar panned, sent to a bus after its volume, with a volume envelope: frozen, it
        // plays its rendered audio through the same volume, envelope and send.
        for (auto db : { 0.0f, -6.0f })
        {
            auto s = session (db);
            const auto guitar = s.tracks[0].id;
            Envelope ride;
            ride.points = { { 0, 0.0f }, { 6000, -9.0f }, { 12000, 2.0f } };
            SessionEdits::setEnvelope (s, guitar, AutomationParameter::volume, ride);
            const juce::Range<juce::int64> range (0, 14000);
            const auto before = mix (s, range);
            auto frozenAudio = AudioEdits::fromBuffer (mix (SessionEdits::frozenSource (s, guitar), { 0, 10000 + 2000 }));

            auto frozen = s;
            const auto froze = SessionEdits::freezeTrack (frozen, guitar, juce::File ("/takes/Guitar Frozen.wav"), frozenAudio);
            const auto after = mix (frozen, range);
            const auto most = largestDifference (before, after);
            const auto& source = SessionEdits::frozenSource (s, guitar).tracks;

            check (froze && frozen.tracks[0].isFrozen() && ! frozen.tracks[0].takesClips() && source.size() == 1 && source[0].sends.empty()
                       && juce::exactlyEqual (source[0].gainDb, 0.0f) && source[0].volumeEnvelope.points.empty() && juce::exactlyEqual (source[0].pan, -0.3f)
                       && frozenAudio->getNumChannels() == 2,
                   "freeze: renders the track alone with its pan, at 0 dB with no envelope or sends, in stereo");
            check (identical (before, after),
                   "freeze: at " + juce::String (db) + " dB with a volume envelope and a send, the mix is bit for bit the live one (largest difference "
                       + juce::String (most, 10) + ")");

            auto unfrozen = frozen;
            check (SessionEdits::unfreezeTrack (unfrozen, guitar) && ! unfrozen.tracks[0].isFrozen() && identical (mix (unfrozen, range), before)
                       && ! SessionEdits::unfreezeTrack (unfrozen, guitar),
                   "freeze: unfreezing plays the clips live again, mixing as before");
        }

        // While frozen its clips stay as they are, and new ones go onto a new track.
        auto s = session (0.0f);
        const auto guitar = s.tracks[0].id;
        const auto clip = s.tracks[0].clips[0].id;
        const auto busFrozen = SessionEdits::freezeTrack (s, s.tracks[2].id, {}, ramp (2, 100, 0.1f));
        SessionEdits::freezeTrack (s, guitar, juce::File ("/takes/Guitar Frozen.wav"), ramp (2, 12000, 0.1f));
        const auto kept = s;
        const auto refused = ! SessionEdits::moveClip (s, clip, 1, 5000) && ! SessionEdits::splitClip (s, clip, 3000)
                             && ! SessionEdits::setClipMuted (s, clip, true);
        auto cleared = s;
        SessionEdits::clearTime (cleared, 0, 100000);
        const auto clipAfter = cleared.tracks[0].clips;
        const auto added = SessionEdits::addClip (s, 0, juce::File ("/takes/more.wav"), ramp (1, 500, 0.1f), 0);
        const auto moved = SessionEdits::moveClip (s, s.tracks[1].clips[0].id, 0, 2000);

        check (! busFrozen && refused && clipAfter.size() == 1 && clipAfter[0].position == kept.tracks[0].clips[0].position
                   && clipAfter[0].length == kept.tracks[0].clips[0].length && s.tracks[0].clips.size() == 1
                   && s.tracks.size() == 4 && s.tracks[3].clips.size() == 1 && s.tracks[3].clips[0].id == added
                   && moved && s.tracks[1].clips[0].position == 2000,
               "freeze: a frozen track's clips can't be moved, split, muted or cleared; added clips go to a new track and moves onto it stay put");
    }
}

namespace
{
    /** A clip's gain at a session frame, from its line, as a gain. */
    float lineGain (const SessionClip& c, juce::int64 frame)
    {
        return juce::Decibels::decibelsToGain (c.gainEnvelope.valueAt (c.offset + frame - c.position), -150.0f);
    }

    void testClipGainEnvelope()
    {
        // A steady mono clip from 1000, its audio's first 500 frames trimmed off, with a line
        // from 0 dB down to -12 dB and up to +6 dB.
        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        juce::AudioBuffer<float> steady (1, 20000);
        steady.clear();
        juce::FloatVectorOperations::fill (steady.getWritePointer (0), 0.25f, 20000);
        const auto clip = SessionEdits::addClip (s, 0, juce::File ("/takes/steady.wav"), AudioEdits::fromBuffer (std::move (steady)), 1000);
        SessionEdits::trimClip (s, clip, 1500, 1000 + 20000);
        auto plain = s;
        Envelope line;
        line.points = { { 2000, 0.0f }, { 6000, -12.0f }, { 9000, 6.0f } };
        const auto set = SessionEdits::setClipGainEnvelope (s, clip, line);
        const juce::Range<juce::int64> range (0, 22000);
        const auto withLine = mix (s, range), without = mix (plain, range);
        const auto& c = s.tracks[0].clips[0];
        auto worst = 0.0f;

        for (int i = 0; i < 22000; i += 7)
            for (int ch = 0; ch < 2; ++ch)
                worst = juce::jmax (worst, std::abs (withLine.getSample (ch, i) - without.getSample (ch, i) * (i >= 1500 && i < 21000 ? lineGain (c, i) : 1.0f)));

        // The clip plays its audio from frame 500 at session frame 1500: audio frame 6000 at 7000.
        check (set && worst < 1.0e-6f && std::abs (lineGain (c, 7000) - juce::Decibels::decibelsToGain (-12.0f)) < 1.0e-6f
                   && std::abs (lineGain (c, 10000) - juce::Decibels::decibelsToGain (6.0f)) < 1.0e-6f,
               "clip gain line: the mix follows the line at the clip's audio frames, every 7th frame within 0.000001 (" + juce::String (worst, 9) + ")");

        // Moved, trimmed further and split, the line stays with the sound.
        auto moved = s;
        SessionEdits::moveClip (moved, clip, 0, 4000);
        const auto movedMix = mix (moved, { 2500, 24500 });
        auto trimmed = s;
        SessionEdits::trimClip (trimmed, clip, 3000, 21000);
        const auto trimmedMix = mix (trimmed, { 3000, 21000 }), beforeTrim = mix (s, { 3000, 21000 });
        auto split = s;
        SessionEdits::splitClip (split, clip, 7000);
        check (identical (mix (s, range), withLine) && identical (movedMix, mix (s, { 0, 22000 })) && identical (trimmedMix, beforeTrim)
                   && identical (mix (split, range), withLine),
               "clip gain line: moved, trimmed or split, the clip plays the same, bit for bit, where it still plays");

        // Reversed and stretched, the line turns round or scales with the audio.
        auto reversed = s;
        const auto& r0 = reversed.tracks[0].clips[0];
        const auto before = r0;
        SessionEdits::reverseClip (reversed, clip, juce::File ("/takes/steady Reversed.wav"), SessionEdits::reversedAudio (r0));
        const auto& r1 = reversed.tracks[0].clips[0];
        auto reverseWorst = 0.0f;

        for (juce::int64 i = 0; i < before.length; i += 11)
            reverseWorst = juce::jmax (reverseWorst, std::abs (lineGain (r1, r1.position + i) - lineGain (before, before.position + before.length - 1 - i)));

        auto stretched = s;
        juce::AudioBuffer<float> twice (1, 39000);
        twice.clear();
        SessionEdits::stretchClip (stretched, clip, juce::File ("/takes/steady Stretched.wav"), AudioEdits::fromBuffer (std::move (twice)));
        const auto& st = stretched.tracks[0].clips[0];
        const auto& sp = st.gainEnvelope.points;
        check (reverseWorst < 1.0e-6f && r1.offset == 0 && sp.size() == 5 && sp[1].frame == 3000 && sp[2].frame == 11000 && sp[3].frame == 17000
                   && juce::exactlyEqual (sp[2].value, -12.0f) && sp.back().frame == 38999 - 1,
               "clip gain line: reversing turns the line round (within " + juce::String (reverseWorst, 9) + "), stretching scales it");

        // Choosing another take keeps the line on the same moments; a locked clip is refused,
        // and values are kept to the clip gain's range.
        auto takes = s;
        auto& tc = takes.tracks[0].clips[0];
        tc.takes = { { tc.source, tc.audio, 0 }, { tc.source, tc.audio, 300 } };
        const auto chose = SessionEdits::chooseTake (takes, clip, 1);
        const auto shifted = takes.tracks[0].clips[0].gainEnvelope.points[0].frame == 2300;
        auto locked = s;
        SessionEdits::setClipLocked (locked, clip, true);
        const auto refused = ! SessionEdits::setClipGainEnvelope (locked, clip, {});
        auto limited = s;
        SessionEdits::setClipGainEnvelope (limited, clip, { { { 0, -90.0f }, { 10, 40.0f } } });
        const auto& lp = limited.tracks[0].clips[0].gainEnvelope.points;
        check (chose && shifted && refused && juce::exactlyEqual (lp[0].value, SessionClip::minGainDb) && juce::exactlyEqual (lp[1].value, SessionClip::maxGainDb),
               "clip gain line: a take change shifts the line with the take, a locked clip is refused, values are limited");
    }
}

namespace
{
    void testFolderStems()
    {
        // Guitar and the bus it sends to in a folder at -3 dB, Keys outside; an empty folder too.
        auto s = session (0.0f);
        const auto guitar = s.tracks[0].id, keys = s.tracks[1].id, bus = s.tracks[2].id;
        auto& folder = s.addFolder();
        folder.gainDb = -3.0f;
        folder.mute = true;   // a folder's own mute is left out of its stem
        const auto folderId = folder.id;
        const auto emptyId = s.addFolder().id;
        SessionEdits::setTrackFolder (s, guitar, folderId);
        SessionEdits::setTrackFolder (s, bus, folderId);
        s.tracks[1].solo = true;   // so are solos

        const juce::Range<juce::int64> range (0, 12000);
        const auto stem = mix (Bounce::isolateFolder (s, folderId), range);
        auto asHeard = s;
        asHeard.tracks[1].solo = false;
        asHeard.tracks[1].mute = true;
        asHeard.tracks[3].mute = false;
        const auto listed = Bounce::stemFolders (s) == std::vector<juce::uint32> { folderId };

        // A send to a bus outside the folder is left out of its stem, and a muted track in it stays muted.
        auto outside = s;
        SessionEdits::setTrackFolder (outside, bus, 0);
        outside.tracks[0].mute = true;
        const auto alone = Bounce::isolateFolder (outside, folderId);

        check (listed && identical (stem, mix (asHeard, range)) && stem.getMagnitude (0, 12000) > 0.01f,
               "folder stems: a folder's stem is its tracks and buses mixed as set, with its volume, bit for bit, leaving out its own mute, solos and the other tracks; empty folders have none");
        check (alone.tracks.size() == 2 && alone.tracks[0].id == guitar && alone.tracks[0].sends.empty() && alone.tracks[0].mute
                   && alone.findTrack (bus) == nullptr && alone.findTrack (keys) == nullptr && emptyId != 0
                   && juce::exactlyEqual (mix (alone, range).getMagnitude (0, 12000), 0.0f),
               "folder stems: sends to buses outside the folder are left out, and a muted track in it stays silent");
    }
    /** Energy more than 6 Hz from the given partials, against all of it, in dB, over the middle of x. */
    double offNotes (const float* x, int length, const std::vector<double>& partials)
    {
        const auto order = (int) std::floor (std::log2 ((double) length / 2.0));
        const auto n = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> work ((size_t) (2 * n), 0.0f);
        const auto start = (length - n) / 2;

        for (int i = 0; i < n; ++i)
            work[(size_t) i] = x[start + i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) n));

        fft.performFrequencyOnlyForwardTransform (work.data(), true);
        auto all = 0.0, off = 0.0;

        for (int k = 1; k < n / 2; ++k)
        {
            const auto f = k * rate / n, e = (double) work[(size_t) k] * work[(size_t) k];
            all += e;

            if (std::none_of (partials.begin(), partials.end(), [f] (double p) { return std::abs (f - p) <= 6.0; }))
                off += e;
        }

        return 10.0 * std::log10 (off / all + 1.0e-30);
    }

    void testOwnStretch()
    {
        using TimeStretch::Method;
        const auto own = Method::spectrablade;

        // A tone: lengths, pitch, level, and the channels exactly opposite, as they went in.
        const auto tone = whole (*toneFile (96000));
        auto toneRight = true;
        juce::String toneFound;

        for (auto ratio : { 0.5, 0.75, 1.5, 2.0 })
        {
            const auto out = TimeStretch::stretch (tone, ratio, rate, {}, own);
            const auto n = out.getNumSamples();
            const auto f = frequencyOf (out.getReadPointer (0), n / 4, 3 * n / 4, rate);
            const auto rms = out.getRMSLevel (0, n / 4, n / 2);
            const auto first = out.getMagnitude (0, 0, 128), last = out.getMagnitude (0, n - 128, 128);
            auto opposite = true;

            for (int i = 0; i < n && opposite; ++i)
                opposite = juce::exactlyEqual (out.getSample (1, i), -out.getSample (0, i));

            toneRight = toneRight && n == (int) std::llround (96000 * ratio) && std::abs (f / 440.0 - 1.0) < 0.005
                        && std::abs (juce::Decibels::gainToDecibels (rms / (0.5f / std::sqrt (2.0f)))) < 1.0f && opposite && first > 0.45f && last > 0.45f;
            toneFound << juce::String (ratio, 2) << "x: " << juce::String (f, 2) << " Hz, " << juce::String (juce::Decibels::gainToDecibels (rms), 2)
                      << " dBFS, ends " << juce::String (first, 3) << " / " << juce::String (last, 3) << "; ";
        }

        check (toneRight, "own stretch: a 440 Hz tone from 50% to 200% keeps its length, pitch (0.5%), level (1 dB) and edges (over 0.45 of 0.5), "
                          "its channels exactly opposite (" + toneFound.trimEnd() + ")");

        // Clicks: found where they are, and each one comes out once, in place, with nothing before it.
        juce::AudioBuffer<float> clicks (2, 192000);
        clicks.clear();
        std::vector<int> at;

        for (int t = 6000; t + 200 < clicks.getNumSamples(); t += 12000)
        {
            at.push_back (t);

            for (int i = 0; i < 96; ++i)
            {
                const auto v = 0.8f * std::sin (juce::MathConstants<float>::twoPi * 3000.0f * (float) i / (float) rate) * std::exp (-(float) i / 24.0f);
                clicks.setSample (0, i + t, v);
                clicks.setSample (1, i + t, v);
            }
        }

        const auto foundClicks = PhaseVocoder::onsets (clicks, 0, clicks.getNumSamples(), rate);
        auto onsetsRight = foundClicks.size() == at.size();
        auto worstOnset = 0;

        for (size_t k = 0; k < foundClicks.size() && onsetsRight; ++k)
            worstOnset = juce::jmax (worstOnset, std::abs (foundClicks[k] - at[k]));

        onsetsRight = onsetsRight && worstOnset <= 48;

        auto clicksKept = true;
        auto worstBefore = -400.0, worstPeak = 0.0;

        for (auto ratio : { 0.5, 0.75, 1.25, 1.5, 2.0 })
        {
            const auto out = TimeStretch::stretch (clicks, ratio, rate, {}, own);

            for (auto t : at)
            {
                const auto placed = (int) std::lround (t * ratio);
                const auto peakNear = out.getMagnitude (0, placed, 96);
                const auto before = out.getRMSLevel (0, placed - 1200, 1056), after = out.getRMSLevel (0, placed, 96);
                const auto beforeDb = juce::Decibels::gainToDecibels (before, -400.0f) - juce::Decibels::gainToDecibels (after);
                worstBefore = juce::jmax (worstBefore, (double) beforeDb);
                worstPeak = juce::jmax (worstPeak, (double) std::abs (peakNear - clicks.getMagnitude (0, t, 96)));
                clicksKept = clicksKept && std::abs (peakNear - clicks.getMagnitude (0, t, 96)) < 0.01f && beforeDb < -100.0f;
            }
        }

        check (onsetsRight && clicksKept,
               "own stretch: 16 clicks found within 1 ms, and from 50% to 200% each comes out where the stretch puts it at its own peak (within 0.01), "
               "the 25 ms before it silent (" + juce::String ((int) foundClicks.size()) + " found, at most " + juce::String (worstOnset) + " samples out; peaks within "
                   + juce::String (worstPeak, 4) + "; at most " + juce::String (worstBefore, 1) + " dB before)");

        // A busy kit: every 160 ms a kick (a 3 kHz click on a 60 Hz thump dying away over 80 ms) and
        // two ghost hits 15 dB quieter, 60 and 110 ms after it, while the thump still sounds. Each hit
        // is found, and comes out where the stretch puts it as it went in, the 10 ms before it unchanged.
        juce::AudioBuffer<float> kit (2, 192000);
        kit.clear();
        std::vector<int> hits;
        const auto addClick = [&kit] (int t, float gain)
        {
            for (int i = 0; i < 96; ++i)
            {
                const auto v = gain * std::sin (juce::MathConstants<float>::twoPi * 3000.0f * (float) i / (float) rate) * std::exp (-(float) i / 24.0f);
                kit.addSample (0, t + i, v);
                kit.addSample (1, t + i, v);
            }
        };

        for (int t = 4800; t + 14400 < kit.getNumSamples(); t += 7680)
        {
            for (int i = 0; i < 14400; ++i)
            {
                const auto v = 0.4f * std::sin (juce::MathConstants<float>::twoPi * 60.0f * (float) i / (float) rate) * std::exp (-(float) i / 3840.0f);
                kit.addSample (0, t + i, v);
                kit.addSample (1, t + i, v);
            }

            addClick (t, 0.5f);
            addClick (t + 2880, 0.09f);
            addClick (t + 5280, 0.09f);
            hits.insert (hits.end(), { t, t + 2880, t + 5280 });
        }

        const auto foundHits = PhaseVocoder::onsets (kit, 0, kit.getNumSamples(), rate);
        auto hitsFound = 0, worstHit = 0;

        for (auto t : hits)
        {
            auto nearest = std::numeric_limits<int>::max();

            for (auto f : foundHits)
                nearest = juce::jmin (nearest, std::abs (f - t));

            if (nearest <= 48)
                ++hitsFound;

            worstHit = juce::jmax (worstHit, juce::jmin (nearest, 100000));
        }

        auto worstStray = 0.0;

        for (auto ratio : { 0.5, 0.75, 1.5, 2.0 })
        {
            const auto out = TimeStretch::stretch (kit, ratio, rate, {}, own);

            for (auto t : hits)
            {
                // Within a sample of where the stretch puts it (an attack found a sample late lands half one out).
                auto stray = std::numeric_limits<double>::max();

                for (int lag = -1; lag <= 1; ++lag)
                {
                    const auto placed = (int) std::lround (t * ratio) + lag;
                    auto most = 0.0;

                    for (int i = -48; i < 96; ++i)
                        for (int ch = 0; ch < 2; ++ch)
                            most = juce::jmax (most, (double) std::abs (out.getSample (ch, placed + i) - kit.getSample (ch, t + i)));

                    stray = juce::jmin (stray, most);
                }

                worstStray = juce::jmax (worstStray, stray);
            }
        }

        check (hitsFound == (int) hits.size() && (int) foundHits.size() == (int) hits.size() && worstStray < 0.01,
               "own stretch: a busy kit's " + juce::String ((int) hits.size()) + " hits (ghost hits 15 dB down under a dying kick, 50 and 60 ms apart) are all found "
               "within 1 ms, nothing else, and from 50% to 200% each comes out within a sample of where the stretch puts it as it went in, from 1 ms before (within 0.01) ("
                   + juce::String ((int) foundHits.size()) + " found, " + juce::String (hitsFound) + " within 1 ms, worst " + juce::String (worstHit) + " samples; worst difference "
                   + juce::String (worstStray, 4) + ")");

        // Steady noise: no attacks.
        juce::AudioBuffer<float> noise (2, 192000);
        juce::Random noiseRandom (7);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < noise.getNumSamples(); ++i)
                noise.setSample (ch, i, 0.2f * (noiseRandom.nextFloat() - 0.5f));

        const auto noiseOnsets = PhaseVocoder::onsets (noise, 0, noise.getNumSamples(), rate);
        check (noiseOnsets.empty(), "own stretch: no attacks found in 4 s of steady white noise (" + juce::String ((int) noiseOnsets.size()) + " found)");

        // Hats over a bass line: every 125 ms a burst of bright noise dying away over 20 ms, about
        // 20 dB under a steady 70 Hz bass. The bass's swings don't move where each hat is placed, so
        // each comes out where the stretch puts it as it went in.
        juce::AudioBuffer<float> hats (2, 192000);
        std::vector<int> hatTimes;
        juce::Random hatRandom (3);

        for (int i = 0; i < hats.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch)
                hats.setSample (ch, i, 0.25f * std::sin (juce::MathConstants<float>::twoPi * 70.0f * (float) i / (float) rate));

        for (int t = 4800; t + 4800 < hats.getNumSamples(); t += 6000)
        {
            auto bright = 0.0f, last = 0.0f;

            for (int i = 0; i < 4800; ++i)
            {
                const auto white = hatRandom.nextFloat() - 0.5f;
                bright = 0.9f * (bright + white - last);
                last = white;

                for (int ch = 0; ch < 2; ++ch)
                    hats.addSample (ch, t + i, 0.06f * bright * std::exp (-(float) i / 960.0f));
            }

            hatTimes.push_back (t);
        }

        const auto foundHats = PhaseVocoder::onsets (hats, 0, hats.getNumSamples(), rate);
        auto hatsFound = 0, worstHat = 0;

        for (auto t : hatTimes)
        {
            auto nearest = std::numeric_limits<int>::max();

            for (auto f : foundHats)
                nearest = juce::jmin (nearest, std::abs (f - t));

            if (nearest <= 48)
                ++hatsFound;

            worstHat = juce::jmax (worstHat, juce::jmin (nearest, 100000));
        }

        auto worstHatStray = 0.0;

        for (auto ratio : { 0.5, 0.75, 1.5, 2.0 })
        {
            const auto out = TimeStretch::stretch (hats, ratio, rate, {}, own);

            for (auto t : hatTimes)
            {
                // Within 4 samples of where the stretch puts it (a hat found a few samples off its
                // first sample lands that much times the stretch less one away).
                auto stray = std::numeric_limits<double>::max();

                for (int lag = -4; lag <= 4; ++lag)
                {
                    const auto placed = (int) std::lround (t * ratio) + lag;
                    auto most = 0.0;

                    for (int i = -48; i < 96; ++i)
                        for (int ch = 0; ch < 2; ++ch)
                            most = juce::jmax (most, (double) std::abs (out.getSample (ch, placed + i) - hats.getSample (ch, t + i)));

                    stray = juce::jmin (stray, most);
                }

                worstHatStray = juce::jmax (worstHatStray, stray);
            }
        }

        check (hatsFound == (int) hatTimes.size() && (int) foundHats.size() == (int) hatTimes.size() && worstHatStray < 0.01,
               "own stretch: " + juce::String ((int) hatTimes.size()) + " hats about 20 dB under a 70 Hz bass are all found within 1 ms, nothing else, and from 50% to 200% "
               "each comes out within 4 samples of where the stretch puts it as it went in, from 1 ms before (within 0.01) ("
                   + juce::String ((int) foundHats.size()) + " found, " + juce::String (hatsFound) + " within 1 ms, worst " + juce::String (worstHat) + " samples; worst difference "
                   + juce::String (worstHatStray, 4) + ")");

        // A steady chord: no attacks found, and little energy away from its notes.
        const std::vector<double> notes { 220.0, 277.18, 329.63, 440.0 };
        juce::AudioBuffer<float> chord (2, 192000);

        for (int i = 0; i < chord.getNumSamples(); ++i)
        {
            auto v = 0.0;

            for (auto f : notes)
                v += 0.2 * std::sin (juce::MathConstants<double>::twoPi * f * i / rate);

            chord.setSample (0, i, (float) v);
            chord.setSample (1, i, (float) -v);
        }

        auto chordClean = PhaseVocoder::onsets (chord, 0, chord.getNumSamples(), rate).empty();
        juce::String chordFound;

        for (auto ratio : { 0.5, 0.75, 1.25, 1.5, 2.0 })
        {
            const auto out = TimeStretch::stretch (chord, ratio, rate, {}, own);
            const auto off = offNotes (out.getReadPointer (0), out.getNumSamples(), notes);
            chordClean = chordClean && off < -40.0;
            chordFound << juce::String (ratio, 2) << "x: " << juce::String (off, 1) << " dB; ";
        }

        check (chordClean, "own stretch: a steady four-note chord has no attacks, and from 50% to 200% its energy more than 6 Hz from the notes stays under -40 dB ("
                               + chordFound.trimEnd() + " the input " + juce::String (offNotes (chord.getReadPointer (0), chord.getNumSamples(), notes), 1) + " dB)");

        // Progress, stopping, and the time it takes on 10 s of dense stereo.
        std::vector<float> heard;
        const auto all = TimeStretch::stretch (tone, 1.5, rate, [&heard] (float p) { heard.push_back (p); return true; }, own);
        auto calls = 0;
        const auto stopped = TimeStretch::stretch (tone, 1.5, rate, [&calls] (float) { return ++calls < 2; }, own);
        juce::AudioBuffer<float> dense (2, 480000);
        juce::Random random (5);

        for (int i = 0; i < dense.getNumSamples(); ++i)
        {
            const auto v = 0.2f * std::sin (juce::MathConstants<float>::twoPi * 220.0f * (float) i / (float) rate) + 0.1f * (random.nextFloat() - 0.5f);
            dense.setSample (0, i, v);
            dense.setSample (1, i, 0.5f * v + 0.1f * (random.nextFloat() - 0.5f));
        }

        const auto started = juce::Time::getMillisecondCounterHiRes();
        const auto denseOut = TimeStretch::stretch (dense, 1.5, rate, {}, own);
        const auto ms = juce::Time::getMillisecondCounterHiRes() - started;
        check (all.getNumSamples() == 144000 && heard.size() > 1 && std::is_sorted (heard.begin(), heard.end()) && heard.back() < 1.0f
                   && stopped.getNumSamples() == 0 && calls == 2 && denseOut.getNumSamples() == 720000,
               "own stretch: progress rises as it goes, stopping part way gives nothing, and 10 s of dense stereo stretches to 15 s in "
                   + juce::String (ms, 0) + " ms");

        // Shift Pitch with it: the same length, each pitch within 0.5%.
        auto shiftsRight = true;
        juce::String shifted;

        for (auto semitones : { -12.0, -5.0, 7.0, 12.0 })
        {
            const auto out = TimeStretch::shiftPitch (tone, semitones, rate, {}, own);
            const auto n = out.getNumSamples();
            const auto f = frequencyOf (out.getReadPointer (0), n / 4, 3 * n / 4, rate), wanted = 440.0 * std::pow (2.0, semitones / 12.0);
            const auto rms = out.getRMSLevel (0, n / 4, n / 2);
            shiftsRight = shiftsRight && n == tone.getNumSamples() && std::abs (f / wanted - 1.0) < 0.005
                          && std::abs (juce::Decibels::gainToDecibels (rms / (0.5f / std::sqrt (2.0f)))) < 1.0f;
            shifted << juce::String (semitones, 0) << ": " << juce::String (f, 2) << " Hz (" << juce::String (wanted, 2) << "); ";
        }

        check (shiftsRight, "own stretch: Shift Pitch with it keeps the length, each pitch within 0.5% and the level within 1 dB (" + shifted.trimEnd() + ")");

        // As a sequence step: method 1 runs it, the name says so, other methods are refused.
        const auto file = toneFile (144000);
        const Sequences::Step step { "stretch", { { "percent", 150.0 }, { "method", 1.0 } }, nullptr };
        const auto r = Sequences::apply (step, file, 48000, 96000, rate, {}, nullptr);
        const auto expected = TimeStretch::stretchRange (*file, 48000, 96000, 1.5, rate, {}, own);
        const auto shiftStep = Sequences::Step { "pitch", { { "semitones", 3.0 }, { "method", 1.0 } }, nullptr };
        auto same = r.snapshot != nullptr && expected != nullptr && r.snapshot->getNumFrames() == expected->getNumFrames();

        if (same)
        {
            const auto a = whole (*r.snapshot), b = whole (*expected);

            for (int ch = 0; ch < 2 && same; ++ch)
                for (int i = 0; i < a.getNumSamples() && same; ++i)
                    same = juce::exactlyEqual (a.getSample (ch, i), b.getSample (ch, i));
        }

        check (same && r.end == 120000 && step.name() == "Stretch to 150.0% (SpectraBlade Stretch)"
                   && shiftStep.name() == "Shift Pitch +3.00 Semitones (SpectraBlade Stretch)"
                   && Sequences::problem ({ "stretch", { { "percent", 150.0 }, { "method", 2.0 } }, nullptr }, rate).isNotEmpty()
                   && Sequences::problem ({ "stretch", { { "percent", 150.0 } }, nullptr }, rate).isEmpty(),
               "own stretch as a sequence step: method 1 gives just what the stretch gives, named for it; no method is Signalsmith Stretch, other methods are refused");
    }

}

void runBounceTests()
{
    testFolderStems();
    testTimeStretch();
    testStretchInFiles();
    testOwnStretch();
    testFreeze();
    testClipGainEnvelope();

    const juce::Range<juce::int64> range (0, 12000);

    for (auto db : { 0.0f, -6.0f })
    {
        const auto s = session (db);
        const auto guitar = s.tracks[0].id;
        const auto full = mix (s, range);
        auto alone = mix (Bounce::isolate (s, guitar), range);

        auto bounced = s;
        const auto id = Bounce::land (bounced, guitar, juce::File ("/takes/Guitar Bounce.wav"), AudioEdits::fromBuffer (std::move (alone)), 0);
        const auto after = mix (bounced, range);

        if (juce::exactlyEqual (db, 0.0f))
        {
            const auto& t = bounced.tracks;
            check (id != 0 && t.size() == 4 && t[1].id == id && t[1].name == "Guitar Bounce" && t[0].mute && ! t[1].mute
                       && t[1].clips.size() == 1 && t[1].clips[0].position == 0 && t[1].clips[0].length == 12000
                       && t[1].clips[0].source == juce::File ("/takes/Guitar Bounce.wav") && t[1].sends.size() == 1
                       && t[1].sends[0].busId == t[3].id && t[2].name == "Keys" && t[3].isBus,
                   "bounce: lands on a new track after the source, with its sends, and mutes the source");
            check (identical (full, after),
                   "bounce: at 0 dB, the session mixes bit for bit the same after the bounce, through the bus too");
        }
        else
        {
            // The send now scales the bounced audio rather than the track's: a rounding apart.
            const auto most = largestDifference (full, after);
            check (most > 0.0f && most < 1.0e-6f,
                   "bounce: at -6 dB the mix after it is within float rounding of the one before (" + juce::String (most, 10) + ")");
        }
    }

    auto muted = session (0.0f);
    muted.tracks[0].mute = true;
    muted.tracks[1].solo = true;
    const auto alone = Bounce::isolate (muted, muted.tracks[0].id);
    const auto rendered = mix (alone, range);
    check (alone.tracks.size() == 1 && alone.tracks[0].sends.empty() && rendered.getMagnitude (0, 12000) > 0.1f
               && Bounce::isolate (muted, 9999).tracks.empty(),
           "bounce: a muted track bounces as it would play unmuted, alone and without its sends");

    // Stems: the tracks with clips, each alone; without buses they add up to the mix bit for bit.
    {
        SessionState s;
        s.sampleRate = rate;
        s.addTrack().name = "Drums";
        s.addTrack().name = "Empty";
        s.addBus();
        s.addTrack().name = "Bass/DI";
        s.addTrack().name = "Pad";
        s.tracks[0].gainDb = -4.0f;
        s.tracks[3].pan = 0.7f;
        s.tracks[4].mute = true;
        SessionEdits::addClip (s, 0, juce::File ("/takes/drums.wav"), ramp (2, 12000, 0.3f), 0);
        SessionEdits::addClip (s, 3, juce::File ("/takes/bass.wav"), ramp (1, 8000, 0.6f), 2000);
        SessionEdits::addClip (s, 4, juce::File ("/takes/pad.wav"), ramp (2, 6000, 0.1f), 500);

        const auto ids = Bounce::stemTracks (s);
        check (ids == std::vector<juce::uint32> { s.tracks[0].id, s.tracks[3].id, s.tracks[4].id },
               "stems: one for each track with clips, buses and empty tracks left out");

        // A track with only note clips (an instrument track) is a stem too, reaches as far as its
        // last note clip, and keeps them when isolated for a bounce.
        NoteClip notes;
        notes.position = 30000;
        notes.length = 9000;
        notes.notes = { { 60, 0, 9000, 0.8f } };
        const auto noteTrack = s.tracks[1].id;
        SessionEdits::addNoteClip (s, noteTrack, notes);
        const auto withNotes = Bounce::stemTracks (s);
        const auto onlyNotes = Bounce::isolate (s, noteTrack);
        const auto* isolated = onlyNotes.findTrack (noteTrack);
        check (withNotes == std::vector<juce::uint32> { s.tracks[0].id, noteTrack, s.tracks[3].id, s.tracks[4].id }
                   && s.tracks[1].hasClips() && s.tracks[1].clipsEnd() == 39000 && ! s.tracks[2].hasClips() && s.tracks[2].clipsEnd() == 0
                   && s.tracks[0].clipsEnd() == 12000 && isolated != nullptr && isolated->noteClips.size() == 1,
               "stems: a track with only note clips is a stem, reaches to its last note clip, and keeps them isolated for a bounce");
        SessionEdits::removeNoteClip (s, s.tracks[1].noteClips[0].id);
        check (Bounce::stemFileName ("Song", 2, "Bass/DI") == juce::File::createLegalFileName ("Song - 02 Bass/DI") + ".wav"
                   && Bounce::stemFileName ("Song", 2, "Bass/DI").startsWith ("Song - 02 Bass") && ! Bounce::stemFileName ("Song", 2, "Bass/DI").containsChar ('/')
                   && Bounce::stemFileName ("Song", 12, "Pad") == "Song - 12 Pad.wav",
               "stems: files are named after the session and the track, numbered in order (" + Bounce::stemFileName ("Song", 2, "Bass/DI") + ")");

        auto playing = s;
        playing.tracks[4].mute = false;   // stems are of every track as it would play
        const auto full = mix (playing, range);
        juce::AudioBuffer<float> sum (2, (int) range.getLength());
        sum.clear();

        for (auto id : ids)
        {
            const auto stem = mix (Bounce::isolate (s, id), range);

            for (int ch = 0; ch < 2; ++ch)
                sum.addFrom (ch, 0, stem, ch, 0, stem.getNumSamples());
        }

        check (identical (full, sum), "stems: without buses, the stems add up to the mix bit for bit, a muted track's as it would play");
    }

    // A muted clip mixes as if it weren't there, bit for bit, through its track's send too.
    {
        const juce::Range<juce::int64> span (0, 12000);
        auto silenced = session (0.0f);
        const auto guitarClip = silenced.tracks[0].clips[0].id;
        const auto changed = SessionEdits::setClipMuted (silenced, guitarClip, true) && ! SessionEdits::setClipMuted (silenced, guitarClip, true);
        auto removed = session (0.0f);
        SessionEdits::removeClip (removed, removed.tracks[0].clips[0].id);
        const auto unmutedMix = mix (session (0.0f), span);
        const auto mutedMix = mix (silenced, span);
        check (changed && identical (mutedMix, mix (removed, span)) && ! identical (mutedMix, unmutedMix),
               "clip mute: a muted clip mixes as if it were removed, bit for bit, its send included");

        const auto copy = SessionEdits::placeClip (silenced, 1, silenced.tracks[0].clips[0], 20000);
        const auto where = silenced.findClip (copy);
        check (silenced.tracks[(size_t) where->first].clips[(size_t) where->second].muted
                   && identical (mix (Bounce::isolate (silenced, silenced.tracks[0].id), span), mix (Bounce::isolate (removed, removed.tracks[0].id), span)),
               "clip mute: copies stay muted, and a bounce or stem of the track leaves the muted clip out");
    }

    // A duplicated track: right after the source, everything copied with new ids, and alone it
    // plays exactly as the source does.
    {
        const juce::Range<juce::int64> span (0, 12000);
        auto doubled = session (-4.0f);
        doubled.tracks[0].group = 2;
        doubled.tracks[0].colour = 5;
        doubled.tracks[0].volumeEnvelope.points = { { 0, -4.0f }, { 6000, -12.0f } };
        SessionEdits::setClipLocked (doubled, doubled.tracks[0].clips[0].id, true);
        const auto source = doubled.tracks[0];
        const auto copyId = SessionEdits::duplicateTrack (doubled, 0);
        const auto& copy = doubled.tracks[1];
        const auto sameSettings = copy.id == copyId && copy.name == "Guitar Copy" && juce::exactlyEqual (copy.gainDb, source.gainDb)
                                  && juce::exactlyEqual (copy.pan, source.pan) && copy.group == 2 && copy.colour == 5
                                  && copy.volumeEnvelope.points == source.volumeEnvelope.points && copy.sends.size() == 1
                                  && copy.sends[0].busId == source.sends[0].busId && copy.clips.size() == 1
                                  && copy.clips[0].id != source.clips[0].id && copy.clips[0].audio == source.clips[0].audio
                                  && ! copy.clips[0].locked && doubled.tracks[2].name == "Keys";
        check (sameSettings && identical (mix (Bounce::isolate (doubled, copyId), span), mix (Bounce::isolate (doubled, source.id), span))
                   && SessionEdits::duplicateTrack (doubled, 9) == 0,
               "duplicate track: right after the source with its settings, sends, envelope and clips (unlocked, new ids), playing the same bit for bit");
    }

    {
        auto hidden = session (0.0f);

        for (auto& t : hidden.tracks)
            t.hidden = true;

        check (identical (mix (hidden, { 0, 12000 }), mix (session (0.0f), { 0, 12000 })), "hiding: hidden tracks and buses still play, bit for bit");
    }

    // Track templates: what they keep, sends matched to buses by name, and through a file.
    {
        auto from = session (-4.5f);
        from.tracks[0].colour = 4;
        from.tracks[0].note = "DI & amp";
        from.tracks[0].input = { 2, 1 };
        from.tracks[0].group = 3;
        from.tracks[0].mute = true;
        auto effects = std::make_unique<juce::XmlElement> ("EFFECTS");
        effects->createNewChildElement ("SLOT")->setAttribute ("id", 7);
        const auto xml = TrackTemplate::create (from, from.tracks[0].id, std::move (effects));
        const auto file = juce::File::createTempFile (TrackTemplate::extension);
        const auto written = xml != nullptr && xml->writeTo (file);
        const auto read = juce::XmlDocument::parse (file);
        file.deleteFile();

        // Into a session with the bus (renamed to match) and one without.
        SessionState into;
        into.sampleRate = rate;
        into.addTrack().name = "Keys";
        into.addBus().name = from.tracks[2].name;
        auto bare = into;
        bare.tracks.pop_back();
        const auto added = read != nullptr ? TrackTemplate::add (into, *read) : TrackTemplate::Added {};
        const auto withoutBus = read != nullptr ? TrackTemplate::add (bare, *read) : TrackTemplate::Added {};
        const auto& t = into.tracks.back();
        check (written && added.trackId == t.id && t.name == "Guitar" && ! t.isBus && juce::exactlyEqual (t.gainDb, -4.5f)
                   && juce::exactlyEqual (t.pan, -0.3f) && t.colour == 4 && t.note == "DI & amp" && t.input == TrackInput { 2, 1 }
                   && t.group == 0 && ! t.mute && t.clips.empty() && t.sends.size() == 1 && t.sends[0].busId == into.tracks[1].id
                   && juce::exactlyEqual (t.sends[0].gainDb, -10.0f) && added.effects != nullptr && added.effects->getChildByName ("SLOT") != nullptr
                   && added.effects->getChildByName ("SLOT")->getIntAttribute ("id") == 7 && added.missingBuses.isEmpty()
                   && withoutBus.trackId != 0 && bare.tracks.back().sends.empty() && withoutBus.missingBuses == juce::StringArray { from.tracks[2].name }
                   && TrackTemplate::create (from, 999, nullptr) == nullptr && TrackTemplate::add (bare, juce::XmlElement ("SESSION")).trackId == 0,
               "track templates: settings, note, input, sends by bus name and effects through a file, not clips, group or mute; a missing bus named");
    }

    // Track templates keep the envelopes: at the same times, moved to another sample rate.
    {
        auto from = session (0.0f);
        auto& g = from.tracks[0];
        g.volumeEnvelope.points = { { 0, -4.0f }, { 6000, -12.0f }, { 9001, -2.0f } };
        g.panEnvelope.points = { { 500, -0.5f }, { 7000, 0.75f } };
        g.surroundEnvelopes[0].points = { { 1000, -1.0f }, { 3000, 1.0f } };
        g.effectEnvelopes.push_back ({ 7, "gain", { { { 200, 0.25f }, { 4000, 0.8f } } } });
        auto effects = std::make_unique<juce::XmlElement> ("EFFECTS");
        effects->createNewChildElement ("SLOT")->setAttribute ("id", 7);
        const auto xml = TrackTemplate::create (from, g.id, std::move (effects));
        const auto file = juce::File::createTempFile (TrackTemplate::extension);
        const auto written = xml != nullptr && xml->writeTo (file);
        const auto read = juce::XmlDocument::parse (file);
        file.deleteFile();

        SessionState same;
        same.sampleRate = from.sampleRate;
        const auto added = read != nullptr ? TrackTemplate::add (same, *read) : TrackTemplate::Added {};
        const auto* t = same.findTrack (added.trackId);
        const auto kept = written && t != nullptr && t->volumeEnvelope == g.volumeEnvelope && t->panEnvelope == g.panEnvelope
                          && t->surroundEnvelopes == g.surroundEnvelopes && t->effectEnvelopes == g.effectEnvelopes
                          && added.effects != nullptr && added.effects->getChildByName ("SLOT")->getIntAttribute ("id") == (int) t->effectEnvelopes[0].slotId;

        SessionState doubled;
        doubled.sampleRate = 2.0 * from.sampleRate;
        const auto* d = doubled.findTrack (read != nullptr ? TrackTemplate::add (doubled, *read).trackId : 0);
        auto scaled = d != nullptr && d->volumeEnvelope.points.size() == 3 && d->effectEnvelopes.size() == 1;

        for (size_t i = 0; scaled && i < 3; ++i)
            scaled = d->volumeEnvelope.points[i].frame == 2 * g.volumeEnvelope.points[i].frame
                     && juce::exactlyEqual (d->volumeEnvelope.points[i].value, g.volumeEnvelope.points[i].value);

        scaled = scaled && d->effectEnvelopes[0].envelope.points[1].frame == 8000 && d->panEnvelope.points[1].frame == 14000;

        SessionState cd;
        cd.sampleRate = 44100.0;
        const auto* c = cd.findTrack (read != nullptr ? TrackTemplate::add (cd, *read).trackId : 0);
        const auto rounded = c != nullptr && c->volumeEnvelope.points[2].frame == std::llround (9001.0 * 44100.0 / from.sampleRate);

        auto plain = session (0.0f);
        const auto plainXml = TrackTemplate::create (plain, plain.tracks[0].id, nullptr);
        check (kept && scaled && rounded && plainXml != nullptr && plainXml->getChildByName ("ENVELOPE") == nullptr && ! plainXml->hasAttribute ("sampleRate"),
               "track templates: volume, pan, surround pan and effect envelopes come back through a file exactly (the effect's on its "
               "slot as the template's effects load it); at twice the rate every point is at twice the frame, at 44.1 kHz rounded to "
               "the nearest; a track with none writes none");
    }

    {
        // Session-wide time edits: Guitar plays 1000 to 10000, Keys 0 to 12000.
        const auto original = session (0.0f);
        const auto silent = [] (const juce::AudioBuffer<float>& b)
        {
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                if (b.getMagnitude (ch, 0, b.getNumSamples()) > 0.0f)
                    return false;

            return true;
        };

        auto cleared = original;
        const auto didClear = SessionEdits::clearTime (cleared, 2000, 5000);
        check (didClear && identical (mix (cleared, { 0, 2000 }), mix (original, { 0, 2000 })) && silent (mix (cleared, { 2000, 5000 }))
                   && identical (mix (cleared, { 5000, 12000 }), mix (original, { 5000, 12000 }))
                   && ! SessionEdits::clearTime (cleared, 50000, 60000),
               "clear range: every track silent across it and bit for bit the same either side; false where nothing is");

        auto deleted = original;
        const auto didDelete = SessionEdits::deleteTime (deleted, 2000, 5000);
        check (didDelete && identical (mix (deleted, { 0, 2000 }), mix (original, { 0, 2000 }))
                   && identical (mix (deleted, { 2000, 9000 }), mix (original, { 5000, 12000 })) && deleted.getLength() == 9000,
               "delete time: what was after the range plays right after what was before it, bit for bit, on every track");

        auto inserted = original;
        const auto didInsert = SessionEdits::insertTime (inserted, 3000, 2000);
        check (didInsert && identical (mix (inserted, { 0, 3000 }), mix (original, { 0, 3000 })) && silent (mix (inserted, { 3000, 5000 }))
                   && identical (mix (inserted, { 5000, 14000 }), mix (original, { 3000, 12000 })) && inserted.getLength() == 14000
                   && inserted.tracks[0].clips.size() == 2 && inserted.tracks[1].clips.size() == 2,
               "insert time: clips across it split, empty time there, and what was after it plays later, bit for bit");

        // Envelopes, markers and locked clips.
        auto marked = original;
        marked.tracks[0].volumeEnvelope.points = { { 0, -4.0f }, { 6000, -12.0f }, { 9000, -2.0f } };
        marked.tracks[1].panEnvelope.points = { { 4000, 0.5f } };
        marked.markers = { { 1000, 0, "a" }, { 3000, 0, "b" }, { 8000, 0, "c" } };
        SessionEdits::addClip (marked, 1, juce::File ("/takes/pad.wav"), ramp (1, 1000, 0.1f), 13000);
        SessionEdits::addClip (marked, 0, juce::File ("/takes/hit.wav"), ramp (1, 1000, 0.1f), 6000);
        const auto lockedId = marked.tracks[0].clips.back().id;
        SessionEdits::setClipLocked (marked, lockedId, true);
        const auto oldVolume = marked.tracks[0].volumeEnvelope, oldPan = marked.tracks[1].panEnvelope;
        const auto lastStart = [] (const SessionTrack& t)
        {
            juce::int64 last = -1;

            for (auto& c : t.clips)
                last = juce::jmax (last, c.position);

            return last;
        };
        const auto lockedAt = [] (const SessionState& st, juce::uint32 id)
        {
            const auto where = st.findClip (id);
            return where.has_value() ? st.tracks[(size_t) where->first].clips[(size_t) where->second].position : (juce::int64) -1;
        };

        auto cut = marked;
        SessionEdits::deleteTime (cut, 2000, 5000);
        auto shapeKept = true;

        for (juce::int64 f = 0; f < 12000; f += 250)
            shapeKept = shapeKept && std::abs (cut.tracks[0].volumeEnvelope.valueAt (f) - oldVolume.valueAt (f < 2000 ? f : f + 3000)) < 1.0e-4f
                        && std::abs (cut.tracks[1].panEnvelope.valueAt (f) - oldPan.valueAt (f < 2000 ? f : f + 3000)) < 1.0e-6f;

        check (shapeKept && cut.markers.size() == 2 && cut.markers[0].start == 1000 && cut.markers[1].start == 5000 && cut.markers[1].name == "c"
                   && lockedAt (cut, lockedId) == 6000 && lastStart (cut.tracks[1]) == 10000,
               "delete time: envelopes keep their shape either side, markers inside go and later ones move back, a locked clip stays put");

        auto opened = marked;
        SessionEdits::insertTime (opened, 3000, 2000);
        auto held = true;

        for (juce::int64 f = 0; f < 14000; f += 250)
            held = held && std::abs (opened.tracks[0].volumeEnvelope.valueAt (f) - oldVolume.valueAt (f < 3000 ? f : f < 5000 ? 3000 : f - 2000)) < 1.0e-4f
                   && std::abs (opened.tracks[1].panEnvelope.valueAt (f) - oldPan.valueAt (f < 3000 ? f : f < 5000 ? 3000 : f - 2000)) < 1.0e-6f;

        check (held && opened.markers.size() == 3 && opened.markers[0].start == 1000 && opened.markers[2].start == 10000
                   && lockedAt (opened, lockedId) == 6000 && lastStart (opened.tracks[1]) == 15000
                   && ! SessionEdits::insertTime (opened, 3000, 0) && ! SessionEdits::deleteTime (opened, 4000, 4000),
               "insert time: envelopes hold their value across it and keep their shape after, later markers move on, a locked clip stays put");
    }

    {
        // Copying and pasting time: Guitar plays 1000 to 10000, Keys 0 to 12000.
        const auto original = session (0.0f);
        const auto copied = SessionEdits::copyTime (original, 2000, 5000);
        auto pasted = original;
        const auto didPaste = SessionEdits::pasteTime (pasted, copied, 8000);
        check (didPaste && copied.length == 3000 && copied.tracks.size() == 3 && pasted.getLength() == 15000
                   && identical (mix (pasted, { 0, 8000 }), mix (original, { 0, 8000 }))
                   && identical (mix (pasted, { 8000, 11000 }), mix (original, { 2000, 5000 }))
                   && identical (mix (pasted, { 11000, 15000 }), mix (original, { 8000, 12000 })),
               "paste time: the copied time plays at the cursor, bit for bit on every track, with what was after it moved on");

        auto marked = original;
        marked.tracks[0].volumeEnvelope.points = { { 0, -4.0f }, { 6000, -12.0f }, { 9000, -2.0f } };
        marked.markers = { { 1000, 0, "a" }, { 3000, 0, "b" }, { 9000, 0, "c" } };
        SessionEdits::setClipLocked (marked, marked.tracks[1].clips[0].id, true);
        const auto old = marked.tracks[0].volumeEnvelope;
        const auto markedCopy = SessionEdits::copyTime (marked, 2000, 5000);
        auto onto = marked;
        SessionEdits::pasteTime (onto, markedCopy, 8000);
        auto shaped = true;

        for (juce::int64 f = 0; f < 15000; f += 250)
            shaped = shaped && std::abs (onto.tracks[0].volumeEnvelope.valueAt (f) - old.valueAt (f < 8000 ? f : f < 11000 ? f - 6000 : f - 3000)) < 1.0e-4f;

        auto plain = original;
        SessionEdits::pasteTime (plain, markedCopy, 8000);
        const auto& given = plain.tracks[0].volumeEnvelope;
        const auto namedB = std::count_if (onto.markers.begin(), onto.markers.end(), [] (auto& m) { return m.name == "b"; });
        const auto pastedB = std::any_of (onto.markers.begin(), onto.markers.end(), [] (auto& m) { return m.name == "b" && m.start == 9000; });
        check (shaped && ! markedCopy.tracks[1].clips.empty() && ! markedCopy.tracks[1].clips[0].locked
                   && std::abs (given.valueAt (4000)) < 1.0e-6f && std::abs (given.valueAt (8500) - old.valueAt (2500)) < 1.0e-4f
                   && std::abs (given.valueAt (13000)) < 1.0e-6f && plain.tracks[1].volumeEnvelope.isEmpty()
                   && onto.markers.size() == 4 && namedB == 2 && pastedB,
               "paste time: envelopes keep their shape around the pasted line, a track without one gets one at its setting outside it, markers come too, copies unlocked");

        SessionState other;
        other.sampleRate = 44100.0;
        other.addTrack();
        SessionState single;
        single.sampleRate = rate;
        single.addTrack();
        const auto onOne = SessionEdits::pasteTime (single, copied, 0);
        check (! SessionEdits::pasteTime (other, copied, 0) && ! SessionEdits::pasteTime (pasted, SessionEdits::copyTime (original, 3000, 3000), 0)
                   && onOne && single.tracks.size() == 1 && single.tracks[0].clips.size() == 1 && single.getLength() == 3000,
               "paste time: refused at another rate or with nothing copied; tracks past the last are left out");
    }

    {
        // Export Regions: each region in time order, numbered among the regions, points left out.
        auto regioned = session (0.0f);
        regioned.markers = { { 0, 0, "Start" }, { 1000, 4000, "Verse/A" }, { 3000, 0, "Hit" }, { 6000, 3000, "Chorus" } };
        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory);
        const auto files = Bounce::regionFiles (regioned, "Song", folder);
        check (files.size() == 2 && files[0].first == juce::Range<juce::int64> (1000, 5000) && files[0].second == folder.getChildFile (Bounce::stemFileName ("Song", 1, "Verse/A"))
                   && files[1].first == juce::Range<juce::int64> (6000, 9000) && files[1].second.getFileName() == "Song - 02 Chorus.wav"
                   && Bounce::regionFiles (session (0.0f), "Song", folder).empty(),
               "export regions: a file for each region in time order, numbered and named as stems are; point markers left out");

        const auto fileRegions = Bounce::regionFiles (regioned.markers, "Take", folder);
        check (fileRegions.size() == 2 && fileRegions[0].first == files[0].first && fileRegions[1].first == files[1].first
                   && fileRegions[1].second.getFileName() == "Take - 02 Chorus.wav",
               "save each region: an open file's regions named and numbered the same way");

        // Fitting regions to clips (0 to 12,000 across both tracks): one running past them is cut to
        // their end; one past every clip and one already fitting are left.
        auto fitting = session (0.0f);
        fitting.markers = { { 500, 19500, "Loose" }, { 15000, 1000, "Empty" }, { 0, 12000, "Fits" }, { 300, 0, "Point" } };
        const auto fitCount = SessionEdits::fitRegionsToClips (fitting);
        check (fitCount == 1 && fitting.markers == Markers { { 0, 12000, "Fits" }, { 300, 0, "Point" }, { 500, 11500, "Loose" }, { 15000, 1000, "Empty" } },
               "fit regions to clips: a region cut to the clips inside it; one with no clips, one already fitting and a point left alone");

        // Regions over clips: two overlapping clips and one touching them make one group, a
        // clip after a gap another; doing it again adds none.
        SessionState grouped;
        grouped.sampleRate = rate;
        grouped.addTrack();
        grouped.addTrack();
        SessionEdits::addClip (grouped, 0, juce::File ("/takes/a.wav"), ramp (1, 1000, 0.5f), 0);
        SessionEdits::addClip (grouped, 1, juce::File ("/takes/b.wav"), ramp (1, 1000, 0.5f), 500);
        SessionEdits::addClip (grouped, 0, juce::File ("/takes/c.wav"), ramp (1, 500, 0.5f), 1500);
        SessionEdits::addClip (grouped, 1, juce::File ("/takes/d.wav"), ramp (1, 1000, 0.5f), 5000);
        const auto groupCount = SessionEdits::regionsOverClips (grouped);
        check (groupCount == 2 && grouped.markers == Markers { { 0, 2000, "Region 01" }, { 5000, 1000, "Region 02" } }
                   && SessionEdits::regionsOverClips (grouped) == 0,
               "regions over clips: overlapping and touching clips on any track make one region, a gap starts another; none twice");
    }

    {
        // Fade shapes: their levels half way, and the mix following each one.
        const auto half = [] (FadeShape shape) { return FadeShapes::level (shape, 500, 1000); };
        auto ends = true;

        for (auto shape : FadeShapes::all)
            ends = ends && juce::exactlyEqual (FadeShapes::level (shape, 0, 1000), 0.0f) && juce::exactlyEqual (FadeShapes::level (shape, 1000, 1000), 1.0f)
                   && FadeShapes::fromId (FadeShapes::id (shape)) == shape;

        auto power = true;

        for (juce::int64 f = 0; f <= 1000; f += 10)
            power = power && std::abs (std::pow (FadeShapes::level (FadeShape::equalPower, f, 1000), 2.0f)
                                       + std::pow (FadeShapes::level (FadeShape::equalPower, 1000 - f, 1000), 2.0f) - 1.0f) < 1.0e-6f;

        check (ends && power && std::abs (half (FadeShape::sCurve) - 0.5f) < 1.0e-6f && std::abs (half (FadeShape::linear) - 0.5f) < 1.0e-6f
                   && std::abs (half (FadeShape::equalPower) - std::sqrt (0.5f)) < 1.0e-6f && std::abs (half (FadeShape::slow) - 0.25f) < 1.0e-6f
                   && std::abs (half (FadeShape::fast) - 0.75f) < 1.0e-6f && FadeShapes::fromId ("unknown") == FadeShape::sCurve,
               "fade shapes: from 0 to 1, half way at 0.5, 0.5, 0.707, 0.25 and 0.75; equal-power fades keep the power at 1 across a crossfade");

        juce::AudioBuffer<float> steady (1, 4000);

        for (int i = 0; i < steady.getNumSamples(); ++i)
            steady.setSample (0, i, 0.5f);

        auto followed = true;

        for (auto shape : FadeShapes::all)
        {
            SessionState faded;
            faded.sampleRate = rate;
            faded.addTrack();
            const auto id = SessionEdits::addClip (faded, 0, juce::File ("/takes/steady.wav"), AudioEdits::fromBuffer (juce::AudioBuffer<float> (steady)), 0);
            SessionEdits::setClipFades (faded, id, 1000, 1000);
            SessionEdits::setClipFadeShape (faded, id, false, shape);
            SessionEdits::setClipFadeShape (faded, id, true, shape);
            const auto out = mix (faded, { 0, 4000 });
            const auto full = out.getSample (0, 2000);

            for (int i = 0; i < 1000; i += 7)
                followed = followed && std::abs (out.getSample (0, i) - full * FadeShapes::level (shape, i, 1000)) < 1.0e-6f
                           && std::abs (out.getSample (0, 3999 - i) - full * FadeShapes::level (shape, i, 1000)) < 1.0e-6f;
        }

        SessionState fixedClip;
        fixedClip.sampleRate = rate;
        fixedClip.addTrack();
        const auto fixedId = SessionEdits::addClip (fixedClip, 0, juce::File ("/takes/steady.wav"), AudioEdits::fromBuffer (juce::AudioBuffer<float> (steady)), 0);
        const auto same = ! SessionEdits::setClipFadeShape (fixedClip, fixedId, false, FadeShape::sCurve);
        SessionEdits::setClipLocked (fixedClip, fixedId, true);
        check (followed && same && ! SessionEdits::setClipFadeShape (fixedClip, fixedId, true, FadeShape::linear),
               "fade shapes: the mix follows each shape in and out; none set on a locked clip or where it is already so");
    }

    auto gone = session (0.0f);
    check (Bounce::land (gone, 9999, {}, nullptr, 0) == 0 && gone.tracks.size() == 3,
           "bounce: nothing lands when the source track is gone");
}
