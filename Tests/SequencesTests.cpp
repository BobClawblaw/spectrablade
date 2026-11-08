// Steps are compared with their commands' own results, bit for bit.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/Delivery.h"
#include "../Source/AudioEngine.h"
#include "../Source/AudioEdits.h"
#include "../Source/AudioFileIO.h"
#include "../Source/Batch.h"
#include "../Source/BuiltInEffects.h"
#include "../Source/Dynamics.h"
#include "../Source/Equalizer.h"
#include "../Source/Loudness.h"
#include "../Source/Modulation.h"
#include "../Source/Multiband.h"
#include "../Source/Exciter.h"
#include "../Source/Maximizer.h"
#include "../Source/OfflineRender.h"
#include "../Source/Reverb.h"
#include "../Source/SampleRateConverter.h"
#include "../Source/Sequences.h"
#include "../Source/TimeStretch.h"
#include "../Source/Dereverb.h"
#include "../Source/Leveller.h"
#include "../Source/CentreChannel.h"
#include "../Source/PhaseCorrection.h"
#include "../Source/PitchCorrection.h"
#include "TestUtils.h"
#include "SequencesTests.h"

namespace
{
    using TestUtils::check;
    using Snapshot = AudioDocument::Snapshot;
    using Sequences::Step;

    Snapshot music (int channels, int frames, double rate, int seed)
    {
        juce::Random r (seed);
        juce::AudioBuffer<float> b (channels, frames);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, 0.4f * (float) std::sin (juce::MathConstants<double>::twoPi * (330.0 + 97.0 * ch) * i / rate)
                                        * (0.5f + 0.5f * (float) std::sin (i / 3000.0)) + 0.1f * (r.nextFloat() * 2.0f - 1.0f) + 0.01f);

        return AudioEdits::fromBuffer (std::move (b));
    }

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    bool identical (const Snapshot& a, const Snapshot& b)
    {
        if (a == nullptr || b == nullptr || a->getNumChannels() != b->getNumChannels() || a->getNumFrames() != b->getNumFrames())
            return false;

        const auto x = readAll (*a), y = readAll (*b);

        for (int ch = 0; ch < x.getNumChannels(); ++ch)
            if (std::memcmp (x.getReadPointer (ch), y.getReadPointer (ch), sizeof (float) * (size_t) x.getNumSamples()) != 0)
                return false;

        return true;
    }

    /** Runs work on another thread while the message loop turns (plug-ins are made on it). */
    void offThread (std::function<void()> work)
    {
        std::atomic<bool> done { false };
        std::thread worker ([&] { work(); done = true; });
        TestUtils::runUntil ([&] { return done.load(); }, 120000);
        worker.join();
    }

    std::unique_ptr<juce::XmlElement> compressorRack()
    {
        BuiltInEffects::CompressorProcessor c;
        c.setSettings ({ -30.0, 6.0, 3.0, 5.0, 80.0, 4.0 });
        juce::MemoryBlock state;
        c.getStateInformation (state);
        auto rack = std::make_unique<juce::XmlElement> ("SPECTRABLADE_RACK");
        auto* slot = rack->createNewChildElement ("SLOT");
        juce::PluginDescription d;
        c.fillInPluginDescription (d);
        slot->addChildElement (d.createXml().release());
        slot->createNewChildElement ("STATE")->addTextElement (state.toBase64Encoding());
        return rack;
    }
}

