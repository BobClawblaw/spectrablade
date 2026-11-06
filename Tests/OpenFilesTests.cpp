#include <JuceHeader.h>
#include "../Source/AudioEdits.h"
#include "../Source/AudioEngine.h"
#include "../Source/OpenFiles.h"
#include "../Source/WaveformView.h"
#include "TestUtils.h"
#include "OpenFilesTests.h"

namespace
{
    using TestUtils::check;

    /** A mono file of `frames` frames, every sample `value`, at 48 kHz, as if read from `name`. */
    void load (AudioEngine& engine, const juce::String& name, juce::int64 frames, float value)
    {
        juce::AudioBuffer<float> b (1, (int) frames);

        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, value);

        AudioFileIO::Loaded loaded;
        loaded.snapshot = AudioEdits::fromBuffer (std::move (b));
        loaded.sampleRate = 48000.0;
        loaded.bitDepth = 24;
        engine.adoptFile (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name), std::move (loaded));
    }

    float firstSample (const AudioDocument& doc)
    {
        juce::AudioBuffer<float> b (doc.getNumChannels(), 1);
        doc.getSnapshot()->read (b, 0, 0, 1);
        return b.getSample (0, 0);
    }

    juce::File fileNamed (const juce::String& name)
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name);
    }

    void testSwitching()
    {
        AudioEngine engine;
        OpenFiles files (engine);
        auto& doc = engine.document;

        files.prepareForAnother ({});
        check (files.size() == 1 && files.getActiveIndex() == 0, "open files: an empty editor takes the first file in place");

        load (engine, "a.wav", 1000, 0.25f);
        doc.apply (AudioEdits::gain (*doc.getSnapshot(), 0, 1000, 6.0f), "Gain");
        const auto aSnapshot = doc.getSnapshot();
        FilePlace aPlace { { 100, 200 }, 100, 50.0, 2.0, {}, {} };

        files.prepareForAnother (aPlace);
        const auto emptied = ! doc.hasAudio() && doc.getFile() == juce::File() && ! doc.canUndo();
        load (engine, "b.wav", 2000, 0.5f);

        check (emptied && files.size() == 2 && files.getActiveIndex() == 1 && files.indexOf (fileNamed ("a.wav")) == 0
                   && files.indexOf (fileNamed ("b.wav")) == 1 && files.indexOf ({}) == -1,
               "open files: another file opens beside the first, in an empty document, and each is found by its file");
        check (files.documentAt (0).getSnapshot() == aSnapshot && files.documentAt (0).isDirty() && files.documentAt (0).canUndo()
                   && doc.getNumFrames() == 2000 && ! doc.isDirty(),
               "open files: the first is kept aside as it was, its edit and undo step included");
        check (files.unsaved() == juce::Array<int> { 0 }, "open files: the one with unsaved changes is listed");

        FilePlace bPlace { { 10, 20 }, 10, 0.0, 1.5, {}, {} };
        const auto back = files.activate (0, bPlace);
        check (back.has_value() && back->selection == aPlace.selection && juce::exactlyEqual (back->framesPerPixel, 2.0)
                   && doc.getSnapshot() == aSnapshot && doc.getFile() == fileNamed ("a.wav") && doc.isDirty()
                   && juce::approximatelyEqual (firstSample (doc), juce::Decibels::decibelsToGain (6.0f) * 0.25f),
               "open files: switching back brings the first file's audio, edited, into the editor with its place");

        doc.undo();
        check (! doc.isDirty() && juce::exactlyEqual (firstSample (doc), 0.25f) && files.unsaved().isEmpty(),
               "open files: and its undo history: undoing the edit there leaves it unchanged since it was opened");

        check (! files.activate (0, {}).has_value() && ! files.activate (5, {}).has_value(),
               "open files: switching to the file already shown, or one that isn't open, does nothing");

        const auto toB = files.activate (1, {});
        check (toB.has_value() && toB->selection == bPlace.selection && doc.getNumFrames() == 2000 && juce::exactlyEqual (firstSample (doc), 0.5f),
               "open files: the second comes back with its own audio and place");

        load (engine, "c.wav", 3000, 0.75f);   // as if read into the editor without making room: replaces b
        files.prepareForAnother ({});
        load (engine, "d.wav", 4000, 1.0f);
        check (files.size() == 3 && files.getActiveIndex() == 2 && files.nameAt (1) == "c.wav" && files.nameAt (2) == "d.wav",
               "open files: a new file goes in after the one it was opened from");

        files.activate (1, {});
        const auto afterClose = files.close (1);
        check (afterClose.has_value() && files.size() == 2 && files.getActiveIndex() == 1 && doc.getFile() == fileNamed ("d.wav"),
               "open files: closing the file shown brings in the one below it");

        files.close (0);
        check (files.size() == 1 && files.getActiveIndex() == 0 && doc.getFile() == fileNamed ("d.wav"),
               "open files: closing one aside leaves the one shown as it was");

        files.close (0);
        check (files.size() == 1 && ! doc.hasAudio() && doc.getFile() == juce::File(),
               "open files: closing the last leaves the editor empty");
    }

    void testViewPlaces()
    {
        AudioEngine engine;
        OpenFiles files (engine);
        WaveformView view (engine);
        view.setBounds (0, 0, 1000, 400);
        const auto settle = [] { TestUtils::runUntil ([] { return false; }, 60); };

        load (engine, "first.wav", 480000, 0.1f);
        settle();
        view.zoomIn();
        view.zoomIn();
        view.setSelection ({ 12000, 24000 });
        const auto kept = view.getPlace();

        files.prepareForAnother (view.getPlace());
        load (engine, "second.wav", 96000, 0.2f);
        settle();
        const auto secondFitted = view.getSelection().isEmpty() && view.getVisibleFrames().getLength() >= 96000.0 - 1.0;

        if (const auto place = files.activate (0, view.getPlace()))
            view.setPlace (*place);

        settle();   // the change the switch sends must not fit it afterwards
        const auto now = view.getPlace();
        check (secondFitted && now.selection == kept.selection && juce::exactlyEqual (now.viewStart, kept.viewStart)
                   && juce::exactlyEqual (now.framesPerPixel, kept.framesPerPixel),
               "open files: a file opened is fitted to the view; switching back puts the first one's selection and zoom back");
    }

    void testPanel()
    {
        AudioEngine engine;
        OpenFiles files (engine);
        FilesPanel panel (files);
        panel.setSize (300, 400);

        load (engine, "song.wav", 48000 * 75, 0.1f);
        files.prepareForAnother ({});
        load (engine, "voice.wav", 24000, 0.1f);
        engine.document.apply (AudioEdits::gain (*engine.document.getSnapshot(), 0, 100, 1.0f), "Gain");

        check (panel.describeRow (0) == "song.wav - 1:15.000, 48 kHz, mono, 24-bit" && panel.describeRow (1) == "voice.wav * - 0:00.500, 48 kHz, mono, 24-bit",
               "Files panel: each file with its length and format, an unsaved one starred (" + panel.describeRow (1) + ")");

        int chosen = -1, closed = -1;
        panel.onChoose = [&] (int i) { chosen = i; };
        panel.onClose = [&] (int i) { closed = i; };
        panel.clickRow (0, false);
        panel.clickRow (1, true);
        panel.clickRow (7, false);
        check (chosen == 0 && closed == 1, "Files panel: clicking a row asks to show it, its x to close it; a row that isn't there does nothing");
    }
}

