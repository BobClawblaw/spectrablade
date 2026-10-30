// Integration checks against the real machine: document editing and file I/O, the plug-in scanner
// worker, the audio device (including multi-output interfaces) and rack save/restore with real plug-ins.
// The player is never started, so nothing is heard.

#include <JuceHeader.h>
#include "../Source/AudioEngine.h"
#include "../Source/PluginScanner.h"
#include "DocumentTests.h"
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
        OutOfProcessScanner scanner (juce::File (WAVELOOM_APP_EXECUTABLE));

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

    void testRack (AudioEngine& engine, const juce::Array<juce::PluginDescription>& found)
    {
        int added = 0;

        for (auto& d : found)
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
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::Array<juce::PluginDescription> found;

    {
        AudioEngine engine;
        runDocumentTests (engine);

        // Scan the way the app does: on a background thread, with the message loop running.
        std::atomic<bool> scanned { false };
        std::thread scanThread ([&] { testScanner (engine, found); scanned = true; });
        runUntil ([&] { return scanned.load(); }, 600000);
        scanThread.join();

        testDevice (engine);
        testRack (engine, found);
    }

    const auto failures = TestUtils::failures;
    std::cout << (failures == 0 ? "ALL PASSED" : juce::String (failures) + " FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
