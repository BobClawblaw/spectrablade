#include <JuceHeader.h>
#include "../Source/AudioEdits.h"
#include "../Source/AudioFileIO.h"
#include "../Source/BuiltInEffects.h"
#include "../Source/Project.h"
#include "TestUtils.h"
#include "ProjectTests.h"

namespace
{
    using TestUtils::check;

    AudioDocument::Snapshot tone (int frames, double hz, float level)
    {
        juce::AudioBuffer<float> b (2, frames);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, level * (float) std::sin (juce::MathConstants<double>::twoPi * hz * (i + 7 * ch) / 48000.0));

        return AudioEdits::fromBuffer (std::move (b));
    }

    bool identical (const AudioSnapshot& a, const AudioSnapshot& b)
    {
        if (a.getNumChannels() != b.getNumChannels() || a.getNumFrames() != b.getNumFrames())
            return false;

        juce::AudioBuffer<float> x (a.getNumChannels(), (int) a.getNumFrames()), y (b.getNumChannels(), (int) b.getNumFrames());
        a.read (x, 0, 0, x.getNumSamples());
        b.read (y, 0, 0, y.getNumSamples());

        for (int ch = 0; ch < x.getNumChannels(); ++ch)
            if (std::memcmp (x.getReadPointer (ch), y.getReadPointer (ch), sizeof (float) * (size_t) x.getNumSamples()) != 0)
                return false;

        return true;
    }

    bool samePlace (const FilePlace& a, const FilePlace& b)
    {
        return a.selection == b.selection && a.cursor == b.cursor && juce::exactlyEqual (a.viewStart, b.viewStart)
            && juce::exactlyEqual (a.framesPerPixel, b.framesPerPixel) && a.band == b.band;
    }

    juce::MemoryBlock bytes (const juce::File& f)   { juce::MemoryBlock m; f.loadFileAsData (m); return m; }
}

