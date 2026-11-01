#pragma once

#include "Session.h"

/** A copy of a session with unsaved changes, written every so often while it is open, so the
    changes can be had back if the app closes without saving them (a crash, or the power going).
    Saving the session, or quitting normally, removes the copy. */
namespace SessionRecovery
{
    /** How often the main window writes a copy while there are unsaved changes. */
    constexpr int intervalSeconds = 120;

    struct Copy
    {
        juce::File session;                // the copy, to open
        juce::File original;               // the session's own file, or none for an unsaved one
        juce::Time written;
    };

    /** Writes the copy into folder (made if need be), replacing the last: the session, with
        trackEffects as SessionFile::save takes them, and a note of its own file and the time.
        Returns an error message, or empty. */
    juce::String write (const SessionState&, const juce::File& original,
                        const std::function<std::unique_ptr<juce::XmlElement> (juce::uint32 trackId)>& trackEffects,
                        const juce::File& folder);

    /** The copy in folder, if there is one. */
    std::optional<Copy> find (const juce::File& folder);

    /** Removes the copy from folder. */
    void clear (const juce::File& folder);
}
