#include <JuceHeader.h>
#include "../Source/AudioEdits.h"
#include "../Source/Bounce.h"
#include "../Source/OfflineRender.h"
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
                inverse = juce::exactlyEqual (out.getSample (1, i), -out.getSample (0, i));

            shiftsRight = shiftsRight && n == length && std::abs (f / wanted - 1.0) < 0.005 && inverse
                          && std::abs (juce::Decibels::gainToDecibels (rms / (0.5f / std::sqrt (2.0f)))) < 1.0f;
            shifted << juce::String (semitones, 0) << ": " << juce::String (f, 2) << " Hz (" << juce::String (wanted, 2) << "), "
                    << juce::String (juce::Decibels::gainToDecibels (rms), 2) << " dBFS; ";
        }

        auto shiftCalls = 0;
        check (shiftsRight && TimeStretch::shiftPitch (tone, 5.0, rate, [&shiftCalls] (float) { return ++shiftCalls < 2; }).getNumSamples() == 0,
               "pitch shift: the same length, a 440 Hz tone within 0.5% of each shifted pitch and within 1 dB, channels in step, and cancelling ("
                   + shifted.trimEnd() + ")");

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

void runBounceTests()
{
    testTimeStretch();
    testFreeze();

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
