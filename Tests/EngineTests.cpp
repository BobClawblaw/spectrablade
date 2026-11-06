// Integration checks against the real machine: document editing and file I/O, the plug-in scanner
// worker, the audio device (including multi-output interfaces) and rack save/restore with real plug-ins.
// The player is never started, so nothing is heard.

#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/PluginScanner.h"
#include "../Source/AudioEdits.h"
#include "../Source/PluginMenu.h"
#include "../Source/RackView.h"
#include "DocumentTests.h"
#include "RecorderTests.h"
#include "MarkerTests.h"
#include "ConverterTests.h"
#include "SpectralTests.h"
#include "LoudnessTests.h"
#include "EqTests.h"
#include "DynamicsTests.h"
#include "ModulationTests.h"
#include "ReverbTests.h"
#include "ConvolverTests.h"
#include "EffectPresetsTests.h"
#include "GraphicEqTests.h"
#include "DockTests.h"
#include "OpenFilesTests.h"
#include "PanelTests.h"
#include "ThemeTests.h"
#include "EditorBarTests.h"
#include "FadeHandleTests.h"
#include "MultibandTests.h"
#include "BuiltInEffectsTests.h"
#include "NoiseTests.h"
#include "ClickTests.h"
#include "SessionTests.h"
#include "TrackEffectsTests.h"
#include "BusTests.h"
#include "AutomationTests.h"
#include "MultitrackViewTests.h"
#include "MixDownTests.h"
#include "EffectAutomationTests.h"
#include "TrackRecordingTests.h"
#include "MetronomeTests.h"
#include "MixerTests.h"
#include "BounceTests.h"
#include "AuditionTests.h"
#include "PluginTests.h"
#include "TestUtils.h"

namespace
{
    using TestUtils::check;
    using TestUtils::runUntil;

    juce::AudioPluginFormat* findFormat (AudioEngine& engine, const juce::String& name)
    {
        for (auto* f : engine.pluginFormatManager.getFormats())
            if (f->getName() == name)
                return f;

        return nullptr;
    }

