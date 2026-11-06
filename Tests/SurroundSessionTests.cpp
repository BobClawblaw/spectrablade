// Mixes and gains are compared bit for bit where the arithmetic is the same.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/Bounce.h"
#include "../Source/MixerView.h"
#include "../Source/MultitrackView.h"
#include "../Source/OfflineRender.h"
#include "../Source/SessionMixer.h"
#include "../Source/Surround.h"
#include "../Source/SurroundPad.h"
#include "../Source/TrackTemplate.h"
#include "TestUtils.h"
#include "SurroundSessionTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;
    constexpr int frames = 4000;

    AudioDocument::Snapshot noise (int channels, int length, int seed)
    {
        juce::Random r (seed);
        juce::AudioBuffer<float> b (channels, length);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < length; ++i)
                b.setSample (ch, i, r.nextFloat() * 0.5f - 0.25f);

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, length } };
        return std::make_shared<const AudioSnapshot> (channels, std::move (segments));
    }

    SessionTrack& addClipTrack (SessionState& s, int channels, juce::int64 position, int seed)
    {
        auto& t = s.addTrack();
        SessionClip c;
        c.id = s.nextId++;
        c.audio = noise (channels, 3000, seed);
        c.position = position;
        c.length = c.audio->getNumFrames();
        c.fadeIn = 40;
        c.fadeOut = 200;
        t.clips.push_back (c);
        return t;
    }

    /** A stereo track, a mono one panned left, and a bus the stereo track sends to. */
    SessionState mixed()
    {
        SessionState s;
        s.sampleRate = rate;
        auto& a = addClipTrack (s, 2, 100, 21);
        a.gainDb = -1.5f;
        auto& b = addClipTrack (s, 1, 700, 22);
        b.pan = -0.4f;
        b.gainDb = -3.0f;
        auto& x = s.addBus();
        x.gainDb = -2.0f;
        x.pan = 0.2f;
        SessionEdits::setSend (s, s.tracks[0].id, { s.tracks[2].id, -6.0f, false });
        return s;
    }

    juce::AudioBuffer<float> render (const SessionState& s, int channels, int surroundChannel, int interfaceChannel = 2, int blockSize = 512,
                                     SessionMixer::Inserts* inserts = nullptr)
    {
        juce::AudioBuffer<float> out (channels, frames);
        SessionMixer::Buffers buffers;
        buffers.reserve (blockSize);
        buffers.surroundChannel = surroundChannel;
        buffers.interfaceChannel = interfaceChannel;

        if (inserts != nullptr)
            SessionMixer::prime (s, 0, buffers, *inserts);

        for (int p = 0; p < frames; p += blockSize)
            SessionMixer::render (s, p, out, p, juce::jmin (blockSize, frames - p), buffers, inserts);

        return out;
    }

    /** Effects on every track and bus that delay by `latency` frames (and halve, to show they ran),
        noting which tracks they processed. */
    struct Delays final : SessionMixer::Inserts
    {
        int latency = 64;
        std::set<juce::uint32> processed;
        std::map<juce::uint32, juce::AudioBuffer<float>> rings;
        std::map<juce::uint32, int> writes;

        bool isActive (juce::uint32) override                       { return true; }
        int getLatency (juce::uint32) override                      { return latency; }
        int getLatencyBefore (juce::uint32, juce::uint32) override  { return -1; }

        void reset (juce::uint32 id) override
        {
            rings[id].setSize (2, latency);
            rings[id].clear();
            writes[id] = 0;
        }

        void process (const SessionTrack& t, juce::int64, juce::AudioBuffer<float>& b, int n, const std::vector<SessionMixer::KeyAudio>&) override
        {
            processed.insert (t.id);
            auto& ring = rings[t.id];

            if (ring.getNumSamples() != latency)
                reset (t.id);

            auto& w = writes[t.id];

            for (int i = 0; i < n; ++i)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    const auto in = b.getSample (ch, i);
                    b.setSample (ch, i, 0.5f * ring.getSample (ch, w));
                    ring.setSample (ch, w, in);
                }

                w = (w + 1) % latency;
            }
        }
    };

    /** A 5.1 clip of six different signals, with no fades. */
    SessionTrack& addFiveOneTrack (SessionState& s, int channels, juce::int64 position, int seed)
    {
        auto& t = addClipTrack (s, channels, position, seed);
        t.clips[0].fadeIn = t.clips[0].fadeOut = 0;
        t.fiveOne = true;
        return t;
    }

    /** A clip's audio placed at its position in a buffer of frames, each channel where a 5.1
        track plays it (mono to C), times gain, held back by delay. */
    juce::AudioBuffer<float> placed (const SessionClip& clip, float gain, int delay = 0)
    {
        juce::AudioBuffer<float> raw (clip.audio->getNumChannels(), (int) clip.length), out (6, frames);
        clip.audio->read (raw, 0, 0, raw.getNumSamples());
        out.clear();

        for (int ch = 0; ch < raw.getNumChannels() && ch < 6; ++ch)
            for (int i = 0; i < raw.getNumSamples() && (int) clip.position + delay + i < frames; ++i)
                out.setSample (raw.getNumChannels() == 1 ? 2 : ch, (int) clip.position + delay + i, raw.getSample (ch, i) * gain);

        return out;
    }

    bool same (const juce::AudioBuffer<float>& a, int chA, const juce::AudioBuffer<float>& b, int chB)
    {
        return a.getNumSamples() == b.getNumSamples()
            && std::memcmp (a.getReadPointer (chA), b.getReadPointer (chB), sizeof (float) * (size_t) a.getNumSamples()) == 0;
    }

    bool silent (const juce::AudioBuffer<float>& b, int ch)
    {
        return b.getMagnitude (ch, 0, b.getNumSamples()) == 0.0f;
    }

    float worst (const juce::AudioBuffer<float>& a, int chA, const juce::AudioBuffer<float>& b, int chB)
    {
        float most = 0.0f;

        for (int i = 0; i < a.getNumSamples(); ++i)
            most = juce::jmax (most, std::abs (a.getSample (chA, i) - b.getSample (chB, i)));

        return most;
    }

    void testGains()
    {
        using SessionMixer::surroundGains;
        const auto front = surroundGains ({});
        auto onlyLR = front[0][0] == 1.0f && front[1][1] == 1.0f;

        for (int ch = 0; ch < 6; ++ch)
            onlyLR = onlyLR && (ch == 0 || front[0][(size_t) ch] == 0.0f) && (ch == 1 || front[1][(size_t) ch] == 0.0f);

        check (onlyLR, "surround pan: by default a track's left goes to L and its right to R at exactly 1, nothing else");

        SurroundPan mono;
        mono.width = 0.0f;
        const auto centre = surroundGains (mono);
        auto onlyC = std::abs (centre[0][2] - 0.70710678f) < 1.0e-6f && centre[1][2] == centre[0][2];

        for (int ch = 0; ch < 6; ++ch)
            onlyC = onlyC && (ch == 2 || (centre[0][(size_t) ch] == 0.0f && centre[1][(size_t) ch] == 0.0f));

        check (onlyC, "surround pan: at width 0 in front both channels go to C alone, at 0.7071 each");

        // Constant power: each channel's gains, squared, add up to its width scaling squared.
        juce::Random r (5);
        auto power = true;
        float spreadLeast = 1.0f;

        for (int n = 0; n < 200; ++n)
        {
            SurroundPan p;
            p.x = r.nextFloat() * 2.0f - 1.0f;
            p.y = r.nextFloat() * 2.0f - 1.0f;
            p.width = r.nextFloat();
            const auto g = surroundGains (p);
            const auto scale = std::cos ((1.0 - p.width) * juce::MathConstants<double>::pi / 4.0);

            for (int side = 0; side < 2; ++side)
            {
                double sum = 0.0;

                for (const auto ch : { 0, 1, 2, 4, 5 })
                    sum += (double) g[(size_t) side][(size_t) ch] * g[(size_t) side][(size_t) ch];

                power = power && std::abs (sum - scale * scale) < 1.0e-5 && g[(size_t) side][3] == 0.0f;
            }
        }

        SurroundPan middle;
        middle.y = 0.0f;
        middle.width = 0.0f;
        const auto m = surroundGains (middle);

        for (const auto ch : { 0, 1, 2, 4, 5 })
            spreadLeast = juce::jmin (spreadLeast, m[0][(size_t) ch]);

        const auto even = std::abs (spreadLeast - 0.70710678f * std::sqrt (0.2f)) < 1.0e-6f
                          && std::abs (m[0][0] - m[0][4]) < 1.0e-7f && std::abs (m[0][2] - m[0][5]) < 1.0e-7f;
        check (power && even, "surround pan: the power is kept wherever the point is (200 random points), and in the middle "
                              "it spreads evenly over all five speakers");

        SurroundPan behind;
        behind.y = -1.0f;
        const auto b = surroundGains (behind);
        SurroundPan left;
        left.x = -1.0f;
        left.y = 0.0f;
        left.width = 0.0f;
        const auto l = surroundGains (left);
        check (b[0][0] == 0.0f && b[0][1] == 0.0f && b[0][2] == 0.0f && b[0][4] > b[0][5] && b[1][5] > b[1][4] && b[1][2] == 0.0f
                   && l[0][0] > 0.0f && l[0][4] > 0.0f && l[0][1] == 0.0f && l[0][5] == 0.0f && l[0][2] == 0.0f,
               "surround pan: behind, the left channel plays mostly from Ls and the right from Rs, none in front; "
               "at the left side, from L and Ls alone");

        SurroundPan withLfe;
        withLfe.lfeDb = 0.0f;
        SurroundPan quieter;
        quieter.lfeDb = -6.0f;
        check (surroundGains (withLfe)[0][3] == Surround::foldGain && surroundGains (withLfe)[1][3] == Surround::foldGain
                   && std::abs (surroundGains (quieter)[0][3] - juce::Decibels::decibelsToGain (-6.0f) * Surround::foldGain) < 1.0e-7f
                   && front[0][3] == 0.0f,
               "surround pan: the LFE takes each channel at 0.7071 times its share, and nothing when off");
    }

    void testMaster()
    {
        const auto stereo = mixed();
        auto fiveOne = stereo;
        fiveOne.surround = true;
        const auto two = render (stereo, 2, -1);
        const auto six = render (fiveOne, 6, 2, -1);

        check (same (six, 0, two, 0) && same (six, 1, two, 1) && silent (six, 2) && silent (six, 3) && silent (six, 4) && silent (six, 5)
                   && ! silent (six, 0),
               "5.1 master: with every pan as it starts, L and R are the stereo mix bit for bit and the rest silent");

        // Folded down (playing, no separate outputs): the master's own fold-down.
        auto placed = fiveOne;
        SessionEdits::setSurroundPan (placed, placed.tracks[0].id, { 0.3f, -0.6f, 0.7f, -6.0f });
        SessionEdits::setSurroundPan (placed, placed.tracks[1].id, { 0.0f, 1.0f, 0.0f, SurroundPan::lfeOff });
        SessionEdits::setSurroundPan (placed, placed.tracks[2].id, { -1.0f, -1.0f, 1.0f, SurroundPan::lfeOff });
        auto atLs = placed;   // the bus at Ls's direction (-110 degrees), width 0
        SessionEdits::setSurroundPan (atLs, atLs.tracks[2].id, { -1.0f, (float) (std::cos (juce::degreesToRadians (110.0)) / std::sin (juce::degreesToRadians (110.0))),
                                                                0.0f, SurroundPan::lfeOff });
        const auto master = render (placed, 6, 2, -1);
        juce::AudioBuffer<float> folded (2, frames);
        Surround::foldDown (master.getArrayOfReadPointers(), folded.getWritePointer (0), folded.getWritePointer (1), frames);
        const auto played = render (placed, 2, -1);
        check (same (played, 0, folded, 0) && same (played, 1, folded, 1) && ! silent (master, 3) && ! silent (master, 4) && ! silent (master, 5),
               "5.1 master: played folded down, it is the six channels folded as a 5.1 file is, bit for bit");

        // Separate outputs: L and R on the mix, C to Rs on the outputs after the pair.
        juce::AudioBuffer<float> wide = render (placed, 2 + SessionTrack::maxDeviceOutputs, 4);
        auto rest = true;

        for (int ch = 2; ch < wide.getNumChannels(); ++ch)
            if (ch < 4 || ch > 7)
                rest = rest && silent (wide, ch);

        check (same (wide, 0, master, 0) && same (wide, 1, master, 1) && same (wide, 4, master, 2) && same (wide, 5, master, 3)
                   && same (wide, 6, master, 4) && same (wide, 7, master, 5) && rest,
               "5.1 master: played to separate outputs, L and R go to the mix and C, LFE, Ls and Rs to the four channels given");

        // Blocks of any size give the same.
        check (same (render (placed, 6, 2, -1, 97), 2, master, 2) && same (render (placed, 6, 2, -1, 97), 5, master, 5),
               "5.1 master: the same in blocks of 97 frames as of 512");

        // The mono track alone at width 0: its sound in C, at the level a mono clip has.
        auto alone = placed;
        alone.tracks[0].mute = true;
        alone.tracks[2].mute = true;
        alone.tracks[1].pan = 0.0f;
        alone.tracks[1].gainDb = 0.0f;
        const auto c = render (alone, 6, 2, -1);
        juce::AudioBuffer<float> source (1, frames);
        source.clear();
        const auto& clip = alone.tracks[1].clips[0];
        juce::AudioBuffer<float> raw (1, (int) clip.length);
        clip.audio->read (raw, 0, 0, raw.getNumSamples());

        for (int i = 0; i < raw.getNumSamples(); ++i)
            source.setSample (0, (int) clip.position + i, raw.getSample (0, i) * FadeShapes::level (clip.fadeInShape, i, clip.fadeIn)
                                                           * FadeShapes::level (clip.fadeOutShape, clip.length - 1 - i, clip.fadeOut));

        check (worst (c, 2, source, 0) < 1.0e-6f && silent (c, 0) && silent (c, 1) && silent (c, 4) && silent (c, 5) && silent (c, 3),
               "5.1 master: a mono track in front at width 0 plays from C alone, at its own level");

        // LFE: 0.7071 of both channels times the share.
        auto lfeOnly = alone;
        lfeOnly.tracks[1].surroundPan.lfeDb = -6.0f;
        const auto withLfe = render (lfeOnly, 6, 2, -1);
        juce::AudioBuffer<float> expected (1, frames);

        for (int i = 0; i < frames; ++i)
            expected.setSample (0, i, withLfe.getSample (2, i) * juce::Decibels::decibelsToGain (-6.0f));

        check (worst (withLfe, 3, expected, 0) < 1.0e-6f && same (withLfe, 2, c, 2),
               "5.1 master: the LFE share sends the track to the LFE too (here -6 dB under C), leaving the rest as it was");

        // A track playing out of an interface pair stays there, and out of the mixdown.
        auto paired = placed;
        SessionEdits::setTrackDeviceOutput (paired, paired.tracks[1].id, 10);
        const auto out = render (paired, 2 + SessionTrack::maxDeviceOutputs, 4);
        auto withoutB = placed;
        withoutB.tracks[1].mute = true;
        const auto mixdown = render (paired, 6, 2, -1);
        const auto noB = render (withoutB, 6, 2, -1);
        check (! silent (out, 12) && ! silent (out, 13) && same (mixdown, 2, noB, 2) && same (mixdown, 4, noB, 4),
               "5.1 master: a track playing out of an interface pair still does, and a mixdown leaves it out");

        // The bus places what plays into it.
        auto busOnly = atLs;
        busOnly.tracks[1].mute = true;
        SessionEdits::setTrackOutput (busOnly, busOnly.tracks[0].id, busOnly.tracks[2].id);
        busOnly.tracks[0].sends.clear();
        const auto viaBus = render (busOnly, 6, 2, -1);
        check (silent (viaBus, 0) && silent (viaBus, 1) && silent (viaBus, 2) && silent (viaBus, 3) && ! silent (viaBus, 4) && silent (viaBus, 5),
               "5.1 master: a bus is placed by its own surround pan (here at Ls, width 0: Ls alone), not by the tracks in it");

        // Through OfflineRender: the mixdown is six channels in file order.
        juce::AudioBuffer<float> offline;
        OfflineRender::mixSession (placed, { 0, frames }, offline, nullptr, nullptr);
        juce::AudioBuffer<float> offlineStereo;
        OfflineRender::mixSession (stereo, { 0, frames }, offlineStereo, nullptr, nullptr);
        check (offline.getNumChannels() == 6 && same (offline, 3, master, 3) && same (offline, 5, master, 5) && offlineStereo.getNumChannels() == 2,
               "5.1 master: a mixdown is six channels (L R C LFE Ls Rs); a stereo master's stays two");

        check (AudioEngine::deviceOutputsOf (placed, 4).countNumberOfSetBits() == 4 && AudioEngine::deviceOutputsOf (placed, 4)[4]
                   && AudioEngine::deviceOutputsOf (placed, 4)[7] && AudioEngine::deviceOutputsOf (placed).isZero()
                   && AudioEngine::deviceOutputsOf (stereo, 4).isZero(),
               "5.1 master: to separate outputs it uses the four outputs after the pair; folded down or in stereo, none");
    }

    void testEdits (AudioEngine& engine)
    {
        auto s = mixed();
        const auto id = s.tracks[0].id;
        const auto turned = SessionEdits::setSurround (s, true) && s.surround && ! SessionEdits::setSurround (s, true);
        const auto clamped = SessionEdits::setSurroundPan (s, id, { 3.0f, -2.0f, -1.0f, -80.0f })
                             && s.tracks[0].surroundPan == SurroundPan { 1.0f, -1.0f, 0.0f, SurroundPan::lfeOff }
                             && SessionEdits::setSurroundPan (s, id, { 0.0f, 0.0f, 2.0f, 40.0f })
                             && juce::exactlyEqual (s.tracks[0].surroundPan.width, 1.0f) && juce::exactlyEqual (s.tracks[0].surroundPan.lfeDb, SurroundPan::maxLfeDb)
                             && ! SessionEdits::setSurroundPan (s, id, { 0.0f, 0.0f, 1.0f, SurroundPan::maxLfeDb });
        auto& folder = s.addFolder();
        const auto folderRefused = ! SessionEdits::setSurroundPan (s, folder.id, { 0.5f, 0.5f, 1.0f, 0.0f }) && ! SessionEdits::setSurroundPan (s, 9999, {});
        check (turned && clamped && folderRefused,
               "5.1 master edits: turned on once, the pan kept in range (LFE under -60 dB is off), folders and missing tracks refused");

        // The session file.
        const juce::TemporaryFile temp (".sbsession");
        SessionEdits::setSurroundPan (s, s.tracks[2].id, { -0.25f, -0.5f, 0.5f, -12.0f });
        const auto error = SessionFile::save (s, temp.getFile(), nullptr);
        const auto text = temp.getFile().loadFileAsString();
        const auto loaded = SessionFile::load (temp.getFile(), engine.formatManager);
        const auto& lt = loaded.state.tracks;
        check (error.isEmpty() && loaded.state.surround && lt.size() == 4 && lt[0].surroundPan == s.tracks[0].surroundPan
                   && lt[2].surroundPan == s.tracks[2].surroundPan && lt[1].surroundPan.isDefault() && text.contains ("surround=\"1\"")
                   && text.contains ("lfeDb=\"-12") && text.contains ("surroundWidth=\"0.5")
                   && text.indexOf ("surroundX") >= 0 && text.indexOf (text.indexOf ("surroundX") + 1, "surroundX") >= 0
                   && text.indexOf (text.indexOf (text.indexOf ("surroundX") + 1, "surroundX") + 1, "surroundX") < 0,
               "5.1 master: the session file keeps the master and each pan (pans as they start are left out)");

        auto plain = mixed();
        const auto plainError = SessionFile::save (plain, temp.getFile(), nullptr);
        const auto plainText = temp.getFile().loadFileAsString();
        check (plainError.isEmpty() && ! plainText.contains ("surround") && ! SessionFile::load (temp.getFile(), engine.formatManager).state.surround,
               "5.1 master: a stereo session's file has nothing of it, and loads stereo");

        // Track templates carry the pan.
        const auto xml = TrackTemplate::create (s, s.tracks[2].id, nullptr);
        auto other = mixed();
        const auto added = TrackTemplate::add (other, *xml);
        check (xml != nullptr && other.findTrack (added.trackId) != nullptr && other.findTrack (added.trackId)->surroundPan == s.tracks[2].surroundPan,
               "5.1 master: a track template keeps the surround pan");

        // Freezing and bouncing render the track's own stereo sound; the bounce's track takes the pan.
        const auto frozen = SessionEdits::frozenSource (s, id);
        const auto alone = Bounce::isolate (s, id);
        const auto landedId = Bounce::land (s, id, juce::File(), noise (2, 100, 3), 0);
        check (! frozen.surround && alone.surround && s.findTrack (landedId) != nullptr && s.findTrack (landedId)->surroundPan == s.findTrack (id)->surroundPan,
               "5.1 master: freezing renders the track in stereo, a stem stays 5.1, and a bounce's new track has the source's surround pan");
    }

    void testMixerStrips (AudioEngine& engine)
    {
        engine.session.reset (mixed(), {});
        MixerView mixer (engine);
        mixer.setSize (MixerView::stripWidth * 6, 500);
        const auto id = engine.session.getState()->tracks[0].id;
        auto* pad = mixer.findSurroundPad (id);
        const auto hidden = pad != nullptr && ! pad->isVisible() && ! mixer.findLfe (id)->isVisible();

        auto next = *engine.session.getState();
        SessionEdits::setSurround (next, true);
        engine.session.apply (std::move (next), "5.1 Master");
        TestUtils::runUntil ([&] { return pad->isVisible(); }, 2000);
        check (hidden && pad->isVisible() && mixer.findSurroundWidth (id)->isVisible() && mixer.findLfe (id)->isVisible()
                   && pad->getHeight() > 20 && mixer.findSurroundPad (0) == nullptr,
               "mixer: the surround pad, width and LFE show on each strip once the master is 5.1");

        // At the Multitrack workspace's mixer height (210) the strips widen and the pad sits beside a
        // full-height fader; sliders at 0 show their text.
        const auto shown = [] (juce::Slider& slider)
        {
            for (auto* c : slider.getChildren())
                if (auto* label = dynamic_cast<juce::Label*> (c))
                    return label->getText();

            return juce::String();
        };

        mixer.setSize (MixerView::surroundStripWidth * 6, 210);
        const auto shortFader = mixer.findFader (id)->getHeight();
        const auto shortPad = pad->getHeight();
        const auto stripWide = pad->getParentComponent()->getWidth();
        const auto beside = pad->getRight() <= mixer.findFader (id)->getX() && ! pad->getBounds().intersects (mixer.findSurroundWidth (id)->getBounds());
        mixer.setSize (MixerView::surroundStripWidth * 6, 500);
        mixer.findSurroundWidth (id)->setValue (0.0, juce::sendNotificationSync);
        const auto widthText = shown (*mixer.findSurroundWidth (id));
        mixer.findSurroundWidth (id)->setValue (100.0, juce::sendNotificationSync);
        engine.session.undo();
        check (shortFader >= 80 && shortPad >= 56 && stripWide == MixerView::surroundStripWidth && beside
                   && pad->getHeight() == pad->getWidth() && pad->getWidth() >= shortPad
                   && shown (*mixer.findPan (id)) == "C" && shown (*mixer.findLfe (id)) == "LFE off" && widthText == "W 0%",
               "mixer: in a 5.1 master the strips widen to " + juce::String (MixerView::surroundStripWidth) + ", and at the Multitrack workspace's "
                   "mixer height (210) the surround pad (" + juce::String (shortPad) + " pixels) sits beside the fader (" + juce::String (shortFader)
                   + " high); the pan, width and LFE show their text at 0 and off");

        // A click then a drag on the pad: one undo step, the point where the mouse is, inside the room.
        const auto square = pad->getSquare();
        const auto there = SurroundPad::fromPad (SurroundPad::toPad (-0.5f, -0.5f, square), square);
        pad->onChange ({ 0.2f, 0.2f, 1.0f, SurroundPan::lfeOff }, false);
        pad->onChange ({ there.x, there.y, 1.0f, SurroundPan::lfeOff }, true);
        const auto p = engine.session.getState()->tracks[0].surroundPan;
        const auto described = engine.session.getUndoDescription();
        engine.session.undo();
        const auto back = engine.session.getState()->tracks[0].surroundPan.isDefault() && engine.session.getState()->surround;
        engine.session.redo();
        check (described == "Surround Pan" && std::abs (p.x + 0.5f) < 1.0e-5f && std::abs (p.y + 0.5f) < 1.0e-5f && back
                   && engine.session.getState()->tracks[0].surroundPan == p
                   && SurroundPad::fromPad ({ -1000.0f, -1000.0f }, square) == juce::Point<float> (-1.0f, 1.0f),
               "mixer: a click and drag on the pad move the point as one undo step, kept inside the room");

        mixer.findSurroundWidth (id)->setValue (25.0, juce::sendNotificationSync);
        mixer.findLfe (id)->setValue (-3.0, juce::sendNotificationSync);
        const auto set = juce::exactlyEqual (engine.session.getState()->tracks[0].surroundPan.width, 0.25f)
                         && juce::exactlyEqual (engine.session.getState()->tracks[0].surroundPan.lfeDb, -3.0f)
                         && engine.session.getUndoDescription() == "LFE Level";
        mixer.findLfe (id)->setValue (-61.0, juce::sendNotificationSync);
        check (set && juce::exactlyEqual (engine.session.getState()->tracks[0].surroundPan.lfeDb, SurroundPan::lfeOff)
                   && mixer.findLfe (id)->getTextFromValue (-61.0) == "LFE off",
               "mixer: the width and LFE sliders set the track's, the far left turning the LFE off");

        engine.session.undo();
        check (juce::exactlyEqual (engine.session.getState()->tracks[0].surroundPan.width, 0.25f)
                   && engine.session.getState()->tracks[0].surroundPan.lfeDb <= SurroundPan::lfeOff,
               "mixer: the LFE's changes undo, the width staying as set");
    }
}

