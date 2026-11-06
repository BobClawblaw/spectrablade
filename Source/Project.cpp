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

    const juce::StringArray infoFields { "title", "artist", "album", "genre", "year", "track", "comment",
                                         "description", "originator", "originatorReference", "originationDate", "originationTime" };

    juce::String* infoField (FileInfo& i, int index)
    {
        juce::String* fields[] = { &i.title, &i.artist, &i.album, &i.genre, &i.year, &i.track, &i.comment,
                                   &i.description, &i.originator, &i.originatorReference, &i.originationDate, &i.originationTime };
        return fields[index];
    }

    void writeStep (juce::XmlElement& e, const History::Step& s)
    {
        e.setAttribute ("channels", s.channels);
        e.setAttribute ("rate", s.sampleRate);
        e.setAttribute ("description", s.description);
        e.setAttribute ("picture", s.picture.toString());
        e.setAttribute ("cover", s.cover);

        for (auto& p : s.pieces)
        {
            auto* x = e.createNewChildElement ("PIECE");
            x->setAttribute ("block", p.block);
            x->setAttribute ("offset", int64Text (p.offset));
            x->setAttribute ("length", int64Text (p.length));
        }

        for (auto& m : s.markers)
        {
            auto* x = e.createNewChildElement ("MARKER");
            x->setAttribute ("start", int64Text (m.start));
            x->setAttribute ("length", int64Text (m.length));
            x->setAttribute ("name", m.name);
            x->setAttribute ("colour", m.colour);
        }

        auto* info = e.createNewChildElement ("INFO");
        auto copy = s.info;

        for (int i = 0; i < infoFields.size(); ++i)
            if (infoField (copy, i)->isNotEmpty())
                info->setAttribute (infoFields[i], *infoField (copy, i));

        info->setAttribute ("timeReference", int64Text (s.info.timeReference));
    }

    History::Step readStep (const juce::XmlElement& e)
    {
        History::Step s;
        s.channels = e.getIntAttribute ("channels");
        s.sampleRate = e.getDoubleAttribute ("rate", 44100.0);
        s.description = e.getStringAttribute ("description");
        s.picture = PictureMap::fromString (e.getStringAttribute ("picture"));
        s.cover = e.getIntAttribute ("cover", -1);

        for (auto* x : e.getChildWithTagNameIterator ("PIECE"))
            s.pieces.push_back ({ x->getIntAttribute ("block"), int64Of (*x, "offset"), int64Of (*x, "length") });

        for (auto* x : e.getChildWithTagNameIterator ("MARKER"))
            s.markers.push_back ({ int64Of (*x, "start"), int64Of (*x, "length"), x->getStringAttribute ("name"), x->getIntAttribute ("colour", -1) });

        if (auto* info = e.getChildByName ("INFO"))
        {
            for (int i = 0; i < infoFields.size(); ++i)
                *infoField (s.info, i) = info->getStringAttribute (infoFields[i]);

            s.info.timeReference = int64Of (*info, "timeReference");
        }

        return s;
    }
}

juce::File filesFolder (const juce::File& project)
{
    return project.getSiblingFile (project.getFileNameWithoutExtension() + " Files");
}

juce::File historyFolder (const juce::File& project)
{
    return filesFolder (project).getChildFile ("History");
}

std::shared_ptr<History> historyOf (const AudioDocument& doc)
{
    const auto steps = doc.getSteps();

    if (steps.size() < 2)
        return {};

    auto h = std::make_shared<History>();
    h->position = doc.getPosition();
    h->saved = doc.getSavedPosition();
    h->bitDepth = doc.getSourceBitDepth();
    std::map<const AudioBlock*, int> blockIndex;
    std::map<const juce::MemoryBlock*, int> coverIndex;

    for (auto& s : steps)
    {
        History::Step out;
        out.channels = s.snapshot->getNumChannels();
        out.markers = s.markers;
        out.sampleRate = s.sampleRate;
        out.description = s.description;
        out.info = s.info;
        out.info.cover = nullptr;
        out.picture = s.picture;

        for (auto& seg : s.snapshot->getSegments())
        {
            auto [it, isNew] = blockIndex.emplace (seg.block.get(), (int) h->blocks.size());

            if (isNew)
                h->blocks.push_back (seg.block);

            out.pieces.push_back ({ it->second, seg.offset, seg.length });
        }

        if (s.info.hasCover())
        {
            auto [it, isNew] = coverIndex.emplace (s.info.cover.get(), (int) h->covers.size());

            if (isNew)
                h->covers.push_back (s.info.cover);

            out.cover = it->second;
        }

        h->steps.push_back (std::move (out));
    }

    return h;
}

juce::String writeHistory (History& h, const juce::File& folder, juce::AudioFormatManager& formats, const std::function<bool()>& keepGoing)
{
    if (! folder.createDirectory())
        return "Could not make the folder " + folder.getFullPathName();

    h.blockFiles.clear();
    h.coverFiles.clear();

    for (size_t k = 0; k < h.blocks.size(); ++k)
    {
        if (keepGoing != nullptr && ! keepGoing())
            return "Stopped.";

        const auto& block = h.blocks[k];
        const auto target = folder.getChildFile ("Block " + juce::String ((int) k + 1) + ".wav");
        const AudioSnapshot whole (block->getNumChannels(), { { block, 0, block->getNumFrames() } });

        // The rate in the WAV doesn't matter: the steps say theirs.
        if (const auto error = AudioFileIO::save (whole, 48000.0, target, 32, formats); error.isNotEmpty())
            return target.getFileName() + ": " + error;

        h.blockFiles.push_back (target);
    }

    for (size_t k = 0; k < h.covers.size(); ++k)
    {
        const auto target = folder.getChildFile ("Cover " + juce::String ((int) k + 1) + ".bin");

        if (! target.replaceWithData (h.covers[k]->getData(), h.covers[k]->getSize()))
            return "Could not write " + target.getFullPathName();

        h.coverFiles.push_back (target);
    }

    return {};
}