    void testScanner (AudioEngine& engine, juce::Array<juce::PluginDescription>& found)
    {
        // The built app is the worker, exactly as in production.
        OutOfProcessScanner scanner (TestUtils::appExecutable);

        if (auto* au = findFormat (engine, "AudioUnit"))
        {
            // Apple's AUDelay ships with macOS: type aufx, subtype dely, manufacturer appl.
            juce::OwnedArray<juce::PluginDescription> results;
            const auto ok = scanner.findPluginTypesFor (*au, results, "AudioUnit:Effects/aufx,dely,appl");
            check (ok && results.size() == 1, "out-of-process scan finds Apple AUDelay (" + juce::String (results.size()) + ")");

            for (auto* d : results)
                found.add (*d);
        }

        if (auto* vst3 = findFormat (engine, "VST3"))
        {
            juce::StringArray candidates;

            for (auto& dir : { juce::File ("/Library/Audio/Plug-Ins/VST3"),
                               juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Audio/Plug-Ins/VST3") })
                for (auto& f : dir.findChildFiles (juce::File::findDirectories, false, "*.vst3"))
                    candidates.add (f.getFullPathName());

            candidates.sort (true);
            int scanned = 0, withTypes = 0;

            for (auto& path : candidates)
            {
                if (scanned == 8)
                    break;

                juce::OwnedArray<juce::PluginDescription> results;
                const auto ok = scanner.findPluginTypesFor (*vst3, results, path);
                ++scanned;
                std::cout << "      VST3 " << juce::File (path).getFileName() << ": "
                          << (ok ? juce::String (results.size()) + " type(s)" : juce::String ("worker failed")) << std::endl;

                if (ok && ! results.isEmpty())
                {
                    ++withTypes;

                    for (auto* d : results)
                        if (! d->isInstrument)
                            found.add (*d);
                }
            }

            check (withTypes > 0, "out-of-process scan reads VST3 bundles (" + juce::String (withTypes) + "/" + juce::String (scanned) + ")");
        }

        scanner.scanFinished();
    }

    void testDevice (AudioEngine& engine)
    {
        const auto error = engine.openDevice (nullptr);
        check (error.isEmpty(), "device opens" + (error.isEmpty() ? juce::String() : ": " + error));

        auto* device = engine.deviceManager.getCurrentAudioDevice();

        if (device == nullptr)
            return;

        {
            // The probe at launch asks for the same device the engine opens, off the message thread,
            // and for a saved one that has gone, the default.
            juce::String probed, savedProbed, goneProbed;
            juce::XmlElement saved ("DEVICESETUP"), gone ("DEVICESETUP");
            saved.setAttribute ("audioOutputDeviceName", device->getName());
            gone.setAttribute ("audioOutputDeviceName", "No Such Interface");
            std::thread ([&]
            {
                probed = AudioEngine::probeDevice (nullptr);
                savedProbed = AudioEngine::probeDevice (&saved);
                goneProbed = AudioEngine::probeDevice (&gone);
            }).join();
            check (probed == device->getName() && savedProbed == device->getName() && goneProbed == probed,
                   "device probe: off the message thread it finds the device the engine opens (" + probed + "), the saved one, "
                   "and the default for one that has gone");

            // A setup saved while no device was open names none; it is passed over, so the default opens.
            juce::XmlElement none ("DEVICESETUP");
            none.setAttribute ("deviceType", "");
            none.setAttribute ("audioOutputDeviceName", "");
            none.setAttribute ("audioInputDeviceName", "");
            AudioEngine fresh;
            const auto freshError = fresh.openDevice (&none);
            auto* opened = fresh.deviceManager.getCurrentAudioDevice();
            check (AudioEngine::namingADevice (&none) == nullptr && AudioEngine::namingADevice (&saved) == &saved
                       && freshError.isEmpty() && opened != nullptr && opened->getName() == probed,
                   "device: a saved setup naming no device opens the default (" + (opened != nullptr ? opened->getName() : juce::String ("none")) + ")");
        }

        const auto names = device->getOutputChannelNames();
        std::cout << "      device: " << device->getName() << ", " << names.size() << " outputs, "
                  << device->getCurrentSampleRate() << " Hz, " << device->getCurrentBufferSizeSamples() << " samples" << std::endl;

        if (names.size() >= 10)
        {
            // Route to the fifth pair (channels 9/10) and check those device channels are active.
            engine.setOutputPair (8);
            const auto active = device->getActiveOutputChannels();
            check (engine.getOutputPair() == 8 && active[8] && active[9], "output pair 9/10 selected and active");
        }

        engine.setOutputPair (0);
        const auto active = engine.deviceManager.getCurrentAudioDevice()->getActiveOutputChannels();
        check (engine.getOutputPair() == 0 && active[0] && active[1], "output pair 1/2 selected and active");

        engine.setOutputPair (10000);
        check (engine.getOutputPair() == 0, "out-of-range output pair falls back to 1/2");
    }

    /** Arms the device's first two inputs and records a second while the document plays,
        checking the real device's callback order gives an anchor. */
    void testLiveInput (AudioEngine& engine)
    {
        auto* device = engine.deviceManager.getCurrentAudioDevice();
        const auto names = engine.getDeviceInputChannelNames();
        std::cout << "      inputs: " << names.size() << std::endl;

        if (device == nullptr || names.size() < 2)
            return;

        const auto error = engine.setInput (0, 2);
        device = engine.deviceManager.getCurrentAudioDevice();
        const auto active = device->getActiveInputChannels();
        check (error.isEmpty() && active[0] && active[1] && engine.getRecorder().isInputActive(),
               "input 1/2 armed" + (error.isEmpty() ? juce::String() : ": " + error));

        const auto rate = device->getCurrentSampleRate();
        const auto latency = engine.getRoundTripLatency();
        std::cout << "      round trip " << latency << " frames at " << rate << " Hz, "
                  << device->getCurrentBufferSizeSamples() << "-frame buffer" << std::endl;
        check (latency > 0, "round-trip latency is reported");

        juce::AudioBuffer<float> silence (2, (int) (rate * 3.0));
        silence.clear();
        engine.document.reset (AudioEdits::fromBuffer (std::move (silence)), rate, {}, 24);
        engine.getPlayer().resetSource();

        auto& recorder = engine.getRecorder();
        check (recorder.start(), "recorder starts on the live device");
        engine.getPlayer().play (0, -1);
        const auto startMs = juce::Time::getMillisecondCounterHiRes();
        runUntil ([] { return false; }, 1000);
        const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0;
        const auto take = recorder.stop();
        engine.getPlayer().pause();

        const auto expected = elapsed * rate;
        std::cout << "      recorded " << take.clip.getNumFrames() << " frames in " << elapsed << " s, anchor "
                  << take.anchorRecorded << " -> " << take.anchorPlayed << std::endl;
        check (take.droppedFrames == 0 && std::abs ((double) take.clip.getNumFrames() - expected) < rate * 0.1,
               "live take has the expected length with nothing dropped");
        check (take.anchorPlayed >= 0 && (double) take.anchorPlayed < rate * 0.2, "live take is anchored to playback");

        engine.setInput (0, 0);
        check (! engine.getRecorder().isInputActive()
                   && engine.deviceManager.getCurrentAudioDevice()->getActiveInputChannels().isZero(),
               "inputs turn off again");
    }

    /** The plug-ins to add, as the rack and the tracks offer them: a submenu a format, then a maker. */
    void testPluginMenu (const juce::Array<juce::PluginDescription>& found)
    {
        juce::Array<juce::PluginDescription> types;

        for (auto& d : found)
            if (! d.isInstrument)
                types.add (d);

        juce::PopupMenu menu;
        PluginMenu::add (menu, types);
        juce::StringArray formats;
        std::vector<int> seen ((size_t) types.size(), 0);
        auto placed = true, sorted = true;
        int maxId = 0;

        for (juce::PopupMenu::MenuItemIterator top (menu); top.next();)
        {
            const auto& format = top.getItem();
            formats.add (format.text);
            juce::String lastMaker;

            for (juce::PopupMenu::MenuItemIterator makers (*format.subMenu); makers.next();)
            {
                const auto& maker = makers.getItem();
                sorted = sorted && lastMaker.compareIgnoreCase (maker.text) < 0;
                lastMaker = maker.text;
                juce::String lastName;

                for (juce::PopupMenu::MenuItemIterator items (*maker.subMenu); items.next();)
                {
                    const auto& item = items.getItem();
                    const auto index = PluginMenu::chosen (types, item.itemID);

                    if (index < 0)
                    {
                        placed = false;
                        continue;
                    }

                    const auto& d = types.getReference (index);
                    seen[(size_t) index]++;
                    maxId = juce::jmax (maxId, item.itemID);
                    placed = placed && d.name == item.text && PluginMenu::formatTitle (d.pluginFormatName) == format.text
                             && (d.manufacturerName.trim().isEmpty() ? juce::String ("Unknown maker") : d.manufacturerName.trim()) == maker.text;
                    sorted = sorted && lastName.compareIgnoreCase (item.text) <= 0;
                    lastName = item.text;
                }
            }
        }

        const auto once = std::all_of (seen.begin(), seen.end(), [] (int n) { return n == 1; });
        const auto auFirst = ! formats.contains ("Audio Units") || formats[0] == "Audio Units";
        const auto vst3Next = ! formats.contains ("VST3") || formats.indexOf ("VST3") == (formats.contains ("Audio Units") ? 1 : 0);
        check (! types.isEmpty() && once && placed && sorted && auFirst && vst3Next && maxId < 1000000 && PluginMenu::chosen (types, 0) == -1
                   && PluginMenu::chosen (types, types.size() + 1) == -1,
               "plug-in menu: " + juce::String (types.size()) + " effects under " + formats.joinIntoString (", ")
                   + ", each once, under its maker, sorted; its ids stay below the track menu's slot actions");
    }

    void testRack (AudioEngine& engine, const juce::Array<juce::PluginDescription>& found)
    {
        int added = 0;

        // Apple's own effects first: third-party plug-ins may stop for a licence or activation
        // alert, which blocks an unattended run (and puts a dialog on the user's screen).
        juce::Array<juce::PluginDescription> candidates;

        for (auto& d : found)
            if (d.manufacturerName == "Apple" && ! d.isInstrument)
                candidates.add (d);

        if (candidates.isEmpty())
            candidates = found;

        for (auto& d : candidates)
        {
            if (added == 3)
                break;

            bool finished = false;
            juce::String error;
            engine.addPlugin (d, [&] (const juce::String& e) { error = e; finished = true; });
            runUntil ([&] { return finished; }, 30000);
            std::cout << "      add " << d.pluginFormatName << " " << d.name << ": " << (error.isEmpty() ? juce::String ("ok") : error) << std::endl;
            added += error.isEmpty() ? 1 : 0;
        }

        check (engine.getNumRackSlots() == added && added > 0, "plug-ins load into the rack (" + juce::String (added) + ")");

        if (added == 0)
            return;

        {
            // The device manager also announces device-list changes, which some plug-ins set off many
            // times a second; they must neither reach the engine's listeners nor rebuild the rack's
            // rows (a row replaced under the mouse loses its click).
            struct Counter final : public juce::ChangeListener
            {
                void changeListenerCallback (juce::ChangeBroadcaster*) override  { ++count; }
                int count = 0;
            } counter;

            RackView view (engine);
            view.setSize (400, 300);
            juce::Array<juce::Component*> before;

            for (auto* c : view.getChildren())
                before.add (c);

            engine.addChangeListener (&counter);

            for (int i = 0; i < 20; ++i)
                engine.deviceManager.sendSynchronousChangeMessage();

            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            const auto quietEngine = counter.count == 0;
            engine.sendChangeMessage();
            runUntil ([&] { return counter.count > 0; }, 1000);

            juce::Array<juce::Component*> after;

            for (auto* c : view.getChildren())
                after.add (c);

            const juce::Component::SafePointer<juce::Component> row (before.getLast());   // a row is gone once rebuilt
            engine.setRackSlotBypassed (0, true);
            runUntil ([&] { return counter.count > 1; }, 1000);
            const auto rebuilt = row == nullptr;
            engine.setRackSlotBypassed (0, false);
            runUntil ([&] { return counter.count > 2; }, 1000);
            engine.removeChangeListener (&counter);

            check (quietEngine && after == before && rebuilt,
                   "rack: device-list announcements pass the engine by, an engine change that leaves the rack alone keeps its rows, "
                   "and turning a slot off rebuilds them");
        }

        // Change a parameter and bypass so restore can be told apart from defaults.
        auto* first = engine.getRackPlugin (0);
        juce::AudioProcessorParameter* param = nullptr;

        for (auto* p : first->getParameters())
            if (p->isAutomatable())
            {
                param = p;
                break;
            }

        float setValue = -1.0f;
        juce::String paramName;   // the plug-in is destroyed by clearRack(), so keep the name, not the pointer

        if (param != nullptr)
        {
            setValue = param->getValue() < 0.5f ? 0.8f : 0.2f;
            param->setValueNotifyingHost (setValue);
            paramName = param->getName (64);
        }

        engine.setRackSlotBypassed (0, true);

        if (added > 1)
            engine.moveRackSlot (1, 0);   // order becomes [second, first]

        const auto expectedFirstName = engine.getRackPlugin (0)->getName();
        auto xml = engine.createRackXml();

        engine.clearRack();
        check (engine.getNumRackSlots() == 0, "rack clears");

        bool finished = false;
        juce::String errors;
        engine.loadRackXml (*xml, [&] (const juce::String& e) { errors = e; finished = true; });
        runUntil ([&] { return finished; }, 60000);

        check (finished && errors.isEmpty() && engine.getNumRackSlots() == added, "rack restores from XML");
        check (engine.getRackPlugin (0)->getName() == expectedFirstName, "rack order survives save/load");

        const auto restoredIndex = added > 1 ? 1 : 0;
        check (engine.isRackSlotBypassed (restoredIndex), "bypass survives save/load");

        if (paramName.isNotEmpty())
        {
            for (auto* p : engine.getRackPlugin (restoredIndex)->getParameters())
            {
                if (p->getName (64) == paramName)
                {
                    check (std::abs (p->getValue() - setValue) < 0.01f,
                           "parameter '" + p->getName (64) + "' survives save/load (" + juce::String (p->getValue(), 3) + ")");
                    break;
                }
            }
        }

        engine.clearRack();

        // The same plug-in on a session track, saved and loaded back.
        SessionState s;
        s.sampleRate = 48000.0;
        const auto trackId = s.addTrack().id;
        engine.session.reset (s, {});

        finished = false;
        engine.addTrackPlugin (trackId, candidates[0], [&] (const juce::String& e) { errors = e; finished = true; });
        runUntil ([&] { return finished; }, 30000);
        check (finished && errors.isEmpty() && engine.getTrackEffects().getNumSlots (trackId) == 1 && engine.session.isDirty(),
               "a real plug-in loads onto a session track");

        engine.setTrackPluginBypassed (trackId, 0, true);
        std::map<juce::uint32, std::shared_ptr<juce::XmlElement>> saved;
        saved[trackId] = engine.getTrackEffects().createXml (trackId);
        engine.clearTrackEffects();

        finished = false;
        engine.loadTrackEffects (saved, [&] (const juce::String& e) { errors = e; finished = true; });
        runUntil ([&] { return finished; }, 30000);
        check (finished && errors.isEmpty() && engine.getTrackEffects().getNumSlots (trackId) == 1
                   && engine.getTrackEffects().isBypassed (trackId, 0),
               "track effects restore with their bypass");
        engine.clearTrackEffects();
    }
}

int main (int argc, char* argv[])
{
    // The live input test opens the interface's inputs. macOS asks for microphone access for
    // every new build of this binary and blocks until someone answers, so it runs only on request.
    //   --live-input            also record from the interface
    //   --app <executable>      use this copy of the app as the scan worker
    //   --plugin-sweep <report> only scan every plug-in on this machine and write what works
    juce::StringArray args;

    for (int i = 1; i < argc; ++i)
        args.add (argv[i]);

    const auto liveInput = args.contains ("--live-input");
    const auto valueOf = [&] (const char* flag) { const auto i = args.indexOf (flag); return i >= 0 ? args[i + 1] : juce::String(); };

    if (const auto app = valueOf ("--app"); app.isNotEmpty())
        TestUtils::appExecutable = juce::File::getCurrentWorkingDirectory().getChildFile (app);

    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::Array<juce::PluginDescription> found;

    if (const auto report = valueOf ("--plugin-sweep"); report.isNotEmpty())
    {
        AudioEngine engine;
        runPluginSweep (engine, juce::File::getCurrentWorkingDirectory().getChildFile (report));
        return 0;
    }

    std::cout << "Scan worker: " << TestUtils::appExecutable.getFullPathName() << std::endl;

    {
        AudioEngine engine;
        runDocumentTests (engine);
        runRecorderTests();
        runMarkerTests (engine);
        runConverterTests();
        runSpectralTests();
        runLoudnessTests();
        runEqTests();
        runGraphicEqTests();
        runMultibandTests();
        runDynamicsTests();
        runModulationTests();
        runReverbTests();
        runConvolverTests();
        runEffectPresetsTests();
        runBuiltInEffectsTests (engine);
        runNoiseTests();
        runClickTests();
        runSessionTests (engine.formatManager);
        runTrackEffectsTests();
        runBusTests();
        runAutomationTests (engine.formatManager);
        runMultitrackViewTests (engine);
        runMixDownTests (engine);
        runEffectAutomationTests (engine.formatManager);
        runTrackRecordingTests (engine.formatManager);
        runMetronomeTests (engine.formatManager);
        runMixerTests (engine);
        runBounceTests();
        runAuditionTests (engine);
        runDockTests();
        runOpenFilesTests();
        runPanelTests();
        runThemeTests();
        runEditorBarTests();
        runFadeHandleTests();

        // Scan the way the app does: on a background thread, with the message loop running.
        std::atomic<bool> scanned { false };
        std::thread scanThread ([&] { testScanner (engine, found); scanned = true; });
        runUntil ([&] { return scanned.load(); }, 600000);
        scanThread.join();
        runPluginTests (engine);
        testPluginMenu (found);

        testDevice (engine);
        if (liveInput)
            testLiveInput (engine);
        testRack (engine, found);
    }

    const auto failures = TestUtils::failures;
    std::cout << (failures == 0 ? "ALL PASSED" : juce::String (failures) + " FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
