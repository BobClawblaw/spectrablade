// The mixdown must be the mix playback makes, so mixes are compared bit for bit.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioFileIO.h"
#include "../Source/OfflineRender.h"
#include "../Source/TrackEffects.h"
#include "TestPlugins.h"
#include "TestUtils.h"
#include "MixDownTests.h"

namespace
{
    using TestUtils::check;
    using TestPlugins::DelayPlugin;

    constexpr double rate = 48000.0;

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

    /** A mono and a stereo track, a volume envelope, and a bus the first sends to. */
    SessionState session()
    {
        SessionState s;
        s.sampleRate = rate;

        for (int t = 0; t < 2; ++t)
        {
            auto& track = s.addTrack();
            track.pan = t == 0 ? -0.4f : 0.2f;
            track.gainDb = t == 0 ? -2.0f : 1.5f;

            SessionClip c;
            c.id = s.nextId++;
            c.audio = noise (t == 0 ? 1 : 2, 9000, 31 + t);
            c.position = t == 0 ? 100 : 1500;
            c.length = c.audio->getNumFrames();
            c.fadeIn = 300;
            c.fadeOut = 500;
            track.clips.push_back (c);
        }

        const auto bus = s.addBus().id;
        SessionEdits::setSend (s, s.tracks[0].id, { bus, -6.0f, false });
        SessionEdits::setEnvelope (s, s.tracks[1].id, AutomationParameter::volume, { { { 2000, -20.0f }, { 8000, 3.0f } } });
        return s;
    }

    /** Playback's mix: frames [start, start + frames) in blocks of blockSize, primed for start. */
    juce::AudioBuffer<float> play (const SessionState& s, juce::int64 start, int frames, int blockSize, SessionMixer::Inserts* inserts)
    {
        juce::AudioBuffer<float> out (2, frames);
        SessionMixer::Buffers buffers;
        buffers.reserve (blockSize);

        if (inserts != nullptr)
            SessionMixer::prime (s, start, buffers, *inserts);

        for (int p = 0; p < frames; p += blockSize)
            SessionMixer::render (s, start + p, out, p, juce::jmin (blockSize, frames - p), buffers, inserts);

        return out;
    }