std::vector<AudioDocument::Step> readHistory (const History& h, juce::AudioFormatManager& formats)
{
    std::vector<AudioDocument::Snapshot> blocks;

    for (auto& f : h.blockFiles)
    {
        auto loaded = AudioFileIO::load (f, formats);

        if (loaded.snapshot == nullptr)
            return {};

        blocks.push_back (loaded.snapshot);
    }

    std::vector<std::shared_ptr<const juce::MemoryBlock>> covers;

    for (auto& f : h.coverFiles)
    {
        auto m = std::make_shared<juce::MemoryBlock>();
        f.loadFileAsData (*m);
        covers.push_back (std::move (m));
    }

    std::vector<AudioDocument::Step> out;

    for (auto& s : h.steps)
    {
        std::vector<AudioSnapshot::Segment> segments;

        for (auto& p : s.pieces)
        {
            if (! juce::isPositiveAndBelow (p.block, (int) blocks.size()) || p.offset < 0 || p.offset + p.length > blocks[(size_t) p.block]->getNumFrames()
                || blocks[(size_t) p.block]->getNumChannels() != s.channels)
                return {};

            for (auto& seg : blocks[(size_t) p.block]->slice (p.offset, p.offset + p.length))
                segments.push_back (seg);
        }

        auto info = s.info;

        if (juce::isPositiveAndBelow (s.cover, (int) covers.size()))
            info.cover = covers[(size_t) s.cover];

        out.push_back ({ std::make_shared<const AudioSnapshot> (s.channels, std::move (segments)), s.markers, s.sampleRate, s.description,
                         std::move (info), s.picture });
    }

    return out;
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

        if (const auto& h = f.history; h != nullptr && ! h->blockFiles.empty())
        {
            auto* x = e->createNewChildElement ("HISTORY");
            x->setAttribute ("position", h->position);
            x->setAttribute ("saved", h->saved);
            x->setAttribute ("bitDepth", h->bitDepth);

            for (auto& b : h->blockFiles)
                writePath (*x->createNewChildElement ("BLOCK"), "path", b, project);

            for (auto& b : h->coverFiles)
                writePath (*x->createNewChildElement ("COVER"), "path", b, project);

            for (auto& s : h->steps)
                writeStep (*x->createNewChildElement ("STEP"), s);
        }
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

            if (auto* x = e->getChildByName ("HISTORY"))
            {
                auto h = std::make_shared<History>();
                h->position = x->getIntAttribute ("position");
                h->saved = x->getIntAttribute ("saved", -1);
                h->bitDepth = x->getIntAttribute ("bitDepth", 24);
                auto complete = true;

                for (auto* b : x->getChildWithTagNameIterator ("BLOCK"))
                {
                    h->blockFiles.push_back (readPath (*b, "path", project));
                    complete = complete && h->blockFiles.back().existsAsFile();
                }

                for (auto* b : x->getChildWithTagNameIterator ("COVER"))
                    h->coverFiles.push_back (readPath (*b, "path", project));

                for (auto* step : x->getChildWithTagNameIterator ("STEP"))
                    h->steps.push_back (readStep (*step));

                if (complete && ! h->steps.empty())
                    f.history = h;
                else
                    out.missing.add (file.getFileName() + ": its undo history (the audio kept for it isn't all there)");
            }

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

        s.files.push_back ({ doc.getFile(), doc.getVideo(), i == files.getActiveIndex() ? activePlace : files.placeAt (i), nullptr });
    }

    s.active = juce::jlimit (0, juce::jmax (0, (int) s.files.size() - 1), s.active);
    return s;
}

void attachHistories (State& state, const OpenFiles& files, const std::vector<std::shared_ptr<History>>& histories)
{
    // gather leaves out untitled files; the rest are in order.
    size_t at = 0;

    for (int i = 0; i < files.size() && at < state.files.size(); ++i)
    {
        const auto& doc = files.documentAt (i);

        if (doc.getFile() == juce::File())
            continue;

        if (auto h = juce::isPositiveAndBelow (i, (int) histories.size()) ? histories[(size_t) i] : nullptr)
        {
            const auto steps = doc.getSteps();

            if (steps.size() == h->steps.size() && doc.getPosition() == h->position)
            {
                h->saved = doc.getSavedPosition();
                h->bitDepth = doc.getSourceBitDepth();
                state.files[at].history = h;
            }
        }

        ++at;
    }
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

        // With its undo history when the project keeps one that can be read; otherwise as the file is.
        auto steps = f.history != nullptr ? readHistory (*f.history, engine.formatManager) : std::vector<AudioDocument::Step>();

        if (f.history != nullptr && steps.empty())
            out.missing.add (f.file.getFileName() + ": its undo history couldn't be read, so it opened as saved");

        if (! steps.empty())
        {
            files.prepareForAnother (previous);
            AudioDocument restored;
            restored.restoreHistory (std::move (steps), f.history->position, f.history->saved, f.file, f.history->bitDepth);
            engine.swapDocument (restored);
        }
        else
        {
            auto loaded = AudioFileIO::load (f.file, engine.formatManager);

            if (loaded.error.isNotEmpty())
            {
                out.missing.add (f.file.getFileName() + ": " + loaded.error);
                continue;
            }

            files.prepareForAnother (previous);
            engine.adoptFile (f.file, std::move (loaded));
        }

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
