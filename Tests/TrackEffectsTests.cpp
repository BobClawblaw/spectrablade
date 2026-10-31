// Delay compensation must line tracks up exactly, so mixes are compared bit for bit.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/SessionMixer.h"
#include "../Source/TrackEffects.h"
#include "TestUtils.h"
#include "TrackEffectsTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    /** Delays its input by a known latency, which it reports, and scales it. */
    class DelayPlugin final : public juce::AudioPluginInstance
    {
    public:
        DelayPlugin (int latencyFrames, float g)
            : AudioPluginInstance (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true)
                                                    .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
              latency (latencyFrames), gain (g)
        {
            setLatencySamples (latency);
        }

        void fillInPluginDescription (juce::PluginDescription& d) const override
        {
            d.name = "Test Delay";
            d.pluginFormatName = "Test";
            d.fileOrIdentifier = "test-delay";
            d.numInputChannels = d.numOutputChannels = 2;
        }

        const juce::String getName() const override  { return "Test Delay"; }

        void prepareToPlay (double, int) override
        {
            line.setSize (2, juce::jmax (1, latency));
            line.clear();
            write = 0;
            ++prepared;
        }

        void releaseResources() override             {}
        void reset() override                        { line.clear(); write = 0; }

        void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
        {
            maxSeen = juce::jmax (maxSeen, b.getNumSamples());

            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                for (int ch = 0; ch < 2; ++ch)
                {
                    const auto in = b.getSample (ch, i);

                    if (latency == 0)
                    {
                        b.setSample (ch, i, in * gain);
                        continue;
                    }

                    b.setSample (ch, i, line.getSample (ch, write) * gain);
                    line.setSample (ch, write, in);
                }

                if (latency > 0)
                    write = (write + 1) % latency;
            }
        }

        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override            { return false; }
        bool producesMidi() const override           { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override              { return false; }
        int getNumPrograms() override                { return 1; }
        int getCurrentProgram() override             { return 0; }
        void setCurrentProgram (int) override        {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}

        void getStateInformation (juce::MemoryBlock& m) override { m.append (&gain, sizeof (gain)); }
        void setStateInformation (const void* data, int size) override
        {
            if (size == (int) sizeof (gain))
                std::memcpy (&gain, data, sizeof (gain));
        }

        const int latency;
        float gain;
        int prepared = 0, maxSeen = 0;

    private:
        juce::AudioBuffer<float> line;
        int write = 0;
    };

    /** Noise, so any misalignment shows. */
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

    SessionState twoTracks()
    {
        SessionState s;
        s.sampleRate = rate;

        for (int t = 0; t < 2; ++t)
        {
            auto& track = s.addTrack();
            track.pan = t == 0 ? -0.4f : 0.0f;
            track.gainDb = t == 0 ? -2.0f : 1.5f;

            SessionClip c;
            c.id = s.nextId++;
            c.audio = noise (t == 0 ? 1 : 2, 6000, 7 + t);
            c.position = t == 0 ? 50 : 700;
            c.length = c.audio->getNumFrames();
            c.fadeIn = 100;
            c.fadeOut = 200;
            track.clips.push_back (c);
        }

        return s;
    }

    /** Mixes frames [start, start + frames) in blocks of blockSize, primed for start first. */
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

    bool same (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < a.getNumSamples(); ++i)
                if (a.getSample (ch, i) != b.getSample (ch, i))
                    return false;

        return true;
    }
}