    bool same (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
            return false;

        for (int ch = 0; ch < a.getNumChannels(); ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
                if (a.getSample (ch, i) != b.getSample (ch, i))
                    return false;

        return true;
    }

    /** Runs fn on a background thread with the message loop going, as the app's jobs do. */
    void inBackground (const std::function<void()>& fn)
    {
        std::atomic<bool> done { false };
        std::thread thread ([&] { fn(); done = true; });
        TestUtils::runUntil ([&] { return done.load(); }, 60000);
        thread.join();
    }

    juce::AudioPluginFormat* findFormat (AudioEngine& engine, const juce::String& name)
    {
        for (auto* f : engine.pluginFormatManager.getFormats())
            if (f->getName() == name)
                return f;

        return nullptr;
    }
}

void runMixDownTests (AudioEngine& engine)
{
    const auto s = session();
    const auto length = s.getLength();
    const auto first = s.tracks[0].id, second = s.tracks[1].id, bus = s.tracks[2].id;

    // Without effects: the whole session, as playback mixes it.
    {
        juce::AudioBuffer<float> mix;
        const auto ok = OfflineRender::mixSession (s, { 0, length }, mix, nullptr, {});
        check (ok && mix.getNumChannels() == 2 && mix.getNumSamples() == (int) length && same (mix, play (s, 0, (int) length, 512, nullptr)),
               "mixdown: the whole session (" + juce::String (length) + " frames) is playback's mix bit for bit");
    }

    // With latent effects on a track and the bus, from part way in: primed, so it is in time.
    {
        TrackEffects live, offline;

        for (auto* e : { &live, &offline })
        {
            e->prepare (rate);
            e->add (first, std::make_unique<DelayPlugin> (300, 0.5f));
            e->add (bus, std::make_unique<DelayPlugin> (700, 1.0f));
            e->add (second, std::make_unique<DelayPlugin> (55, 2.0f));
        }

        const juce::Range<juce::int64> range { 2345, length + 4000 };
        juce::AudioBuffer<float> mix;
        OfflineRender::mixSession (s, range, mix, &offline, {});
        const auto whole = play (s, 0, (int) range.getEnd(), 256, &live);
        juce::AudioBuffer<float> expected (2, (int) range.getLength());

        for (int ch = 0; ch < 2; ++ch)
            expected.copyFrom (ch, 0, whole, ch, (int) range.getStart(), (int) range.getLength());

        check (same (mix, expected), "mixdown: a range with 300, 700 and 55 frames of effect latency matches playback from the start");

        auto cancelled = 0;
        const auto ok = OfflineRender::mixSession (s, range, mix, &offline, [&] (float p) { ++cancelled; return p < 0.5f; });
        check (! ok && cancelled > 1, "mixdown: progress can cancel it part way");
    }

    // Fresh effects from a session's saved chains: Apple's AUDelay, a bypassed copy, and one that can't load.
    if (auto* au = findFormat (engine, "AudioUnit"))
    {
        juce::OwnedArray<juce::PluginDescription> types;
        au->findAllTypesForFile (types, "AudioUnit:Effects/aufx,dely,appl");

        if (types.size() == 1)
        {
            std::map<juce::uint32, std::unique_ptr<juce::XmlElement>> chains;
            auto xml = std::make_unique<juce::XmlElement> ("EFFECTS");

            for (auto bypassed : { false, true })
            {
                auto* slot = xml->createNewChildElement ("SLOT");
                slot->setAttribute ("bypassed", bypassed);
                slot->addChildElement (types[0]->createXml().release());
            }

            juce::PluginDescription missing;
            missing.name = "Missing Effect";
            missing.pluginFormatName = "VST3";
            missing.fileOrIdentifier = "/nonexistent/Missing Effect.vst3";
            xml->createNewChildElement ("SLOT")->addChildElement (missing.createXml().release());
            chains[first] = std::move (xml);

            auto effects = std::make_unique<TrackEffects>();
            juce::String errors;
            juce::AudioBuffer<float> mix;
            inBackground ([&]
            {
                errors = OfflineRender::loadTrackEffects (chains, engine.pluginFormatManager, rate, *effects);
                OfflineRender::mixSession (s, { 0, length }, mix, effects.get(), {});
            });

            auto* plugin = effects->getPlugin (first, 0);
            check (effects->getNumSlots (first) == 2 && ! effects->isBypassed (first, 0) && effects->isBypassed (first, 1)
                       && plugin != nullptr && plugin->isNonRealtime(),
                   "mixdown: fresh effects load in order with their bypass, set for offline processing");
            check (errors.contains ("Missing Effect") && ! errors.containsChar ('\n'),
                   "mixdown: an effect that can't load is named and left out (" + errors + ")");
            check (! same (mix, play (s, 0, (int) length, 512, nullptr)), "mixdown: the loaded delay is heard in the mix");
        }
    }

    // To a file: 32-bit float keeps the mix exactly.
    {
        juce::AudioBuffer<float> mix;
        OfflineRender::mixSession (s, { 0, length }, mix, nullptr, {});
        const auto file = juce::File::createTempFile (".wav");
        const auto error = AudioFileIO::save (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (mix)), rate, file, 32, engine.formatManager);
        const auto loaded = AudioFileIO::load (file, engine.formatManager);
        juce::AudioBuffer<float> back;

        if (loaded.snapshot != nullptr)
        {
            back.setSize (loaded.snapshot->getNumChannels(), (int) loaded.snapshot->getNumFrames());
            loaded.snapshot->read (back, 0, 0, back.getNumSamples());
        }

        check (error.isEmpty() && loaded.sampleRate == rate && same (back, mix), "mixdown: written as 32-bit float WAV and read back bit for bit");
        file.deleteFile();
    }
}