namespace
{
    juce::MouseEvent press (juce::Component& c, juce::Point<int> at, juce::ModifierKeys mods = {})
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at.toFloat(), mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                                 now, at.toFloat(), now, 1, false);
    }

    void testTabs()
    {
        AudioEngine engine;
        OpenFiles files (engine);
        FileTabs tabs (files);
        tabs.setSize (900, FileTabs::height);
        const auto empty = ! tabs.hasTabs();

        load (engine, "Drums.wav", 1000, 0.1f);
        files.prepareForAnother ({});
        load (engine, "Bass.wav", 1000, 0.2f);
        files.prepareForAnother ({});
        load (engine, "A very long file name for the lead vocal take seven.wav", 1000, 0.3f);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (20);

        const auto first = tabs.getTabBounds (0), second = tabs.getTabBounds (1), third = tabs.getTabBounds (2);
        check (empty && tabs.hasTabs() && first.getX() == 0 && second.getX() == first.getRight() && third.getX() == second.getRight()
                   && third.getWidth() == FileTabs::widest && first.getWidth() < third.getWidth() && tabs.tabAt (second.getCentre()) == 1,
               "file tabs: none while the editor is empty, then one per open file side by side, each as wide as its name needs up to a limit");

        std::vector<int> chosen, closed;
        tabs.onChoose = [&] (int i) { chosen.push_back (i); };
        tabs.onClose = [&] (int i) { closed.push_back (i); };
        tabs.mouseDown (press (tabs, first.getCentre().withX (first.getX() + 20)));
        tabs.mouseUp (press (tabs, first.getCentre().withX (first.getX() + 20)));
        const auto closeAt = tabs.getCloseBounds (1).getCentre();
        tabs.mouseDown (press (tabs, closeAt));
        tabs.mouseUp (press (tabs, closeAt));
        tabs.mouseDown (press (tabs, tabs.getCloseBounds (2).getCentre()));
        tabs.mouseUp (press (tabs, first.getCentre()));   // released away from the ×: nothing closes
        check (chosen == std::vector<int> { 0 } && closed == std::vector<int> { 1 },
               "file tabs: clicking a tab shows its file, and its × closes it only when released over it");

        tabs.setSize (150, FileTabs::height);
        const auto squeezed = tabs.getTabBounds (0).getWidth() == 50 && tabs.getTabBounds (2).getRight() == 150;
        tabs.createComponentSnapshot (tabs.getLocalBounds());   // paints
        check (squeezed, "file tabs: when they don't fit they share the width evenly");
    }

    void testDroppingSeveral()
    {
        AudioEngine engine;
        WaveformView view (engine);
        view.setBounds (0, 0, 800, 400);
        juce::Array<juce::File> dropped;
        view.onFilesDropped = [&] (const juce::Array<juce::File>& f) { dropped = f; };
        view.filesDropped ({ "/tmp/one.wav", "/tmp/notes.txt", "/tmp/two.aiff", "/tmp/three.flac" }, 10, 10);
        check (dropped.size() == 3 && dropped[0].getFileName() == "one.wav" && dropped[1].getFileName() == "two.aiff"
                   && dropped[2].getFileName() == "three.flac",
               "file tabs: dropping several files on the editor hands on every audio file, in order, leaving out the rest");
    }
}

void runOpenFilesTests()
{
    testSwitching();
    testViewPlaces();
    testPanel();
    testTabs();
    testDroppingSeveral();
}