namespace
{
    void testFiveOneTracks (AudioEngine& engine)
    {
        SessionState s;
        s.sampleRate = rate;
        s.surround = true;
        auto& t = addFiveOneTrack (s, 6, 300, 31);
        const auto id = t.id;
        const auto master = render (s, 6, 2, -1);
        const auto expected = placed (s.tracks[0].clips[0], 1.0f);
        auto exact = true;

        for (int ch = 0; ch < 6; ++ch)
            exact = exact && same (master, ch, expected, ch);

        check (exact, "5.1 track: a 5.1 clip plays its six channels to the master's six, bit for bit");

        auto mono = s;
        mono.tracks.clear();
        addFiveOneTrack (mono, 1, 300, 32);
        auto stereo = s;
        stereo.tracks.clear();
        addFiveOneTrack (stereo, 2, 300, 33);
        const auto m = render (mono, 6, 2, -1), st = render (stereo, 6, 2, -1);
        const auto me = placed (mono.tracks[0].clips[0], 1.0f), se = placed (stereo.tracks[0].clips[0], 1.0f);
        check (same (m, 2, me, 2) && silent (m, 0) && silent (m, 1) && silent (m, 3) && silent (m, 4) && silent (m, 5)
                   && same (st, 0, se, 0) && same (st, 1, se, 1) && silent (st, 2) && silent (st, 4),
               "5.1 track: a mono clip plays from C and a stereo one from L and R");

        // Volume (setting and envelope) and polarity act on all six; pan, surround pan and effects not at all.
        auto quieter = s;
        quieter.tracks[0].gainDb = -6.0f;
        quieter.tracks[0].invertPolarity = true;
        quieter.tracks[0].pan = 1.0f;
        quieter.tracks[0].surroundPan = { -1.0f, -1.0f, 0.0f, 0.0f };
        auto enveloped = quieter;
        enveloped.tracks[0].gainDb = 0.0f;
        enveloped.tracks[0].volumeEnvelope.points = { { 0, -6.0f }, { (juce::int64) frames, -6.0f } };
        const auto q = render (quieter, 6, 2, -1), e = render (enveloped, 6, 2, -1);
        const auto g = -juce::Decibels::decibelsToGain (-6.0f);
        const auto scaled = placed (s.tracks[0].clips[0], g);
        auto volumes = true;

        for (int ch = 0; ch < 6; ++ch)
            volumes = volumes && worst (q, ch, scaled, ch) < 1.0e-7f && worst (e, ch, scaled, ch) < 1.0e-6f;

        check (volumes, "5.1 track: its volume, a volume envelope and its polarity act on all six channels; its pan and surround pan do nothing");

        Delays fx;
        fx.latency = 1;
        const auto withFx = render (s, 6, 2, -1, 512, &fx);
        check (same (withFx, 0, master, 0) && same (withFx, 2, master, 2) && same (withFx, 5, master, 5) && fx.processed.count (id) == 0,
               "5.1 track: its effects don't run and add no delay");

        // Into a bus with effects of 64 frames' latency: the bus gets the fold-down, and the track (read
        // 64 frames ahead) has its six channels to the master held back to line up with the bus's output.
        auto routed = s;
        auto& bus = routed.addBus();
        const auto busId = bus.id;
        SessionEdits::setSend (routed, id, { busId, 0.0f, false });
        Delays busFx;
        const auto r = render (routed, 6, 2, -1, 97, &busFx);
        juce::AudioBuffer<float> folded (2, frames);
        Surround::foldDown (expected.getArrayOfReadPointers(), folded.getWritePointer (0), folded.getWritePointer (1), frames);
        const auto& heldBack = expected;
        juce::AudioBuffer<float> busOut (2, frames);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < frames; ++i)
                busOut.setSample (ch, i, 0.5f * folded.getSample (ch, i));   // its effect halves it, in time

