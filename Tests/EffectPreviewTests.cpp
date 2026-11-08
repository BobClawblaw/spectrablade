// Rendered previews are compared bit for bit with the effect applied on purpose.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/AudioEdits.h"
#include "../Source/BuiltInEffects.h"
#include "../Source/EffectDialog.h"
#include "../Source/EffectPreview.h"
#include "../Source/EffectPresets.h"
#include "../Source/Generator.h"
#include "TestUtils.h"
#include "EffectPreviewTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    AudioDocument::Snapshot tone (double seconds, float level = 0.5f)
    {
        juce::AudioBuffer<float> b (2, (int) (seconds * rate));

        for (int i = 0; i < b.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch)
                b.setSample (ch, i, level * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / rate));

        return AudioEdits::fromBuffer (std::move (b));
    }

    bool same (const AudioSnapshot& a, const AudioSnapshot& b)
    {
        if (a.getNumFrames() != b.getNumFrames() || a.getNumChannels() != b.getNumChannels())
            return false;

        juce::AudioBuffer<float> x (a.getNumChannels(), (int) a.getNumFrames()), y (b.getNumChannels(), (int) b.getNumFrames());
        a.read (x, 0, 0, x.getNumSamples());
        b.read (y, 0, 0, y.getNumSamples());

        for (int ch = 0; ch < x.getNumChannels(); ++ch)
            for (int i = 0; i < x.getNumSamples(); ++i)
                if (x.getSample (ch, i) != y.getSample (ch, i))
                    return false;

        return true;
    }

    void testRequests()
    {
        const auto r = juce::Range<juce::int64> (1000, 1000 + (juce::int64) (60.0 * rate));
        check (EffectPreview::partOf (r, rate, false) == r.withLength ((juce::int64) (30.0 * rate))
                   && EffectPreview::partOf (r, rate, true) == r
                   && EffectPreview::partOf ({ 5, 500 }, rate, false) == juce::Range<juce::int64> (5, 500),
               "a preview processes the range's first 30 s, or all of it when it needs it all");

        const auto a = EffectPreview::ofStep ({ "echo", { { "delay", 250.0 }, { "feedback", 40.0 } }, nullptr }, rate);
        const auto b = EffectPreview::ofStep ({ "echo", { { "delay", 250.0 }, { "feedback", 40.0 } }, nullptr }, rate);
        const auto c = EffectPreview::ofStep ({ "echo", { { "delay", 260.0 }, { "feedback", 40.0 } }, nullptr }, rate);
        check (a.has_value() && b.has_value() && c.has_value() && a->key == b->key && a->key != c->key,
               "the same settings make the same preview key, others another");
        check (! EffectPreview::ofStep ({ "echo", { { "delay", 0.0 }, { "feedback", 40.0 } }, nullptr }, rate).has_value()
                   && ! EffectPreview::ofStep ({ "stretch", { { "percent", 300.0 } }, nullptr }, rate).has_value(),
               "settings a step can't run with make no preview");
    }

    void testRendering()
    {
        const auto audio = tone (2.0);
        const Sequences::Step step { "gain", { { "db", -6.0 } }, nullptr };
        EffectPreview preview;
        std::optional<EffectPreview::Rendered> got;

        preview.render (*EffectPreview::ofStep (step, rate), audio, { 4800, 52800 }, rate, [&] (const EffectPreview::Rendered& r) { got = r; });
        check (preview.isBusy(), "a preview is busy until its result comes");
        TestUtils::runUntil ([&] { return got.has_value(); }, 10000);

        const auto applied = Sequences::apply (step, audio, 4800, 52800, rate, {}, nullptr);
        check (got.has_value() && got->snapshot != nullptr && same (*got->snapshot, *applied.snapshot) && got->start == 4800 && got->end == 52800
                   && ! preview.isBusy(),
               "a rendered preview is the step applied to the range, bit for bit");

        // A stretch moves the end of the range it processed.
        got.reset();
        preview.render (*EffectPreview::ofStep ({ "stretch", { { "percent", 150.0 } }, nullptr }, rate), audio, { 0, 48000 }, rate,
                        [&] (const EffectPreview::Rendered& r) { got = r; });
        TestUtils::runUntil ([&] { return got.has_value(); }, 30000);
        check (got.has_value() && got->error.isEmpty() && got->start == 0 && std::abs (got->end - 72000) <= 1,
               "a stretched preview says where its range ends now");

        // Only the newest request is answered; one under way stops at its next progress check.
        std::atomic<bool> firstStopped { false };
        int firstAnswers = 0, secondAnswers = 0;
        EffectPreview::Request slow { "slow", [&] (const AudioDocument::Snapshot&, juce::int64 s, juce::int64 e, double, const std::function<bool (float)>& progress)
        {
            for (int i = 0; i < 1000; ++i)
            {
                if (! progress ((float) i / 1000.0f))
                {
                    firstStopped = true;
                    return EffectPreview::Rendered { nullptr, s, e, "Stopped." };
                }

                juce::Thread::sleep (2);
            }

            return EffectPreview::Rendered { nullptr, s, e, {} };
        } };

        preview.render (slow, audio, { 0, 100 }, rate, [&] (const EffectPreview::Rendered&) { ++firstAnswers; });
        juce::Thread::sleep (20);
        preview.render (*EffectPreview::ofStep (step, rate), audio, { 0, 100 }, rate, [&] (const EffectPreview::Rendered&) { ++secondAnswers; });
        TestUtils::runUntil ([&] { return secondAnswers > 0; }, 10000);
        TestUtils::runUntil ([] { return false; }, 100);
        check (firstStopped && firstAnswers == 0 && secondAnswers == 1, "a newer request stops the one under way, and only it is answered");

        preview.render (slow, audio, { 0, 100 }, rate, [&] (const EffectPreview::Rendered&) { ++firstAnswers; });
        preview.cancel();
        TestUtils::runUntil ([] { return false; }, 200);
        check (firstAnswers == 0 && ! preview.isBusy(), "a cancelled request isn't answered");
    }

    // A generator's preview is what Generate puts in, at the same place, bit for bit (the noise with
    // the same seed); a long one plays only its first 30 s; the place is kept within the audio.
    void testGenerated()
    {
        const auto audio = tone (2.0);
        EffectPreview preview;
        std::optional<EffectPreview::Rendered> got;
        const auto run = [&] (const EffectPreview::Request& request)
        {
            got.reset();
            preview.render (request, audio, { 0, audio->getNumFrames() }, rate, [&] (const EffectPreview::Rendered& r) { got = r; });
            TestUtils::runUntil ([&] { return got.has_value(); }, 30000);
        };

        Generator::Noise n;
        n.colour = Generator::Colour::pink;
        n.levelDb = -30.0;
        n.seed = 1234;
        const auto noise = Generator::noise (n, 2, 24000, rate);
        run (EffectPreview::generated ("noise", 12000, [n] { return Generator::noise (n, 2, 24000, rate); }));
        const auto inserted = Generator::insert (*audio, 12000, noise);
        check (got.has_value() && got->error.isEmpty() && got->snapshot != nullptr && same (*got->snapshot, *inserted)
                   && got->start == 12000 && got->end == 36000 && got->note.isEmpty(),
               "a generated preview is the noise put in at the cursor, bit for bit, playing where it went");

        Generator::Tone t;
        t.frequency = 1000.0;
        t.endFrequency = 1000.0;
        t.levelDb = -12.0;
        constexpr int longFrames = (int) (45.0 * rate);
        run (EffectPreview::generated ("tone", 999999999, [t] { return Generator::tone (t, 2, longFrames, rate); }));
        const auto first = (juce::int64) (30.0 * rate);
        check (got.has_value() && got->snapshot != nullptr && got->start == audio->getNumFrames() && got->end == got->start + first
                   && got->snapshot->getNumFrames() == audio->getNumFrames() + first && got->note.contains ("first 30 s"),
               "a generated preview past the end goes in at the end, and of 45 s only the first 30 s play (it says so)");

        const auto a = EffectPreview::generated ("tone a=1", 0, {}), b = EffectPreview::generated ("tone a=1", 0, {});
        check (a.generates && a.key == b.key && ! a.wholeRange, "a generated preview is marked as one, keyed by its settings");
    }

    void testStandIn()
    {
        AudioDocument doc;
        doc.reset (tone (1.0), rate, {}, 24);
        DocumentSource source (doc);
        source.prepareToPlay (512, rate);
        juce::AudioBuffer<float> out (2, 512);
        const auto play = [&] (juce::int64 from)
        {
            out.clear();
            source.setNextReadPosition (from);
            source.getNextAudioBlock (juce::AudioSourceChannelInfo (out));
            return out.getMagnitude (0, 0, 512);
        };

        const auto plain = play (1000);
        source.setStandIn (tone (2.0, 0.25f));
        const auto standIn = play (1000);
        const auto past = play (60000);   // past the document's end, inside the stand-in's
        source.setStandIn (nullptr);
        const auto back = play (1000);

        check (std::abs (plain - 0.5f) < 0.01f && std::abs (standIn - 0.25f) < 0.01f && past > 0.2f && std::abs (back - 0.5f) < 0.01f
                   && ! source.hasStandIn(),
               "a stand-in plays in place of the document (past its end too) until taken away");
    }

    void testBypass (AudioEngine& engine)
    {
        engine.setPreviewEffect (std::make_unique<BuiltInEffects::CompressorProcessor>());
        engine.setPreviewBypassed (true);
        const auto bypassed = engine.isPreviewBypassed();
        engine.setPreviewBypassed (false);
        const auto back = ! engine.isPreviewBypassed();
        engine.setPreviewEffect (nullptr);
        check (bypassed && back && ! engine.isPreviewBypassed(), "the live preview effect can be bypassed and put back");
    }

    void testDialog()
    {
        const EffectDialog::Field gain { "gain", "Gain (dB)", 3.2, 1, {}, -48.0, 48.0, 0.5 };
        check (EffectDialog::nudged (gain, 3.2, 1.0, false) == 3.5 && EffectDialog::nudged (gain, 3.5, -2.0, false) == 2.5
                   && EffectDialog::nudged (gain, 3.5, 1.0, true) == 3.55 && EffectDialog::nudged (gain, 47.8, 3.0, false) == 48.0,
               "a wheel notch moves a number a step onto the step's grid, Shift a tenth of one, within its range");

        EffectDialog::Options o;
        o.fields = { gain, { "mode", "Mode", 1.0, 0, { "One", "Two", "Three" } } };
        EffectDialog dialog (o);
        int changes = 0, settled = 0, applied = 0;
        std::vector<bool> previews;
        EffectDialog::Values appliedValues;
        dialog.onChange = [&] (const EffectDialog::Values&) { ++changes; };
        dialog.onSettled = [&] (const EffectDialog::Values&) { ++settled; };
        dialog.onPreview = [&] (bool on) { previews.push_back (on); };
        dialog.onApply = [&] (const EffectDialog::Values& v) { ++applied; appliedValues = v; };

        auto* slider = dialog.getSlider ("gain");
        auto* menu = dialog.getMenu ("mode");
        const juce::MouseEvent e (juce::Desktop::getInstance().getMainMouseSource(), {}, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                  slider, slider, juce::Time::getCurrentTime(), {}, juce::Time::getCurrentTime(), 1, false);
        juce::MouseWheelDetails notch { 0.0f, 0.1f, false, false, false };
        slider->mouseWheelMove (e, notch);
        const auto afterWheel = dialog.getValues().at ("gain");
        notch.deltaY = -0.1f;
        slider->mouseWheelMove (e, notch);
        slider->mouseWheelMove (e, notch);
        const auto afterDown = dialog.getValues().at ("gain");
        slider->keyPressed (juce::KeyPress (juce::KeyPress::upKey));
        const auto afterKey = dialog.getValues().at ("gain");

        juce::MouseWheelDetails down { 0.0f, -0.1f, false, false, false };
        menu->mouseWheelMove (e.getEventRelativeTo (menu), down);
        const auto afterMenu = dialog.getValues().at ("mode");

        check (afterWheel == 3.5 && afterDown == 2.5 && afterKey == 3.0 && afterMenu == 2.0 && changes == 5,
               "the wheel and arrow keys step a number, the wheel a menu, each telling of the change");

        TestUtils::runUntil ([&] { return settled > 0; }, 2000);
        check (settled == 1, "the settings settle once they stop changing");

        check (! slider->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey)) && ! menu->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey)),
               "Space passes by the fields to the dialog");

        dialog.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
        const auto bypassable = dialog.isPreviewOn();
        dialog.setBypassed (true);
        const auto bypassed = dialog.isBypassed();
        dialog.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
        check (previews == std::vector<bool> { true, false } && bypassable && bypassed && ! dialog.isBypassed(),
               "Space starts and stops the preview; stopping it ends Bypass");

        dialog.setValues ({ { "gain", -12.0 }, { "other", 1.0 } });
        dialog.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        check (applied == 1 && appliedValues.at ("gain") == -12.0 && appliedValues.at ("mode") == 2.0, "Return applies the settings shown (a preset's too)");
    }
}

