#include "Project.h"
#include "AudioFileIO.h"

namespace Project
{
namespace
{
    constexpr int formatVersion = 1;

    /** A path as written: relative to the project's folder when inside it. */
    void writePath (juce::XmlElement& e, const juce::String& name, const juce::File& file, const juce::File& project)
    {
        if (file == juce::File())
            return;

        const auto folder = project.getParentDirectory();
        e.setAttribute (name, file.getFullPathName());

        if (file.isAChildOf (folder))
            e.setAttribute (name + "Relative", file.getRelativePathFrom (folder));
    }

    /** A path as read: the relative one when it is there (the folder moved), otherwise the full one. */
    juce::File readPath (const juce::XmlElement& e, const juce::String& name, const juce::File& project)
    {
        if (const auto relative = e.getStringAttribute (name + "Relative"); relative.isNotEmpty())
            if (const auto f = project.getParentDirectory().getChildFile (relative); f.exists())
                return f;

        const auto full = e.getStringAttribute (name);
        return full.isNotEmpty() && juce::File::isAbsolutePath (full) ? juce::File (full) : juce::File();
    }

    juce::String int64Text (juce::int64 v)   { return juce::String (v); }
    juce::int64 int64Of (const juce::XmlElement& e, const char* name)   { return e.getStringAttribute (name).getLargeIntValue(); }
}

juce::File filesFolder (const juce::File& project)
{
    return project.getSiblingFile (project.getFileNameWithoutExtension() + " Files");
}

std::unique_ptr<juce::XmlElement> toXml (const State& state, const juce::File& project)
{
    auto xml = std::make_unique<juce::XmlElement> ("SPECTRABLADE_PROJECT");
    xml->setAttribute ("version", formatVersion);
    xml->setAttribute ("active", state.active);
    xml->setAttribute ("multitrack", state.multitrack);

    auto* files = xml->createNewChildElement ("FILES");

    for (auto& f : state.files)
    {
        auto* e = files->createNewChildElement ("FILE");
        writePath (*e, "path", f.file, project);
        writePath (*e, "video", f.video, project);
        e->setAttribute ("selectionStart", int64Text (f.place.selection.getStart()));
        e->setAttribute ("selectionEnd", int64Text (f.place.selection.getEnd()));
        e->setAttribute ("cursor", int64Text (f.place.cursor));
        e->setAttribute ("viewStart", f.place.viewStart);
        e->setAttribute ("framesPerPixel", f.place.framesPerPixel);
        e->setAttribute ("bandLow", f.place.band.getStart());
        e->setAttribute ("bandHigh", f.place.band.getEnd());
    }

    if (state.session != juce::File())
        writePath (*xml->createNewChildElement ("SESSION"), "path", state.session, project);

    auto* rack = xml->createNewChildElement ("RACK");

    if (state.rack != nullptr)
        rack->addChildElement (new juce::XmlElement (*state.rack));

    return xml;
}

Loaded fromXml (const juce::XmlElement& xml, const juce::File& project)
{
    Loaded out;

    if (! xml.hasTagName ("SPECTRABLADE_PROJECT"))
    {
        out.error = "This isn't a SpectraBlade project.";
        return out;
    }

    if (xml.getIntAttribute ("version", 0) > formatVersion)
    {
        out.error = "This project was saved by a newer SpectraBlade.";
        return out;
    }

    auto& s = out.state;
    s.multitrack = xml.getBoolAttribute ("multitrack");
    const auto active = xml.getIntAttribute ("active");
    auto index = 0;

    if (auto* files = xml.getChildByName ("FILES"))
    {
        for (auto* e : files->getChildWithTagNameIterator ("FILE"))
        {
            const auto file = readPath (*e, "path", project);

            if (! file.existsAsFile())
            {
                out.missing.add (file != juce::File() ? file.getFullPathName() : e->getStringAttribute ("path"));
                ++index;
                continue;
            }

            OpenFile f;
            f.file = file;
            f.video = readPath (*e, "video", project);
            f.place.selection = { int64Of (*e, "selectionStart"), juce::jmax (int64Of (*e, "selectionStart"), int64Of (*e, "selectionEnd")) };
            f.place.cursor = int64Of (*e, "cursor");
            f.place.viewStart = e->getDoubleAttribute ("viewStart");
            f.place.framesPerPixel = juce::jmax (0.0, e->getDoubleAttribute ("framesPerPixel"));
            f.place.band = { e->getDoubleAttribute ("bandLow"), juce::jmax (e->getDoubleAttribute ("bandLow"), e->getDoubleAttribute ("bandHigh")) };

            // The one shown, or the one before it when that is missing.
            if (index <= active)
                s.active = (int) s.files.size();

            s.files.push_back (f);
            ++index;
        }
    }

    if (auto* session = xml.getChildByName ("SESSION"))
    {
        s.session = readPath (*session, "path", project);

        if (! s.session.existsAsFile())
        {
            out.missing.add (s.session != juce::File() ? s.session.getFullPathName() : session->getStringAttribute ("path"));
            s.session = juce::File();
        }
    }

    if (auto* rack = xml.getChildByName ("RACK"); rack != nullptr && rack->getFirstChildElement() != nullptr)
        s.rack = std::make_shared<const juce::XmlElement> (*rack->getFirstChildElement());

    return out;
}

juce::String save (const State& state, const juce::File& project)
{
    const auto xml = toXml (state, project);
    juce::TemporaryFile temp (project);

    if (! xml->writeTo (temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
        return "Could not write " + project.getFullPathName();

    return {};
}

Loaded load (const juce::File& project)
{
    const auto xml = juce::parseXML (project);

    if (xml == nullptr)
    {
        Loaded out;
        out.error = project.existsAsFile() ? "The project can't be read." : project.getFullPathName() + " isn't there.";
        return out;
    }

    return fromXml (*xml, project);
}

//==============================================================================
std::vector<Copy> copiesNeeded (const OpenFiles& files, const juce::File& project)
{
    std::vector<Copy> out;
    const auto folder = filesFolder (project);
    juce::StringArray taken;

    for (int i = 0; i < files.size(); ++i)
    {
        const auto& doc = files.documentAt (i);

        if (! doc.hasAudio() || (! doc.isDirty() && doc.getFile().existsAsFile()))
            continue;

        const auto stem = doc.getFile() != juce::File() ? doc.getFile().getFileNameWithoutExtension() : juce::String ("Untitled");
        auto target = folder.getChildFile (stem + ".wav");

        // Not over a file of another open document or one already there, unless it is this one's own.
        for (int n = 2; taken.contains (target.getFullPathName()) || (target.exists() && target != doc.getFile()); ++n)
            target = folder.getChildFile (stem + " " + juce::String (n) + ".wav");

        taken.add (target.getFullPathName());
        out.push_back ({ i, doc.getSnapshot(), doc.getSampleRate(), doc.getMarkers(), doc.getInfo(), target });
    }

    return out;
}

juce::String writeCopies (const std::vector<Copy>& copies, juce::AudioFormatManager& formats, const std::function<bool (float)>& progress)
{
    for (size_t i = 0; i < copies.size(); ++i)
    {
        if (progress != nullptr && ! progress ((float) i / (float) copies.size()))
            return "Stopped.";

        const auto& c = copies[i];

        if (! c.target.getParentDirectory().createDirectory())
            return "Could not make the folder " + c.target.getParentDirectory().getFullPathName();

        if (const auto error = AudioFileIO::save (*c.snapshot, c.sampleRate, c.target, 32, formats, c.markers, c.info); error.isNotEmpty())
            return c.target.getFileName() + ": " + error;
    }

    return {};
}

void adoptCopies (OpenFiles& files, const std::vector<Copy>& copies)
{
    for (auto& c : copies)
        if (juce::isPositiveAndBelow (c.index, files.size()))
            if (auto& doc = files.documentAt (c.index); doc.getSnapshot() == c.snapshot)
                doc.markSaved (c.target, 32);
}

std::pair<juce::File, juce::String> saveSession (AudioEngine& engine, const juce::File& project)
{
    auto& session = engine.session;
    const auto state = session.getState();
    auto file = session.getFile();

    if (file == juce::File() && state->tracks.empty())
        return {};   // no session to keep

    if (file != juce::File() && ! session.isDirty() && file.existsAsFile())
        return { file, {} };

    if (file == juce::File())
    {
        const auto folder = filesFolder (project);

        if (! folder.createDirectory())
            return { {}, "Could not make the folder " + folder.getFullPathName() };

        file = folder.getChildFile (project.getFileNameWithoutExtension() + ".sbsession");
    }

    if (const auto error = SessionFile::save (*state, file, [&engine] (juce::uint32 id) { return engine.getTrackEffects().createXml (id); });
        error.isNotEmpty())
        return { {}, error };

    session.markSaved (file);
    return { file, {} };
}

State gather (const OpenFiles& files, const FilePlace& activePlace, const AudioEngine& engine, bool multitrack, const juce::File& session)
{
    State s;
    s.multitrack = multitrack;
    s.session = session;
    s.rack = engine.createRackXml();

    for (int i = 0; i < files.size(); ++i)
    {
        const auto& doc = files.documentAt (i);

        if (doc.getFile() == juce::File())
            continue;

        if (i == files.getActiveIndex())
            s.active = (int) s.files.size();

        s.files.push_back ({ doc.getFile(), doc.getVideo(), i == files.getActiveIndex() ? activePlace : files.placeAt (i) });
    }

    s.active = juce::jlimit (0, juce::jmax (0, (int) s.files.size() - 1), s.active);
    return s;
}

Restored restoreFiles (const State& state, OpenFiles& files, AudioEngine& engine)
{
    Restored out;
    files.closeAll();
    std::vector<int> openedAt (state.files.size(), -1);
    FilePlace previous;

    for (size_t i = 0; i < state.files.size(); ++i)
    {
        const auto& f = state.files[i];
        auto loaded = AudioFileIO::load (f.file, engine.formatManager);

        if (loaded.error.isNotEmpty())
        {
            out.missing.add (f.file.getFileName() + ": " + loaded.error);
            continue;
        }

        files.prepareForAnother (previous);
        engine.adoptFile (f.file, std::move (loaded));

        if (f.video != juce::File())
            engine.document.setVideo (f.video);

        openedAt[i] = files.getActiveIndex();
        previous = f.place;
    }

    // Each is aside with its place; the one to show is brought in (or stays, as the last opened).
    const auto wanted = juce::isPositiveAndBelow (state.active, (int) openedAt.size()) ? openedAt[(size_t) state.active] : -1;

    if (wanted >= 0 && wanted != files.getActiveIndex())
        out.place = files.activate (wanted, previous).value_or (FilePlace {});
    else
        out.place = previous;

    return out;
}
}
