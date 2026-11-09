#include <JuceHeader.h>
#include "../Source/AudioFileIO.h"
#include "../Source/SessionMixer.h"
#include "TestUtils.h"
#include "DuckingTests.h"

namespace
{
    using TestUtils::check;
    constexpr double rate = 48000.0;
    constexpr int length = (int) (3.5 * rate);

    AudioDocument::Snapshot audio (int frames, const std::function<float (int)>& sample)
    {
        juce::AudioBuffer<float> b (2, frames);

        for (int i = 0; i < frames; ++i)
            b.setSample (0, i, sample (i)), b.setSample (1, i, sample (i));

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, frames } };
        return std::make_shared<const AudioSnapshot> (2, std::move (segments));
    }

    void addClip (SessionState& s, SessionTrack& t, AudioDocument::Snapshot a, juce::int64 at)
    {
        SessionClip c;
        c.id = s.nextId++;
        c.audio = std::move (a);
        c.position = at;
        c.length = c.audio->getNumFrames();
        t.clips.push_back (c);
    }

    /** Music (a steady 0.25 on both channels, so its gain can be read off each sample) ducking
        under a voice listed after it: a 1 kHz tone from 0.5 s to 1 s. */
    SessionState session()
    {
        SessionState s;
        s.sampleRate = rate;
        auto& music = s.addTrack();
        addClip (s, music, audio (length, [] (int) { return 0.25f; }), 0);
        auto& voice = s.addTrack();
        addClip (s, voice, audio ((int) (0.5 * rate), [] (int i) { return 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / rate); }),
                 (juce::int64) (0.5 * rate));
        SessionEdits::setDucking (s, s.tracks[0].id, { s.tracks[1].id, -12.0f, -40.0f, 10.0f, 300.0f });
        return s;
    }

    juce::AudioBuffer<float> play (const SessionState& s, int block = 512)
    {
        juce::AudioBuffer<float> out (2, length);
        SessionMixer::Buffers buffers;
        buffers.reserve (block);

        for (int p = 0; p < length; p += block)
            SessionMixer::render (s, p, out, p, juce::jmin (block, length - p), buffers);

        return out;
    }

    /** The mix less the voice alone: the music as it plays. */
    juce::AudioBuffer<float> musicIn (const SessionState& s, int block = 512)
    {
        auto voiceOnly = s;
        voiceOnly.tracks[0].clips.clear();
        auto mix = play (s, block);
        const auto voice = play (voiceOnly, block);

        for (int ch = 0; ch < 2; ++ch)
            mix.addFrom (ch, 0, voice, ch, 0, length, -1.0f);

        return mix;
    }

    double gainDbAt (const juce::AudioBuffer<float>& music, double seconds)
    {
        return juce::Decibels::gainToDecibels ((double) music.getSample (0, (int) (seconds * rate)) / 0.25, -200.0);
    }

    juce::String r (double v, int places = 2)   { return juce::String (v, places); }

    void testMix()
    {
        const auto s = session();
        const auto music = musicIn (s);
        bool untouched = true;

        for (int i = 0; i < (int) (0.5 * rate); ++i)
            untouched = untouched && juce::exactlyEqual (music.getSample (0, i), 0.25f) && juce::exactlyEqual (music.getSample (1, i), 0.25f);

        const auto attack = gainDbAt (music, 0.5 + 0.010);
        const auto held = gainDbAt (music, 0.75), late = gainDbAt (music, 0.999);
        const auto back = gainDbAt (music, 3.4);
        double during = 0.0;

        for (double t = 0.6; t < 1.0; t += 0.01)
            during = juce::jmax (during, std::abs (gainDbAt (music, t) + 12.0));

        // After the voice stops, its held level falls below -40 dB about 78 ms later (20 ms
        // holding from 0.5), and the release then takes 300 ms to bring the music up 63% of the way.
        const auto released = gainDbAt (music, 1.0 + 0.078 + 0.300);
        check (untouched && std::abs (attack + 12.0 * (1.0 - std::exp (-1.0))) < 0.3 && during < 0.01 && std::abs (released + 12.0 * std::exp (-1.0)) < 0.5
                   && std::abs (back) < 0.01,
               "ducking: the music is untouched bit for bit before the voice, " + r (attack) + " dB 10 ms into it (attack: -7.59 expected), "
                   + r (held) + " and " + r (late) + " dB while it talks (within " + r (during, 4) + " dB of -12), " + r (released)
                   + " dB 300 ms into the release (-4.41 expected), and " + r (back, 4) + " dB by 3.4 s");

        bool sameBlocks = true;
        const auto other = musicIn (s, 37);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < length; ++i)
                sameBlocks = sameBlocks && juce::exactlyEqual (other.getSample (ch, i), music.getSample (ch, i));

        auto mutedVoice = s;
        mutedVoice.tracks[1].mute = true;
        const auto notDucked = play (mutedVoice);
        bool steady = true;

        for (int i = 0; i < length; ++i)
            steady = steady && juce::exactlyEqual (notDucked.getSample (0, i), 0.25f);

        check (sameBlocks && steady,
               "ducking: the same bit for bit in 37-frame blocks as in 512 (the voice is mixed first though listed after); a muted voice ducks nothing");
    }

    void testSends()
    {
        // The music sends to a bus before its volume, then after: the bus's share is what the
        // mix gains from the send.
        const auto busShare = [] (bool preFader)
        {
            auto s = session();
            auto& bus = s.addBus();
            const auto busId = bus.id;
            auto noSend = s;
            SessionEdits::setSend (s, s.tracks[0].id, { busId, 0.0f, preFader });
            auto withSend = play (s);
            const auto without = play (noSend);

            for (int ch = 0; ch < 2; ++ch)
                withSend.addFrom (ch, 0, without, ch, 0, length, -1.0f);

            return withSend;
        };

        const auto pre = busShare (true), post = busShare (false);
        const auto preDb = juce::Decibels::gainToDecibels ((double) pre.getSample (0, (int) (0.8 * rate)) / 0.25);
        const auto postDb = juce::Decibels::gainToDecibels ((double) post.getSample (0, (int) (0.8 * rate)) / 0.25);
        check (std::abs (preDb) < 0.001 && std::abs (postDb + 12.0) < 0.01,
               "ducking: it acts with the volume: a pre-fader send stays at " + r (preDb, 3) + " dB while the voice talks, a post-fader send dips to " + r (postDb));
    }

    void testEdits()
    {
        auto s = session();
        const auto music = s.tracks[0].id, voice = s.tracks[1].id;
        auto& bus = s.addBus();
        const auto busId = bus.id;
        const Ducking under { music, -12.0f, -40.0f, 10.0f, 300.0f };
        auto tooDeep = under;
        tooDeep.amountDb = -70.0f;
        auto tooSlow = under;
        tooSlow.releaseMs = 6000.0f;
        const auto refused = ! SessionEdits::setDucking (s, voice, under)                              // a loop
                             && ! SessionEdits::setDucking (s, music, { music, -12.0f, -40.0f, 10.0f, 300.0f })   // itself
                             && ! SessionEdits::setDucking (s, music, { busId, -12.0f, -40.0f, 10.0f, 300.0f })   // a bus
                             && ! SessionEdits::setDucking (s, busId, { voice, -12.0f, -40.0f, 10.0f, 300.0f })   // on a bus
                             && ! SessionEdits::setDucking (s, music, s.tracks[0].ducking);                       // already so
        auto third = s;
        auto& extra = third.addTrack();
        const auto extraId = extra.id;
        const auto chain = SessionEdits::setDucking (third, voice, { extraId, -6.0f, -30.0f, 5.0f, 100.0f })
                           && ! SessionEdits::setDucking (third, extraId, { music, -6.0f, -30.0f, 5.0f, 100.0f });   // a longer loop
        auto bad = s;
        const auto outOfRange = ! SessionEdits::setDucking (bad, voice, { music, tooDeep.amountDb, -40.0f, 10.0f, 300.0f })
                                && ! SessionEdits::setDucking (bad, voice, { music, -12.0f, -40.0f, 10.0f, tooSlow.releaseMs });
        auto removed = s;
        SessionEdits::removeTrack (removed, 1);
        const auto frozen = SessionEdits::frozenSource (s, music);
        check (refused && chain && outOfRange && ! removed.tracks[0].ducking.isOn() && frozen.tracks.size() == 1 && ! frozen.tracks[0].ducking.isOn(),
               "ducking: refused for itself, a bus as key or on a bus, a loop however long, and settings out of range; deleting the key "
               "turns it off; freezing renders without it");

        // Session files: the key listed after the track that ducks.
        auto dir = juce::File::createTempFile ("ducking");
        dir.createDirectory();
        juce::AudioFormatManager formats;
        AudioFileIO::registerFormats (formats);
        auto noClips = session();
        SessionEdits::setDucking (noClips, noClips.tracks[0].id, { noClips.tracks[1].id, -18.0f, -30.0f, 25.0f, 800.0f });

        for (auto& t : noClips.tracks)
            t.clips.clear();

        const auto target = dir.getChildFile ("ducking.sbsession");
        const auto saved = SessionFile::save (noClips, target).isEmpty();
        const auto loaded = SessionFile::load (target, formats).state;
        const auto& d = loaded.tracks[0].ducking;
        check (saved && d.keyTrackId == loaded.tracks[1].id && juce::exactlyEqual (d.amountDb, -18.0f) && juce::exactlyEqual (d.thresholdDb, -30.0f)
                   && juce::exactlyEqual (d.attackMs, 25.0f) && juce::exactlyEqual (d.releaseMs, 800.0f) && ! loaded.tracks[1].ducking.isOn(),
               "ducking: kept in session files, the key found though it comes later in the list");
        dir.deleteRecursively();
    }
}

void runDuckingTests()
{
    testMix();
    testSends();
    testEdits();
}
