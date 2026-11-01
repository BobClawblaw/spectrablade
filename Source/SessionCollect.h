#pragma once

#include "Session.h"

/** Saving a copy of a session with every audio file it uses (clips and their takes) copied
    into an "Audio" folder beside it, so the copy can be moved or sent as one folder. */
namespace SessionCollect
{
    struct Plan
    {
        SessionState state;                                        // the session pointing at the copies
        std::vector<std::pair<juce::File, juce::File>> copies;     // each file used, and where it goes
        juce::StringArray missing;                                 // files used that aren't there
    };

    /** Where each distinct file goes in audioFolder (by its own name; a second file with the
        same name is numbered: "Take 2.wav"), and the session pointing there. Files that aren't
        there are listed and left pointing where they were. */
    Plan plan (const SessionState&, const juce::File& audioFolder);

    /** Copies the audio and saves the session as sessionFile, with its audio in an "Audio"
        folder beside it, and trackEffects as SessionFile::save takes them. Returns an error
        message, or empty; the files that were missing come back in missing. */
    juce::String save (const SessionState&, const juce::File& sessionFile,
                       const std::function<std::unique_ptr<juce::XmlElement> (juce::uint32 trackId)>& trackEffects,
                       juce::StringArray& missing);
}
