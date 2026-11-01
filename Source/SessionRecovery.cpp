#include "SessionRecovery.h"

namespace SessionRecovery
{
namespace
{
    juce::File sessionIn (const juce::File& folder)   { return folder.getChildFile ("Recovered.sbsession"); }
    juce::File noteIn (const juce::File& folder)      { return folder.getChildFile ("Recovered.xml"); }
}

juce::String write (const SessionState& state, const juce::File& original,
                    const std::function<std::unique_ptr<juce::XmlElement> (juce::uint32)>& trackEffects,
                    const juce::File& folder)
{
    if (! folder.createDirectory())
        return "Couldn't make " + folder.getFullPathName() + ".";

    if (const auto error = SessionFile::save (state, sessionIn (folder), trackEffects); error.isNotEmpty())
        return error;

    juce::XmlElement note ("SPECTRABLADE_RECOVERY");
    note.setAttribute ("original", original.getFullPathName());
    note.setAttribute ("written", juce::String (juce::Time::getCurrentTime().toMilliseconds()));
    return note.writeTo (noteIn (folder)) ? juce::String() : "Couldn't write " + noteIn (folder).getFullPathName() + ".";
}

std::optional<Copy> find (const juce::File& folder)
{
    if (! sessionIn (folder).existsAsFile())
        return std::nullopt;

    Copy copy { sessionIn (folder), {}, sessionIn (folder).getLastModificationTime() };

    // Without the note (it is written after the session), the copy is still worth offering.
    if (const auto note = juce::XmlDocument::parse (noteIn (folder)); note != nullptr && note->hasTagName ("SPECTRABLADE_RECOVERY"))
    {
        if (const auto path = note->getStringAttribute ("original"); juce::File::isAbsolutePath (path))
            copy.original = juce::File (path);

        copy.written = juce::Time (note->getStringAttribute ("written").getLargeIntValue());
    }

    return copy;
}

void clear (const juce::File& folder)
{
    sessionIn (folder).deleteFile();
    noteIn (folder).deleteFile();
}
}
