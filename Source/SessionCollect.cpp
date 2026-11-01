#include "SessionCollect.h"

namespace SessionCollect
{
Plan plan (const SessionState& s, const juce::File& audioFolder)
{
    Plan p;
    p.state = s;
    std::map<juce::String, juce::File> placed;   // full path of each file used, and where it goes
    std::set<juce::String> taken;                // names already given in the folder, ignoring case

    const auto place = [&] (juce::File& f)
    {
        if (f == juce::File())
            return;

        if (! f.existsAsFile())
        {
            p.missing.addIfNotAlreadyThere (f.getFullPathName());
            return;
        }

        auto& to = placed[f.getFullPathName()];

        if (to == juce::File())
        {
            auto name = f.getFileName();

            for (int n = 2; taken.count (name.toLowerCase()) > 0; ++n)
                name = f.getFileNameWithoutExtension() + " " + juce::String (n) + f.getFileExtension();

            taken.insert (name.toLowerCase());
            to = audioFolder.getChildFile (name);
            p.copies.emplace_back (f, to);
        }

        f = to;
    };

    for (auto& t : p.state.tracks)
    {
        for (auto& c : t.clips)
        {
            place (c.source);

            for (auto& take : c.takes)
                place (take.source);
        }
    }

    return p;
}

juce::String save (const SessionState& s, const juce::File& sessionFile,
                   const std::function<std::unique_ptr<juce::XmlElement> (juce::uint32 trackId)>& trackEffects,
                   juce::StringArray& missing)
{
    const auto folder = sessionFile.getParentDirectory().getChildFile ("Audio");
    auto p = plan (s, folder);
    missing = p.missing;

    if (! p.copies.empty() && ! folder.createDirectory())
        return "Could not make the folder " + folder.getFullPathName();

    for (auto& [from, to] : p.copies)
        if (from != to && ! from.copyFileTo (to))
            return "Could not copy " + from.getFullPathName() + " to " + to.getFullPathName();

    return SessionFile::save (p.state, sessionFile, trackEffects);
}
}