void runTrackEffectsTests()
{
    const auto s = twoTracks();
    const auto first = s.tracks[0].id, second = s.tracks[1].id;
    const auto dry = play (s, 0, 8000, 8000, nullptr);

    // A chain of two delays (300 + 77 frames) on one track: read ahead, the mix lines up exactly.
    {
        TrackEffects effects;
        effects.prepare (rate);
        effects.add (first, std::make_unique<DelayPlugin> (300, 1.0f));
        effects.add (first, std::make_unique<DelayPlugin> (77, 1.0f));

        check (effects.getChainLatency (first) == 377 && effects.getChainLatency (second) == 0,
               "track effects: a chain's latency is the sum of its plug-ins' (" + juce::String (effects.getChainLatency (first)) + ")");
        check (same (play (s, 0, 8000, 333, &effects), dry), "track effects: 377 frames of latency are compensated bit for bit");

        // Blocks longer than a plug-in is given are split.
        check (same (play (s, 0, 8000, 3000, &effects), dry), "track effects: 3000-frame blocks are split for the plug-ins");
        check (dynamic_cast<DelayPlugin*> (effects.getPlugin (first, 0))->maxSeen <= TrackEffects::maxBlock,
               "track effects: plug-ins never get more than " + juce::String (TrackEffects::maxBlock) + " frames");

        // After a jump, priming fills the chain so the first block is already in time.
        const auto later = play (s, 2500, 3000, 256, &effects);
        juce::AudioBuffer<float> dryLater (2, 3000);
        for (int ch = 0; ch < 2; ++ch)
            dryLater.copyFrom (ch, 0, dry, ch, 2500, 3000);
        check (same (later, dryLater), "track effects: playing from 2500 after priming matches the dry mix");

        // Effects on both tracks with different latencies.
        effects.add (second, std::make_unique<DelayPlugin> (1029, 1.0f));
        check (same (play (s, 0, 8000, 512, &effects), dry), "track effects: tracks with 377 and 1029 frames of latency line up");

        // A bypassed plug-in is skipped and its latency not counted.
        effects.setBypassed (first, 0, true);
        check (effects.getChainLatency (first) == 77 && effects.isBypassed (first, 0),
               "track effects: a bypassed plug-in's latency is not counted");
        check (same (play (s, 0, 8000, 512, &effects), dry), "track effects: the mix still lines up with one plug-in bypassed");

        // Moving a slot keeps the chain's sound; removing hands the plug-in back.
        effects.move (first, 0, 1);
        check (effects.isBypassed (first, 1) && ! effects.isBypassed (first, 0), "track effects: slots move with their bypass");
        auto removed = effects.remove (first, 1);
        check (removed != nullptr && effects.getNumSlots (first) == 1, "track effects: removing a slot hands the plug-in back");

        // Changing a parameter changes the sound.
        dynamic_cast<DelayPlugin*> (effects.getPlugin (first, 0))->gain = 0.5f;
        auto halfFirst = s;
        halfFirst.tracks[0].gainDb += juce::Decibels::gainToDecibels (0.5f);
        const auto expected = play (halfFirst, 0, 8000, 8000, nullptr);
        const auto wet = play (s, 0, 8000, 512, &effects);
        float worst = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 8000; ++i)
                worst = juce::jmax (worst, std::abs (wet.getSample (ch, i) - expected.getSample (ch, i)));
        check (worst < 1.0e-6f, "track effects: the effect's output is what the track plays (worst " + juce::String (worst, 9) + ")");

        // Muted tracks are not processed at all.
        auto muted = s;
        muted.tracks[0].mute = muted.tracks[1].mute = true;
        check (play (muted, 0, 4000, 512, &effects).getMagnitude (0, 4000) == 0.0f, "track effects: muted tracks stay silent");

        // Saved as rack-style slots.
        auto xml = effects.createXml (second);
        check (xml != nullptr && xml->getNumChildElements() == 1 && xml->getChildElement (0)->getChildByName ("PLUGIN") != nullptr
                   && xml->getChildElement (0)->getChildByName ("STATE") != nullptr && effects.createXml (9999) == nullptr,
               "track effects: a chain saves as EFFECTS slots with description and state");

        // A new rate re-prepares every plug-in.
        const auto before = dynamic_cast<DelayPlugin*> (effects.getPlugin (second, 0))->prepared;
        effects.prepare (44100.0);
        check (effects.getSampleRate() == 44100.0 && dynamic_cast<DelayPlugin*> (effects.getPlugin (second, 0))->prepared == before + 1,
               "track effects: a new session rate re-prepares the plug-ins");

        effects.clear();
        check (effects.getNumSlots (first) == 0 && effects.getNumSlots (second) == 0, "track effects: clear removes every chain");
    }

    // The player primes on its own when playback starts somewhere new.
    {
        AudioDocument document;
        Session session;
        session.reset (s, {});
        TrackEffects effects;
        effects.prepare (rate);
        effects.add (first, std::make_unique<DelayPlugin> (300, 1.0f));
        effects.add (second, std::make_unique<DelayPlugin> (64, 1.0f));

        DocumentPlayerProcessor player (document, session, effects);
        player.setMode (DocumentPlayerProcessor::Mode::session);
        player.prepareToPlay (rate, 256);

        juce::AudioBuffer<float> block (2, 256);
        juce::MidiBuffer midi;

        // Play a little from the start, then jump to 3000.
        player.play (0, -1);
        for (int i = 0; i < 4; ++i)
            player.processBlock (block, midi);

        player.play (3000, -1);
        juce::AudioBuffer<float> out (2, 2048);

        for (int p = 0; p < 2048; p += 256)
        {
            player.processBlock (block, midi);
            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, p, block, ch, 0, 256);
        }

        float worst = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 2048; ++i)
                worst = juce::jmax (worst, std::abs (out.getSample (ch, i) - dry.getSample (ch, 3000 + i)));

        check (worst < 1.0e-6f, "track effects: after a jump the player plays in time (worst " + juce::String (worst, 9) + ")");
        player.releaseResources();
    }

    // Session files carry each track's EFFECTS through to its new id.
    {
        auto dir = juce::File::createTempFile ("fx");
        dir.createDirectory();
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        SessionState empty;
        empty.sampleRate = rate;
        empty.addTrack();
        empty.addTrack();
        const auto target = dir.getChildFile ("fx.sbsession");
        const auto error = SessionFile::save (empty, target, [&] (juce::uint32 id)
        {
            if (id != empty.tracks[1].id)
                return std::unique_ptr<juce::XmlElement>();

            auto xml = std::make_unique<juce::XmlElement> ("EFFECTS");
            xml->createNewChildElement ("SLOT")->setAttribute ("bypassed", true);
            return xml;
        });

        const auto loaded = SessionFile::load (target, formats);
        const auto secondId = loaded.state.tracks.size() == 2 ? loaded.state.tracks[1].id : 0u;
        check (error.isEmpty() && loaded.effects.size() == 1 && loaded.effects.count (secondId) == 1
                   && loaded.effects.at (secondId)->getChildElement (0)->getBoolAttribute ("bypassed"),
               "session file: track effects are saved and come back on their track");
        dir.deleteRecursively();
    }
}