        juce::AudioBuffer<float> lr (2, frames);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < frames; ++i)
                lr.setSample (ch, i, heldBack.getSample (ch, i) + busOut.getSample (ch, i));

        check (worst (r, 0, lr, 0) < 1.0e-6f && worst (r, 1, lr, 1) < 1.0e-6f && same (r, 2, heldBack, 2) && same (r, 3, heldBack, 3)
                   && same (r, 4, heldBack, 4) && same (r, 5, heldBack, 5) && busFx.processed.count (busId) == 1 && busFx.processed.count (id) == 0,
               "5.1 track: a send carries its fold-down into a bus, and its six channels line up with the bus's output through "
               "its 64 frames of effects (in blocks of 97)");

        // In a stereo master, and into a bus in place of the master: folded down.
        auto plain = s;
        plain.surround = false;
        const auto two = render (plain, 2, -1);
        auto toBus = routed;
        toBus.tracks[0].sends.clear();
        SessionEdits::setTrackOutput (toBus, id, busId);
        const auto viaBus = render (toBus, 6, 2, -1);
        check (same (two, 0, folded, 0) && same (two, 1, folded, 1) && same (viaBus, 0, folded, 0) && same (viaBus, 1, folded, 1) && silent (viaBus, 2)
                   && silent (viaBus, 3) && silent (viaBus, 4) && silent (viaBus, 5),
               "5.1 track: in a stereo master, or playing into a bus, it is folded down to stereo, bit for bit");

        // Import, edits, the file, templates, freezing and bouncing.
        auto fresh = s;
        fresh.tracks.clear();
        const auto wideClip = SessionEdits::addClip (fresh, 99, juce::File(), noise (6, 100, 1), 0);
        const auto stereoClip = SessionEdits::addClip (fresh, 99, juce::File(), noise (2, 100, 2), 0);
        auto stereoMaster = fresh;
        stereoMaster.surround = false;
        stereoMaster.tracks.clear();
        SessionEdits::addClip (stereoMaster, 99, juce::File(), noise (6, 100, 3), 0);
        check (fresh.tracks.size() == 2 && fresh.tracks[0].fiveOne && ! fresh.tracks[1].fiveOne && wideClip != stereoClip && ! stereoMaster.tracks[0].fiveOne,
               "5.1 track: six-channel audio brought onto a new track in a 5.1 master makes it a 5.1 track; stereo audio, or a stereo master, doesn't");

        auto edits = s;
        auto& b2 = edits.addBus();
        auto& f2 = edits.addFolder();
        const auto refused = ! SessionEdits::setTrackFiveOne (edits, b2.id, true) && ! SessionEdits::setTrackFiveOne (edits, f2.id, true)
                             && ! SessionEdits::setTrackFiveOne (edits, id, true) && ! SessionEdits::freezeTrack (edits, id, juce::File(), noise (2, 10, 4))
                             && SessionEdits::setTrackFiveOne (edits, id, false) && ! edits.tracks[0].fiveOne;
        auto frozen = s;
        frozen.tracks[0].fiveOne = false;
        SessionEdits::freezeTrack (frozen, id, juce::File(), noise (2, 10, 5));
        check (refused && ! SessionEdits::setTrackFiveOne (frozen, id, true),
               "5.1 track edits: buses, folders and frozen tracks can't be 5.1 tracks, and a 5.1 track can't be frozen");

        const juce::TemporaryFile temp (".sbsession");
        SessionFile::save (s, temp.getFile(), nullptr);
        const auto loaded = SessionFile::load (temp.getFile(), engine.formatManager);
        const auto xml = TrackTemplate::create (s, id, nullptr);
        auto other = mixed();
        const auto added = TrackTemplate::add (other, *xml);
        auto bounced = s;
        const auto landed = Bounce::land (bounced, id, juce::File(), noise (6, 100, 6), 0);
        const auto landedStereo = Bounce::land (bounced, id, juce::File(), noise (2, 100, 7), 0);
        check (loaded.state.tracks.size() == 1 && loaded.state.tracks[0].fiveOne && temp.getFile().loadFileAsString().contains ("fiveOne=\"1\"")
                   && other.findTrack (added.trackId)->fiveOne && bounced.findTrack (landed)->fiveOne && ! bounced.findTrack (landedStereo)->fiveOne,
               "5.1 track: the session file and track templates keep it, and a six-channel bounce lands on a 5.1 track");
    }

    void testFiveOneControls (AudioEngine& engine)
    {
        SessionState s;
        s.sampleRate = rate;
        s.surround = true;
        addFiveOneTrack (s, 6, 0, 41).fiveOne = false;
        addClipTrack (s, 2, 0, 42);
        engine.session.reset (s, {});
        const auto id = engine.session.getState()->tracks[0].id, otherId = engine.session.getState()->tracks[1].id;
        MultitrackView view (engine);
        MixerView mixer (engine);
        mixer.setSize (MixerView::surroundStripWidth * 4, 500);
        const auto padBefore = mixer.findSurroundPad (id)->isVisible();
        view.setTrackFiveOne (id, true);
        const auto described = engine.session.getUndoDescription();
        TestUtils::runUntil ([&] { return ! mixer.findSurroundPad (id)->isVisible(); }, 2000);
        check (padBefore && engine.session.getState()->tracks[0].fiveOne && described == "5.1 Track" && ! mixer.findSurroundPad (id)->isVisible()
                   && ! mixer.findPan (id)->isEnabled() && mixer.findSurroundPad (otherId)->isVisible() && mixer.findPan (otherId)->isEnabled(),
               "5.1 track: made one from the track's menu as an undo step, its mixer strip drops the pan and surround controls");

        engine.session.undo();
        check (! engine.session.getState()->tracks[0].fiveOne, "5.1 track: undone, it is a stereo track again");
    }
}

void runSurroundSessionTests (AudioEngine& engine)
{
    testFiveOneTracks (engine);
    testFiveOneControls (engine);
    testGains();
    testMaster();
    testEdits (engine);
    testMixerStrips (engine);
}