namespace
{
    void testTextFields()
    {
        EffectDialog::Options o;
        o.fields = { EffectDialog::Field::textField ("digits", "Digits", "555", "0123456789,", 8), { "level", "Level", -6.0, 1, {}, -96.0, 0.0, 0.5 } };
        EffectDialog dialog (o);
        auto* text = dialog.getTextField ("digits");
        text->clear();
        text->insertTextAtCaret ("12,34 5");
        const auto passes = ! text->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey)) && ! text->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        check (dialog.getText ("digits") == "12,345" && dialog.getTexts().at ("digits") == "12,345" && dialog.getValues().count ("digits") == 0
                   && dialog.getValues().at ("level") == -6.0 && passes,
               "a text field takes only its characters, stays out of the values, and leaves Space and Return to the dialog");
    }

    /** The step a dialog's preset runs as, for the effects that are steps (the phase dialog names its polarity otherwise). */
    std::optional<Sequences::Step> stepOf (const juce::String& key, const EffectPresets::Values& v)
    {
        static const std::map<juce::String, juce::String> kinds {
            { "spectrablade:gain", "gain" }, { "spectrablade:normalize", "normalize" }, { "spectrablade:match-loudness", "matchLoudness" },
            { "spectrablade:level-speech", "levelSpeech" }, { "spectrablade:balance", "balance" }, { "spectrablade:stereo-width", "stereoWidth" },
            { "spectrablade:centre-channel", "centre" }, { "spectrablade:reduce-reverb", "dereverb" }, { "spectrablade:stretch", "stretch" },
            { "spectrablade:shift-pitch", "pitch" }, { "spectrablade:pitch-correction", "pitchCorrect" }, { "spectrablade:eq-band", "eqBand" },
            { "spectrablade:compressor", "compressor" }, { "spectrablade:limiter", "limiter" }, { "spectrablade:de-esser", "deEsser" },
            { "spectrablade:chorus", "chorus" }, { "spectrablade:flanger", "flanger" }, { "spectrablade:phaser", "phaser" },
            { "spectrablade:reverb", "reverb" }, { "spectrablade:tremolo", "tremolo" }, { "spectrablade:echo", "echo" } };

        if (key == "spectrablade:phase-correction")
            return Sequences::Step { "phase", { { "most", v.at ("most") }, { "turnOver", v.at ("polarity") < 0.5 ? 1.0 : 0.0 } }, nullptr };

        const auto found = kinds.find (key);
        return found != kinds.end() ? std::optional<Sequences::Step> (Sequences::Step { found->second, v, nullptr }) : std::nullopt;
    }

    std::unique_ptr<BuiltInEffects::BuiltInEffect> liveEffectOf (const juce::String& key)
    {
        using namespace BuiltInEffects;

        if (key == parametricEqId)  return std::make_unique<ParametricEqProcessor>();
        if (key == compressorId)    return std::make_unique<CompressorProcessor>();
        if (key == limiterId)       return std::make_unique<LimiterProcessor>();
        if (key == chorusId)        return std::make_unique<SweptDelayProcessor> (SweptDelayProcessor::Kind::chorus);
        if (key == flangerId)       return std::make_unique<SweptDelayProcessor> (SweptDelayProcessor::Kind::flanger);
        if (key == phaserId)        return std::make_unique<PhaserProcessor>();
        if (key == reverbId)        return std::make_unique<ReverbProcessor>();
        if (key == convolutionId)   return std::make_unique<ConvolutionProcessor>();
        if (key == graphicEq10Id)   return std::make_unique<GraphicEqProcessor> (10);
        if (key == graphicEq31Id)   return std::make_unique<GraphicEqProcessor> (31);
        if (key == multibandId)     return std::make_unique<MultibandProcessor>();
        if (key == deEsserId)       return std::make_unique<DeEsserProcessor>();
        if (key == dynamicEqId)     return std::make_unique<DynamicEqProcessor>();
        if (key == multibandWidthId) return std::make_unique<MultibandWidthProcessor>();
        if (key == exciterId)       return std::make_unique<ExciterProcessor>();
        if (key == maximizerId)     return std::make_unique<MaximizerProcessor>();
        return nullptr;
    }

    void testFactoryPresets()
    {
        auto presets = 0, steps = 0, live = 0;
        juce::StringArray badSteps, badLive, repeated;

        for (auto& key : EffectPresets::factoryKeys())
        {
            juce::StringArray names;

            for (auto& p : EffectPresets::factory (key))
            {
                ++presets;

                if (names.contains (p.name))
                    repeated.add (key + " " + p.name);

                names.add (p.name);

                if (const auto step = stepOf (key, p.values))
                {
                    ++steps;

                    if (const auto problem = Sequences::problem (*step, rate); problem.isNotEmpty())
                        badSteps.add (key + " " + p.name + ": " + problem);
                }

                // A live effect's preset sets every one of its parameters, each within its range (but the
                // compressor's side-chain switch, which is routing, left as it is).
                if (auto effect = liveEffectOf (key))
                {
                    ++live;

                    for (auto* param : effect->getParameters())
                    {
                        auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (param);

                        if (withId != nullptr && withId->paramID == "sidechain")
                            continue;
                        const auto found = withId != nullptr ? p.values.find (withId->paramID) : p.values.end();
                        auto ok = found != p.values.end();

                        if (ok)
                        {
                            if (auto* f = dynamic_cast<juce::AudioParameterFloat*> (param))
                                ok = f->range.getRange().contains ((float) found->second) || juce::exactlyEqual ((float) found->second, f->range.end);
                            else if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (param))
                                ok = juce::isPositiveAndBelow ((int) found->second, c->choices.size());
                            else
                                ok = juce::exactlyEqual (found->second, 0.0) || juce::exactlyEqual (found->second, 1.0);
                        }

                        if (! ok)
                            badLive.add (key + " " + p.name + ": " + (withId != nullptr ? withId->paramID : juce::String ("?")));
                    }
                }
            }
        }

        std::cout << "  " << presets << " factory presets, " << steps << " checked as steps, " << live << " against live effects" << std::endl;

        for (auto& b : badSteps)
            std::cout << "  step refused: " << b << std::endl;

        for (auto& b : badLive)
            std::cout << "  live parameter missing or out of range: " << b << std::endl;

        check (presets > 150 && steps > 80 && live > 50 && badSteps.isEmpty() && badLive.isEmpty() && repeated.isEmpty(),
               "every factory preset is a setting its effect takes: as a step, and for a live effect every parameter but the side-chain switch set within its range; no name repeated");

        // The presets bar: factory presets first, under their heading, chosen ones applied; only the user's can be deleted.
        const auto folder = juce::File::createTempFile ("presets");
        EffectPresets::Store store (folder);
        store.save ("spectrablade:echo", "Mine", { { "delay", 333.0 }, { "feedback", 20.0 } });
        EffectPresets::Values applied;
        EffectPresets::Bar bar (store, "spectrablade:echo", [] { return EffectPresets::Values {}; }, [&] (const EffectPresets::Values& v) { applied = v; });
        auto* list = dynamic_cast<juce::ComboBox*> (bar.getChildComponent (0));
        auto* remove = dynamic_cast<juce::Button*> (bar.getChildComponent (2));
        const auto& echo = EffectPresets::factory ("spectrablade:echo");
        const auto factoryCount = (int) echo.size();

        list->setSelectedId (1, juce::sendNotificationSync);
        const auto firstApplied = applied == echo[0].values && ! remove->isEnabled();
        list->setSelectedId (1001, juce::sendNotificationSync);
        const auto mineApplied = applied.at ("delay") == 333.0 && remove->isEnabled();

        check (list->getNumItems() == factoryCount + 1 && list->getItemText (0) == echo[0].name && list->getItemText (factoryCount) == "Mine"
                   && firstApplied && mineApplied,
               "the presets menu lists the factory presets first, then the user's; choosing one applies it, and only the user's can be deleted");
        folder.deleteRecursively();
    }
}

void runEffectPreviewTests (AudioEngine& engine)
{
    testTextFields();
    testFactoryPresets();
    testRequests();
    testRendering();
    testGenerated();
    testStandIn();
    testBypass (engine);
    testDialog();
}