void runSequencesTests (AudioEngine& engine)
{
    const double rate = 48000.0;
    const auto audio = music (2, 48000, rate, 7);
    const auto& s = *audio;
    const juce::int64 a = 1000, b = 30000;
    const Markers markers { { 2400, 0, "One", -1 }, { 24000, 4800, "Two", -1 } };

    // Every step against what its command calls, on a part of the file.
    {
        const Dynamics::Compressor comp { -24.0, 3.0, 6.0, 10.0, 120.0, 2.0 };
        const Dynamics::Limiter lim { 3.0, -1.5, 40.0, 5.0, true };
        const Multiband::DeEsser ess { 6000.0, -30.0, 9.0, true };
        const Multiband::Width width { 120.0, 2500.0, { 0.0, 100.0, 140.0 } };
        Exciter::Settings exc;
        exc.lowCrossover = 180.0;
        exc.highCrossover = 2800.0;
        exc.bands = {{ { 6.0, Exciter::Curve::tube, 50.0 }, { 9.0, Exciter::Curve::tape, 40.0 }, { 12.0, Exciter::Curve::transistor, 30.0 } }};
        exc.outputDb = -1.0;
        const Maximizer::Settings maxi { 6.0, -1.0, Maximizer::Character::loud, 50.0 };
        const Modulation::Delay chorus { 0.8, 12.0, 4.0, 0.1, 40.0, 30.0 };
        const Modulation::Phaser ph { 0.5, 300.0, 3000.0, 6, 0.3, 50.0, 20.0 };   // stages 2 in the dialog: 2 * (2 + 1)
        const RoomReverb::Settings rev { 20.0, 1.8, 60.0, 40.0, 80.0, 30.0 };
        const Equalizer::Band band { Equalizer::BandType::highShelf, 4000.0, 3.5, 0.71 };
        const auto measured = Loudness::measure (s, a, b, rate);
        const auto wanted = -18.0 - measured->integrated;

        struct Case { Step step; Snapshot expected; };
        const std::vector<Case> cases {
            { { "gain", { { "db", -3.25 } }, nullptr }, AudioEdits::gain (s, a, b, -3.25f) },
            { { "normalize", { { "db", -0.5 }, { "apart", 0.0 } }, nullptr }, AudioEdits::normalize (s, a, b, -0.5f) },
            { { "normalize", { { "db", -0.5 }, { "apart", 1.0 } }, nullptr }, AudioEdits::normalizeChannels (s, a, b, -0.5f) },
            { { "matchLoudness", { { "target", -18.0 }, { "ceiling", -1.0 }, { "limit", 0.0 } }, nullptr },
              AudioEdits::gain (s, a, b, (float) juce::jmin (wanted, -1.0 - measured->truePeak)) },
            { { "deliverySpec", { { "target", -23.0 }, { "tolerance", 0.5 }, { "ceiling", -1.0 } }, nullptr },
              Delivery::deliver (s, a, b, rate, Delivery::specs()[0]).snapshot },
            { { "invert", {}, nullptr }, AudioEdits::invert (s, a, b) },
            { { "reverse", {}, nullptr }, AudioEdits::reverse (s, a, b) },
            { { "removeDc", {}, nullptr }, AudioEdits::removeDcOffset (s, a, b) },
            { { "swapChannels", {}, nullptr }, AudioEdits::swapChannels (s, a, b) },
            { { "invertLeft", {}, nullptr }, AudioEdits::invertChannel (s, a, b, 0) },
            { { "invertRight", {}, nullptr }, AudioEdits::invertChannel (s, a, b, 1) },
            { { "leftToRight", {}, nullptr }, AudioEdits::copyChannel (s, a, b, 0, 1) },
            { { "rightToLeft", {}, nullptr }, AudioEdits::copyChannel (s, a, b, 1, 0) },
            { { "mono", {}, nullptr }, AudioEdits::convertChannels (s, 1) },
            { { "balance", { { "pan", -40.0 } }, nullptr }, AudioEdits::balance (s, a, b, -0.4f) },
            { { "stereoWidth", { { "width", 150.0 } }, nullptr }, AudioEdits::stereoWidth (s, a, b, 1.5f) },
            { { "centre", { { "keep", 1.0 }, { "low", 150.0 }, { "high", 6000.0 }, { "reduction", 15.0 } }, nullptr },
              CentreChannel::process (s, a, b, rate, { true, 150.0f, 6000.0f, 15.0f }) },
            { { "echo", { { "delay", 250.0 }, { "feedback", 40.0 } }, nullptr }, AudioEdits::echo (s, a, b, 12000, 0.4f) },
            { { "tremolo", { { "rate", 4.5 }, { "depth", 60.0 } }, nullptr }, AudioEdits::tremolo (s, a, b, rate, 4.5, 0.6f) },
            { { "eqBand", { { "type", (double) band.type }, { "frequency", 4000.0 }, { "gain", 3.5 }, { "q", 0.71 } }, nullptr },
              Equalizer::apply (s, a, b, rate, { band }) },
            { { "compressor", { { "threshold", -24.0 }, { "ratio", 3.0 }, { "knee", 6.0 }, { "attack", 10.0 }, { "release", 120.0 }, { "makeup", 2.0 } }, nullptr },
              Dynamics::compress (s, a, b, rate, comp) },
            { { "limiter", { { "gain", 3.0 }, { "ceiling", -1.5 }, { "release", 40.0 }, { "lookahead", 5.0 }, { "peaks", 1.0 } }, nullptr },
              Dynamics::limit (s, a, b, rate, lim) },
            { { "deEsser", { { "frequency", 6000.0 }, { "threshold", -30.0 }, { "reduction", 9.0 }, { "mode", 1.0 } }, nullptr },
              Multiband::deEss (s, a, b, rate, ess) },
            { { "multibandWidth", { { "lowCrossover", 120.0 }, { "highCrossover", 2500.0 }, { "low", 0.0 }, { "mid", 100.0 }, { "high", 140.0 } }, nullptr },
              Multiband::widen (s, a, b, rate, width) },
            { { "exciter", { { "lowCrossover", 180.0 }, { "highCrossover", 2800.0 }, { "lowDrive", 6.0 }, { "lowCurve", 0.0 }, { "lowMix", 50.0 }, { "midDrive", 9.0 }, { "midCurve", 1.0 },
                             { "midMix", 40.0 }, { "highDrive", 12.0 }, { "highCurve", 2.0 }, { "highMix", 30.0 }, { "output", -1.0 } }, nullptr },
              Exciter::apply (s, a, b, rate, exc) },
            { { "maximizer", { { "gain", 6.0 }, { "ceiling", -1.0 }, { "character", 2.0 }, { "emphasis", 50.0 } }, nullptr },
              Maximizer::apply (s, a, b, rate, maxi) },
            { { "chorus", { { "rate", 0.8 }, { "delay", 12.0 }, { "depth", 4.0 }, { "feedback", 0.1 }, { "mix", 40.0 }, { "spread", 30.0 } }, nullptr },
              Modulation::sweptDelay (s, a, b, rate, chorus) },
            { { "phaser", { { "rate", 0.5 }, { "low", 300.0 }, { "high", 3000.0 }, { "stages", 2.0 }, { "feedback", 0.3 }, { "mix", 50.0 }, { "spread", 20.0 } }, nullptr },
              Modulation::phaser (s, a, b, rate, ph) },
            { { "reverb", { { "preDelay", 20.0 }, { "decay", 1.8 }, { "size", 60.0 }, { "damping", 40.0 }, { "width", 80.0 }, { "mix", 30.0 } }, nullptr },
              RoomReverb::apply (s, a, b, rate, rev) },
            { { "convertRate", { { "rate", 44100.0 } }, nullptr }, SampleRateConverter::convert (s, rate, 44100.0) },
            { { "stretch", { { "percent", 120.0 } }, nullptr }, TimeStretch::stretchRange (s, a, b, 1.2, rate) },
            { { "pitch", { { "semitones", -3.0 } }, nullptr }, TimeStretch::shiftRange (s, a, b, -3.0, rate) },
            { { "pitchCorrect", { { "key", 2.0 }, { "scale", 1.0 }, { "speed", 40.0 }, { "strength", 80.0 } }, nullptr },
              PitchCorrection::correct (s, a, b, rate, { 2, PitchCorrection::Scale::major, 40.0f, 80.0f }) },
            { { "dereverb", { { "time", 1.5 }, { "reduction", 9.0 } }, nullptr }, Dereverb::reduce (s, a, b, rate, { 1.5f, 9.0f }) },
            { { "levelSpeech", { { "target", -18.0 }, { "boost", 9.0 }, { "cut", 6.0 }, { "silence", -55.0 } }, nullptr },
              Leveller::level (s, a, b, rate, { -18.0f, 9.0f, 6.0f, -55.0f }) },
        };

        juce::StringArray wrong, covered;

        for (auto& c : cases)
        {
            const auto r = Sequences::apply (c.step, audio, a, b, rate, markers, nullptr);
            covered.addIfNotAlreadyThere (c.step.kind);

            if (! r.error.isEmpty() || c.expected == nullptr || ! identical (r.snapshot, c.expected))
                wrong.add (c.step.kind + (r.error.isNotEmpty() ? " (" + r.error + ")" : juce::String()));
        }

        // The ones left: the flanger shares the chorus's code with its own settings; Make Stereo
        // from mono; the rack, below.
        const auto mono = music (1, 4800, rate, 8);
        const auto stereo = Sequences::apply ({ "stereo", {}, nullptr }, mono, 0, 4800, rate, {}, nullptr);
        const Modulation::Delay flanger { 0.3, 2.0, 1.5, 0.6, 50.0, 0.0 };
        const auto flanged = Sequences::apply ({ "flanger", { { "rate", 0.3 }, { "delay", 2.0 }, { "depth", 1.5 }, { "feedback", 0.6 }, { "mix", 50.0 }, { "spread", 0.0 } }, nullptr },
                                               audio, a, b, rate, {}, nullptr);
        covered.add ("stereo");
        covered.add ("flanger");

        if (! identical (stereo.snapshot, AudioEdits::convertChannels (*mono, 2)))
            wrong.add ("stereo");

        if (! identical (flanged.snapshot, Modulation::sweptDelay (s, a, b, rate, flanger)))
            wrong.add ("flanger");

        // Phase correction: on a file whose right channel is its left 13 frames late.
        juce::AudioBuffer<float> pair (2, 48000);
        const auto source = readAll (*music (1, 48000, rate, 9));

        for (int i = 0; i < 48000; ++i)
        {
            pair.setSample (0, i, source.getSample (0, i));
            pair.setSample (1, i, i >= 13 ? source.getSample (0, i - 13) : 0.0f);
        }

        const auto late = AudioEdits::fromBuffer (std::move (pair));
        const auto lined = Sequences::apply ({ "phase", { { "most", 2.0 }, { "turnOver", 1.0 } }, nullptr }, late, a, b, rate, {}, nullptr);
        const auto lateFound = PhaseCorrection::measure (*late, a, b, rate, 2.0);
        covered.add ("phase");

        if (! lateFound.has_value() || ! identical (lined.snapshot, PhaseCorrection::correct (*late, a, b, rate, lateFound->offset, false)))
            wrong.add ("phase");

        covered.add ("rack");
        auto rack = std::shared_ptr<const juce::XmlElement> (compressorRack());
        Sequences::Result racked;
        juce::AudioBuffer<float> direct (2, (int) (b - a));
        s.read (direct, 0, a, direct.getNumSamples());
        juce::String error;
        offThread ([&]
        {
            racked = Sequences::apply ({ "rack", {}, rack }, audio, a, b, rate, {}, &engine.pluginFormatManager);
            error = OfflineRender::processThroughRack (*rack, engine.pluginFormatManager, rate, direct, {});
        });
        AudioClip clip { 2, rate, AudioEdits::fromBuffer (std::move (direct))->getSegments() };

        if (! error.isEmpty() || ! identical (racked.snapshot, AudioEdits::replaceWithClip (s, a, b, clip)))
            wrong.add ("rack" + racked.error);

        juce::StringArray missing;

        for (auto& k : Sequences::kinds())
            if (! covered.contains (k))
                missing.add (k);

        check (wrong.isEmpty() && missing.isEmpty() && Sequences::kinds().size() == 38,
               "sequences: each of the 38 steps gives its command's own result bit for bit"
                   + (wrong.isEmpty() ? juce::String() : " (differs: " + wrong.joinIntoString (", ") + ")")
                   + (missing.isEmpty() ? juce::String() : " (not tried: " + missing.joinIntoString (", ") + ")"));
    }

    // Steps with nothing to do leave the audio; ones that can't run say why.
    {
        const auto none = [&] (const Step& step) { const auto r = Sequences::apply (step, audio, a, b, rate, markers, nullptr); return r.snapshot == nullptr && r.error.isEmpty(); };
        const auto refused = [&] (const Step& step, double at) { const auto r = Sequences::apply (step, audio, a, b, at, markers, nullptr); return r.snapshot == nullptr && r.error.isNotEmpty(); };
        juce::AudioBuffer<float> zeros (2, 4800);
        zeros.clear();
        const auto silent = AudioEdits::fromBuffer (std::move (zeros));
        const auto quiet = Sequences::apply ({ "normalize", { { "db", 0.0 }, { "apart", 0.0 } }, nullptr }, silent, 0, 4800, rate, {}, nullptr);

        check (none ({ "balance", { { "pan", 0.0 } }, nullptr }) && none ({ "stereoWidth", { { "width", 100.0 } }, nullptr })
                   && none ({ "echo", { { "delay", 100.0 }, { "feedback", 0.0 } }, nullptr }) && none ({ "tremolo", { { "rate", 2.0 }, { "depth", 0.0 } }, nullptr })
                   && none ({ "stereo", {}, nullptr }) && none ({ "convertRate", { { "rate", rate } }, nullptr })
                   && quiet.snapshot == nullptr && quiet.notes.size() == 1
                   && refused ({ "chorus2", {}, nullptr }, rate) && refused ({ "gain", {}, nullptr }, rate)
                   && refused ({ "compressor", { { "threshold", -24.0 }, { "ratio", 0.5 }, { "knee", 6.0 }, { "attack", 10.0 }, { "release", 120.0 }, { "makeup", 0.0 } }, nullptr }, rate)
                   && refused ({ "eqBand", { { "type", 0.0 }, { "frequency", 15000.0 }, { "gain", 3.0 }, { "q", 1.0 } }, nullptr }, 22050.0)
                   && refused ({ "rack", {}, nullptr }, rate),
               "sequences: a step with nothing to do leaves the audio, a silent one says so, and an unknown step, a missing or out-of-range "
               "setting (an EQ band above half a 22.05 kHz rate) or a rack step without a rack is refused");
    }

    // A whole sequence on a part: the steps one after another, through a rate change.
    {
        Sequences::Sequence seq { "Master", { { "gain", { { "db", 2.0 } }, nullptr },
                                              { "convertRate", { { "rate", 44100.0 } }, nullptr },
                                              { "compressor", { { "threshold", -24.0 }, { "ratio", 3.0 }, { "knee", 6.0 }, { "attack", 10.0 }, { "release", 120.0 }, { "makeup", 0.0 } }, nullptr },
                                              { "normalize", { { "db", -1.0 }, { "apart", 0.0 } }, nullptr } } };
        const auto r = Sequences::run (seq, audio, a, b, rate, markers, nullptr);

        auto step = AudioEdits::gain (s, a, b, 2.0f);
        step = SampleRateConverter::convert (*step, rate, 44100.0);
        const auto a2 = (juce::int64) std::llround (a * 44100.0 / rate), b2 = (juce::int64) std::llround (b * 44100.0 / rate);
        step = Dynamics::compress (*step, a2, b2, 44100.0, { -24.0, 3.0, 6.0, 10.0, 120.0, 0.0 });
        step = AudioEdits::normalize (*step, a2, b2, -1.0f);

        check (r.error.isEmpty() && identical (r.snapshot, step) && r.sampleRate == 44100.0 && r.start == a2 && r.end == b2
                   && r.markers == SampleRateConverter::convert (markers, rate, 44100.0),
               "sequences: four steps on a part of the file (gain, 44.1 kHz, compressor, normalize) give the commands' results one after "
               "another, bit for bit, with the part and the markers moved by the rate change");

        int calls = 0;
        const auto stopped = Sequences::run (seq, audio, a, b, rate, markers, nullptr, [&calls] (float) { return ++calls < 3; });
        Sequences::Sequence bad { "Bad", { { "gain", { { "db", 1.0 } }, nullptr }, { "eqBand", { { "type", 0.0 }, { "frequency", 30000.0 }, { "gain", 1.0 }, { "q", 1.0 } }, nullptr } } };
        const auto refused = Sequences::run (bad, audio, a, b, rate, markers, nullptr);
        check (stopped.error == "Stopped." && stopped.snapshot == audio && refused.error == "EQ Band: a setting is out of range." && refused.snapshot == audio,
               "sequences: stopping ends the run, and a step that can't run stops it with its name and leaves the audio as it was");
    }

    // Meet Delivery Spec as a step: named after its spec, refused out of range, and its result in the notes.
    {
        const Step r128 { "deliverySpec", { { "target", -23.0 }, { "tolerance", 0.5 }, { "ceiling", -1.0 } }, nullptr };
        const Step own { "deliverySpec", { { "target", -20.0 }, { "tolerance", 1.0 }, { "ceiling", -2.0 } }, nullptr };
        const Step wide { "deliverySpec", { { "target", -23.0 }, { "tolerance", 12.0 }, { "ceiling", -1.0 } }, nullptr };
        const auto ran = Sequences::run ({ "Deliver", { r128 } }, audio, a, b, rate, markers, nullptr);
        const auto after = Loudness::measure (*ran.snapshot, a, b, rate);
        juce::AudioBuffer<float> zeros (2, 4800);
        zeros.clear();
        const auto silent = Sequences::apply (r128, AudioEdits::fromBuffer (std::move (zeros)), 0, 4800, rate, {}, nullptr);
        check (r128.name() == "Meet EBU R128" && own.name() == "Meet Delivery spec -20.0 LUFS" && Sequences::problem (r128, rate).isEmpty()
                   && Sequences::problem (wide, rate).isNotEmpty() && ran.error.isEmpty() && std::abs (after->integrated + 23.0) < 0.06
                   && ran.notes.size() == 1 && ran.notes[0].startsWith ("Meet EBU R128: meets it at ")
                   && silent.snapshot == nullptr && silent.error.isEmpty() && silent.notes[0].contains ("-70 LUFS"),
               "sequences: the delivery spec step is named after its spec (or its target), refuses a tolerance past 10 LU, and says what "
               "the result measures: " + ran.notes.joinIntoString ("; ") + "; on silence it leaves the audio, with a note");
    }

    // Saving and loading, and the store.
    {
        Sequences::Sequence seq { "Voice / clean", { { "gain", { { "db", -0.1 } }, nullptr },
                                                     { "limiter", { { "gain", 1.0 / 3.0 }, { "ceiling", -1.0 }, { "release", 50.0 }, { "lookahead", 5.0 }, { "peaks", 1.0 } }, nullptr },
                                                     { "rack", {}, std::shared_ptr<const juce::XmlElement> (compressorRack()) } } };
        const auto xml = Sequences::toXml (seq);
        const auto text = xml->toString();
        const auto back = Sequences::fromXml (*juce::XmlDocument::parse (text));
        auto unknown = juce::XmlDocument::parse (text);
        unknown->getChildByName ("STEP")->setAttribute ("kind", "warp");

        check (back.has_value() && back->name == seq.name && back->steps.size() == 3 && back->steps[1].values == seq.steps[1].values
                   && back->steps[0].values == seq.steps[0].values && back->steps[2].rack != nullptr
                   && back->steps[2].rack->isEquivalentTo (seq.steps[2].rack.get(), false) && Sequences::toXml (*back)->toString() == text
                   && ! Sequences::fromXml (*unknown).has_value() && back->steps[1].settingsText().startsWith ("gain 0.333, ceiling -1")
                   && back->steps[0].name() == "Gain -0.10 dB" && back->steps[2].settingsText() == "1 effect",
               "sequences: saved and loaded, every setting comes back exactly (1/3 too) with the rack's plug-in and state; a step this "
               "version doesn't know refuses the file");

        auto folder = juce::File::createTempFile ("sequences");
        Sequences::Store store (folder);
        Sequences::Sequence other { "another", { { "invert", {}, nullptr } } };
        const auto saved = store.save (seq) && store.save (other);
        const auto names = store.names();
        seq.steps.pop_back();
        store.save (seq);
        const auto replaced = store.load ("Voice / clean");
        const auto removed = store.remove ("another") && ! store.contains ("another");
        check (saved && names.joinIntoString ("|") == "another|Voice / clean" && replaced.has_value() && replaced->steps.size() == 2
                   && removed && store.names().joinIntoString ("|") == "Voice / clean" && ! store.save ({ " ", {} }),
               "sequences: the store keeps sequences by name (a / in one too), lists them in order, replaces one saved again under its "
               "name, and removes one; a sequence needs a name");
        folder.deleteRecursively();
    }

    // The recorder.
    {
        Sequences::Recorder r;
        r.add ({ "invert", {}, nullptr });
        r.start();
        r.add ({ "gain", { { "db", 1.0 } }, nullptr });
        r.skip ("Fade In");
        r.add ({ "reverse", {}, nullptr });
        const auto skipped = r.skipped();
        const auto seq = r.stop();
        r.add ({ "invert", {}, nullptr });
        check (seq.steps.size() == 2 && seq.steps[0].kind == "gain" && seq.steps[1].kind == "reverse" && skipped.joinIntoString ("|") == "Fade In"
                   && ! r.isRecording() && r.steps().size() == 2,
               "sequences: the recorder keeps the steps made while it records, in order, and the edits it couldn't record");
    }

    // A sequence in a batch: the file as the sequence plays it on the whole file.
    {
        auto dir = juce::File::createTempFile ("seqbatch");
        dir.createDirectory();
        const auto source = dir.getChildFile ("Take.wav");
        AudioFileIO::save (*audio, rate, source, 32, engine.formatManager, markers);
        auto seq = std::make_shared<Sequences::Sequence> (Sequences::Sequence { "Batch", { { "removeDc", {}, nullptr },
                                                                                           { "convertRate", { { "rate", 44100.0 } }, nullptr },
                                                                                           { "normalize", { { "db", -2.0 }, { "apart", 0.0 } }, nullptr } } });
        Batch::Settings settings;
        settings.sequence = seq;
        settings.bitDepth = 32;
        settings.suffix = " done";
        Batch::Outcome outcome;
        offThread ([&] { outcome = Batch::process (source, settings, engine.formatManager, nullptr, &engine.pluginFormatManager); });
        const auto written = AudioFileIO::load (outcome.written, engine.formatManager);
        const auto loaded = AudioFileIO::load (source, engine.formatManager);
        const auto expected = Sequences::run (*seq, loaded.snapshot, 0, loaded.snapshot->getNumFrames(), rate, loaded.markers, nullptr);
        check (outcome.error.isEmpty() && written.sampleRate == 44100.0 && identical (written.snapshot, expected.snapshot)
                   && written.markers == expected.markers && written.markers == SampleRateConverter::convert (markers, rate, 44100.0),
               "sequences: in a batch, a sequence (remove DC, 44.1 kHz, normalize) gives what it gives on the file, bit for bit, at 44.1 kHz "
               "with the markers moved");

        settings.sequence = std::make_shared<Sequences::Sequence> (Sequences::Sequence { "Bad", { { "eqBand", { { "type", 0.0 }, { "frequency", 30000.0 }, { "gain", 1.0 }, { "q", 1.0 } }, nullptr } } });
        offThread ([&] { outcome = Batch::process (source, settings, engine.formatManager, nullptr, &engine.pluginFormatManager); });
        check (outcome.written == juce::File() && outcome.error.startsWith ("The sequence: EQ Band: "),
               "sequences: in a batch, a step that can't run on a file leaves that file unwritten and says why");
        dir.deleteRecursively();
    }
}