void runProjectTests()
{
    AudioEngine engine;
    OpenFiles files (engine);

    juce::TemporaryFile workTemp;
    const auto work = workTemp.getFile();
    work.getChildFile ("Proj").createDirectory();

    // Two files on disk, one never saved, and a video for the first.
    const auto a = work.getChildFile ("Take A.wav"), b = work.getChildFile ("Take B.wav"), video = work.getChildFile ("Proj/Scene.mp4");
    AudioFileIO::save (*tone (48000, 440.0, 0.25f), 48000.0, a, 24, engine.formatManager);
    AudioFileIO::save (*tone (24000, 660.0, 0.5f), 48000.0, b, 16, engine.formatManager);
    video.replaceWithText ("not really a video");
    const auto aBytes = bytes (a), bBytes = bytes (b);

    const FilePlace placeA { { 1000, 9000 }, 1000, 500.0, 12.5, { 200.0, 4000.0 }, {} };
    const FilePlace placeB { { 0, 0 }, 4321, 0.0, 3.0, {}, {} };
    const FilePlace placeC { { 10, 20 }, 15, 1.0, 0.5, {}, {} };

    engine.adoptFile (a, AudioFileIO::load (a, engine.formatManager));
    engine.document.setVideo (video);
    files.prepareForAnother (placeA);
    engine.adoptFile (b, AudioFileIO::load (b, engine.formatManager));
    const auto original = engine.document.getSnapshot();

    // B's history: a gain, a marker, tags with a cover, a cut, and the cut undone (so it can be redone).
    auto& docB = engine.document;
    docB.apply (AudioEdits::gain (*docB.getSnapshot(), 0, 24000, -6.0f), "Gain");
    docB.setMarkers ({ { 1000, 0, "Hit", 3 } }, "Add Marker");
    FileInfo tags;
    tags.title = "Take B";
    tags.comment = "second take";
    const char fakeJpeg[] = "\xff\xd8\xff\xe0 not much of a JPEG";
    tags.cover = std::make_shared<const juce::MemoryBlock> (fakeJpeg, sizeof (fakeJpeg) - 1);
    docB.setInfo (tags, "File Info");
    docB.apply (AudioEdits::remove (*docB.getSnapshot(), 2000, 4000), "Delete", MarkerEdits::afterReplace (docB.getMarkers(), 2000, 4000, 0));
    const auto cut = docB.getSnapshot();
    const auto cutPicture = docB.getPictureMap();
    docB.undo();
    const auto edited = engine.document.getSnapshot();
    files.prepareForAnother (placeB);
    AudioFileIO::Loaded generated;
    generated.snapshot = tone (12000, 1000.0, 0.3f);
    generated.sampleRate = 48000.0;
    generated.bitDepth = 32;
    engine.adoptFile ({}, std::move (generated));
    const auto generatedAudio = engine.document.getSnapshot();
    files.activate (1, placeC);   // B shown, at placeB

    // An effect in the rack, set away from its default.
    const auto descriptions = BuiltInEffects::getDescriptions();
    auto added = false;
    engine.clearRack();
    engine.addPlugin (descriptions[0], [&] (const juce::String&) { added = true; });
    TestUtils::runUntil ([&] { return added; }, 10000);
    auto* effect = engine.getNumRackSlots() == 1 ? engine.getRackPlugin (0) : nullptr;

    // Band 1's gain, a plain number (the first parameters are switches).
    const auto gainOf = [] (juce::AudioProcessor* p) -> juce::AudioProcessorParameter*
    {
        if (p != nullptr)
            for (auto* param : p->getParameters())
                if (param->getName (64) == "Band 1 Gain")
                    return param;

        return nullptr;
    };

    if (auto* g = gainOf (effect))
        g->setValueNotifyingHost (0.37f);

    const auto setValue = gainOf (effect) != nullptr ? gainOf (effect)->getValue() : -1.0f;

    // A session with a track, never saved.
    SessionState session;
    session.sampleRate = 48000.0;
    SessionEdits::addClip (session, 0, a, AudioFileIO::load (a, engine.formatManager).snapshot, 4800);
    engine.session.reset (std::move (session), {});

    // Saved: the edited and the untitled file are written beside it; the files opened are left alone.
    const auto project = work.getChildFile ("Proj/My Mix.sbproject");
    const auto copies = Project::copiesNeeded (files, project);
    std::vector<std::shared_ptr<Project::History>> histories;

    for (int i = 0; i < files.size(); ++i)
        histories.push_back (Project::historyOf (files.documentAt (i)));

    auto writeError = Project::writeCopies (copies, engine.formatManager);

    for (size_t i = 0; i < histories.size(); ++i)
        if (histories[i] != nullptr && writeError.isEmpty())
            writeError = Project::writeHistory (*histories[i], Project::historyFolder (project).getChildFile (juce::String ((int) i + 1)), engine.formatManager);

    Project::adoptCopies (files, copies);
    const auto [sessionFile, sessionError] = Project::saveSession (engine, project);
    auto state = Project::gather (files, placeB, engine, true, sessionFile);
    Project::attachHistories (state, files, histories);
    const auto saveError = Project::save (state, project);
    const auto blocksWritten = histories[1] != nullptr ? (int) histories[1]->blockFiles.size() : 0;
    const auto keptFor = juce::String (histories[0] == nullptr ? "-" : "A") + (histories[1] == nullptr ? "-" : "B") + (histories[2] == nullptr ? "-" : "C");
    const auto folder = Project::filesFolder (project);

    juce::StringArray copied;

    for (auto& c : copies)
        copied.add (c.target.getRelativePathFrom (work).replaceCharacter ('\\', '/'));   // the same on every system

    const auto bCopy = folder.getChildFile ("Take B.wav"), cCopy = folder.getChildFile ("Untitled.wav");
    const auto bBack = AudioFileIO::load (bCopy, engine.formatManager), cBack = AudioFileIO::load (cCopy, engine.formatManager);

    check (writeError.isEmpty() && sessionError.isEmpty() && saveError.isEmpty() && project.existsAsFile()
               && copied.joinIntoString (", ") == "Proj/My Mix Files/Take B.wav, Proj/My Mix Files/Untitled.wav"
               && bytes (a) == aBytes && bytes (b) == bBytes && bBack.snapshot != nullptr && identical (*bBack.snapshot, *edited) && bBack.bitDepth == 32
               && cBack.snapshot != nullptr && identical (*cBack.snapshot, *generatedAudio) && files.unsaved().isEmpty()
               && files.documentAt (1).getFile() == bCopy && files.documentAt (2).getFile() == cCopy && files.documentAt (0).getFile() == a
               && sessionFile == folder.getChildFile ("My Mix.sbsession") && sessionFile.existsAsFile() && ! engine.session.isDirty(),
           "projects: saving one writes the edited file and the never-saved one as 32-bit float WAVs in \"My Mix Files\" beside it, bit for "
           "bit, and they become those files, saved; the files opened are left as they were, byte for byte; an untitled session with a track is "
           "saved into the same folder (written: " + copied.joinIntoString (", ") + ")");

    // The project's folder moved: the files inside it are found where it went; one outside by its full path.
    const auto moved = work.getChildFile ("Moved");
    work.getChildFile ("Proj").moveFileTo (moved);
    const auto movedProject = moved.getChildFile ("My Mix.sbproject");
    const auto loaded = Project::load (movedProject);

    // Everything changed, then the project opened again.
    files.closeAll();
    engine.clearRack();
    engine.session.reset (SessionState(), {});
    const auto restored = Project::restoreFiles (loaded.state, files, engine);

    auto rackBack = false;
    auto rackValue = -1.0f;

    if (loaded.state.rack != nullptr)
    {
        engine.loadRackXml (*loaded.state.rack, [&] (const juce::String&) { rackBack = true; });
        TestUtils::runUntil ([&] { return rackBack; }, 10000);

        if (auto* g = gainOf (engine.getNumRackSlots() == 1 ? engine.getRackPlugin (0) : nullptr))
            rackValue = g->getValue();
    }

    const auto sessionBack = SessionFile::load (loaded.state.session, engine.formatManager);
    const auto movedFolder = Project::filesFolder (movedProject);

    check (loaded.error.isEmpty() && loaded.missing.isEmpty() && loaded.state.multitrack && files.size() == 3 && files.getActiveIndex() == 1
               && files.documentAt (0).getFile() == a && files.documentAt (1).getFile() == movedFolder.getChildFile ("Take B.wav")
               && files.documentAt (2).getFile() == movedFolder.getChildFile ("Untitled.wav")
               && files.documentAt (0).getVideo() == moved.getChildFile ("Scene.mp4") && identical (*files.documentAt (1).getSnapshot(), *edited)
               && identical (*files.documentAt (2).getSnapshot(), *generatedAudio) && samePlace (restored.place, placeB)
               && samePlace (files.placeAt (0), placeA) && samePlace (files.placeAt (2), placeC) && restored.missing.isEmpty()
               && added && juce::approximatelyEqual (rackValue, setValue) && rackValue >= 0.0f
               && loaded.state.session == movedFolder.getChildFile ("My Mix.sbsession") && sessionBack.error.isEmpty()
               && sessionBack.state.tracks.size() == 1 && sessionBack.state.tracks[0].clips.size() == 1,
           "projects: opened again after its folder moved, the three files come back in order, the second shown, each with its audio "
           "(edits included, bit for bit), selection, cursor, zoom and spectral band, the first with its video; the rack's effect with its "
           "Band 1 Gain (" + juce::String (rackValue, 3) + " for " + juce::String (setValue, 3) + "); the session with its track and clip; "
           "the session in front");

    // B's undo history came back with it: where it was, what it was saved at, every step's audio,
    // markers and tags, redo and undo to the file as opened.
    {
        auto& d = files.documentAt (1);
        const auto steps = d.getSteps();
        juce::StringArray names;

        for (auto& st : steps)
            names.add (st.description);

        const auto atSaved = ! d.isDirty() && d.getPosition() == 3 && d.canRedo();
        const auto withTags = d.getInfo().title == "Take B" && d.getInfo().comment == "second take" && d.getInfo().cover != nullptr
                              && *d.getInfo().cover == *tags.cover;
        const auto withMarker = d.getMarkers().size() == 1 && d.getMarkers()[0].name == "Hit" && d.getMarkers()[0].colour == 3;
        const auto nowAudio = identical (*d.getSnapshot(), *edited);
        d.redo();
        const auto redone = identical (*d.getSnapshot(), *cut) && d.getPictureMap() == cutPicture && ! cutPicture.isIdentity()
                            && d.getMarkers().size() == 1 && d.getMarkers()[0].start == 1000;
        d.goTo (0);
        const auto atStart = identical (*d.getSnapshot(), *original) && d.getMarkers().empty() && d.getInfo().title.isEmpty();

        check (keptFor == "-B-" && names.joinIntoString (",") == ",Gain,Add Marker,File Info,Delete" && atSaved && withTags && withMarker
                   && nowAudio && redone && atStart && blocksWritten >= 2,
               "projects: a file's undo history is kept (" + juce::String ((int) steps.size()) + " steps, its audio in " + juce::String (blocksWritten)
               + " shared blocks): it opens at the step it was at and saved at, with that step's markers and tags (the cover's bytes "
               "too); redo brings back the undone cut, its picture map included; going back to the start gives the file as first "
               "opened, bit for bit; files with no steps keep none");
        d.goTo (3);
    }

    // A file gone: left out and named; the one shown is still the one it was.
    a.deleteFile();
    const auto without = Project::load (movedProject);
    const auto again = Project::restoreFiles (without.state, files, engine);
    auto wrong = juce::XmlElement ("SOMETHING_ELSE");
    auto newer = Project::toXml ({}, movedProject);
    newer->setAttribute ("version", 99);

    check (without.missing.size() == 1 && without.missing[0].endsWith ("Take A.wav") && files.size() == 2 && files.getActiveIndex() == 0
               && files.documentAt (0).getFile().getFileName() == "Take B.wav" && samePlace (again.place, placeB)
               && Project::fromXml (wrong, movedProject).error.isNotEmpty() && Project::fromXml (*newer, movedProject).error.isNotEmpty()
               && Project::load (moved.getChildFile ("nothing.sbproject")).error.isNotEmpty(),
           "projects: a file no longer there is left out and named, the one shown kept; a file that isn't a project, one from a newer "
           "version, or none at all is refused with a reason");

    // A history whose audio is gone: the file opens as saved, and that is said.
    {
        const auto history = Project::historyFolder (movedProject);
        juce::Array<juce::File> blocks;
        history.findChildFiles (blocks, juce::File::findFiles, true, "*.wav");

        for (auto& f : blocks)
            f.deleteFile();

        const auto broken = Project::load (movedProject);
        Project::restoreFiles (broken.state, files, engine);
        check (! blocks.isEmpty() && broken.missing.joinIntoString ("\n").contains ("undo history") && files.size() == 2   // B and the untitled one (A went above)
                   && files.documentAt (0).getSteps().size() == 1 && identical (*files.documentAt (0).getSnapshot(), *edited),
               "projects: when the audio kept for a history is gone, the file opens as it was saved, and that is said");
    }

    // Saved again with nothing changed: nothing more written.
    check (Project::copiesNeeded (files, movedProject).empty() && Project::saveSession (engine, movedProject).first == juce::File(),
           "projects: saved again with nothing changed, no audio is written; with no session, none is kept");

    files.closeAll();
    engine.clearRack();
    work.deleteRecursively();
}
