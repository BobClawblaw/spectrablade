#include <JuceHeader.h>
#include "../Source/AudioEdits.h"
#include "../Source/Bounce.h"
#include "../Source/OfflineRender.h"
#include "../Source/TimeStretch.h"
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
                inStep = juce::exactlyEqual (out.getSample (1, i), -out.getSample (0, i));
        }

        check (lengthsRight && pitchKept && levelKept && inStep,
               "time stretch: lengths from 50% to 200%, a 440 Hz tone's pitch within 0.5% and level within 1 dB, channels cut in step ("
                   + found.trimEnd() + " input " + juce::String (inputFrequency, 2) + " Hz)");

        const auto started = juce::Time::getMillisecondCounterHiRes();
        juce::AudioBuffer<float> long10 (2, 480000);
        long10.clear();
        long10.copyFrom (0, 0, tone, 0, 0, length);
        const auto stretched = TimeStretch::stretch (long10, 1.5, rate);
        const auto ms = juce::Time::getMillisecondCounterHiRes() - started;
        check (stretched.getNumSamples() == 720000, "time stretch: 10 s of stereo to 15 s in " + juce::String (ms, 0) + " ms");

        // On a clip: the part it plays, its fades scaled, and not when locked.
        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        const auto id = SessionEdits::addClip (s, 0, juce::File ("/takes/tone.wav"), AudioEdits::fromBuffer (juce::AudioBuffer<float> (tone)), 5000);
        SessionEdits::trimClip (s, id, 5000 + 1000, 5000 + 49000);   // plays 48000 frames
        SessionEdits::setClipFades (s, id, 4800, 2400);
        const auto audio = SessionEdits::stretchedAudio (s.tracks[0].clips[0], 1.5, rate);
        auto locked = s;
        SessionEdits::setClipLocked (locked, id, true);
        const auto refused = ! SessionEdits::stretchClip (locked, id, juce::File ("/takes/tone Stretched.wav"), audio);
        const auto applied = SessionEdits::stretchClip (s, id, juce::File ("/takes/tone Stretched.wav"), audio);
        const auto& c = s.tracks[0].clips[0];
        check (refused && applied && audio->getNumFrames() == 72000 && c.position == 6000 && c.offset == 0 && c.length == 72000
                   && c.fadeIn == 7200 && c.fadeOut == 3600 && c.audio == audio && c.source.getFileName() == "tone Stretched.wav",
               "time stretch: a clip plays its stretched part from where it starts, fades scaled with it, and a locked one is refused");
    }
}

void runBounceTests()
{
    testTimeStretch();

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

    auto gone = session (0.0f);
    check (Bounce::land (gone, 9999, {}, nullptr, 0) == 0 && gone.tracks.size() == 3,
           "bounce: nothing lands when the source track is gone");
}
