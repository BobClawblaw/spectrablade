#pragma once

#include "OpenFiles.h"

/** A project (.sbproject): everything open, to come back to. The files open in the editor (each
    with its selection, cursor and zoom, and the video it goes with), which one is shown, the
    effects rack beside the editor with its effects' settings, the multitrack session (by its
    .sbsession file, which holds the tracks and their effects), and whether the editor or the
    session is in front.

    Audio isn't kept in the project file; it points to the files. Saving a project leaves the
    files you opened as they are: one with unsaved changes, or never saved, is written as a 32-bit
    float WAV into a folder beside the project ("<project> Files"), and the open file becomes that
    copy. An untitled session with tracks is saved into the same folder. Paths inside the
    project's own folder are kept relative to it, so the folder can be moved or copied whole.

    Undo histories aren't kept: a project reopens each file as it was saved. */
namespace Project
{
    inline constexpr const char* extension = "sbproject";

    struct OpenFile
    {
        juce::File file, video;
        FilePlace place;
    };

    struct State
    {
        std::vector<OpenFile> files;
        int active = 0;                                    // in files
        bool multitrack = false;                           // the session in front, not the editor
        juce::File session;                                // none: no session
        std::shared_ptr<const juce::XmlElement> rack;      // the effects rack, as AudioEngine::createRackXml writes it
    };

    /** The folder beside a project that unsaved audio and an untitled session are written into. */
    juce::File filesFolder (const juce::File& project);

    std::unique_ptr<juce::XmlElement> toXml (const State&, const juce::File& project);

    struct Loaded
    {
        State state;
        juce::StringArray missing;   // files and a session no longer there (left out of state)
        juce::String error;
    };

    Loaded fromXml (const juce::XmlElement&, const juce::File& project);

    juce::String save (const State&, const juce::File& project);
    Loaded load (const juce::File& project);

    //==============================================================================
    /** An open file to be written into the project's folder: changed since it was saved, or never saved. */
    struct Copy
    {
        int index = 0;
        AudioDocument::Snapshot snapshot;
        double sampleRate = 0.0;
        Markers markers;
        FileInfo info;
        juce::File target;
    };

    /** The open files that need writing for a project, with the names they get in its folder (their
        own name as .wav, numbered when taken; "Untitled" for one never saved). Message thread. */
    std::vector<Copy> copiesNeeded (const OpenFiles&, const juce::File& project);

    /** Writes them (any thread). progress (if given) is told how far it has got and may say stop. */
    juce::String writeCopies (const std::vector<Copy>&, juce::AudioFormatManager&, const std::function<bool (float)>& progress = {});

    /** Each open file written is now its copy, saved (unless it was edited meanwhile). Message thread. */
    void adoptCopies (OpenFiles&, const std::vector<Copy>&);

    /** Saves the session for a project if it needs it: one with changes into its own file, an untitled
        one with tracks into the project's folder. Gives the session file the project points to (none
        when there is no session), or an error. */
    std::pair<juce::File, juce::String> saveSession (AudioEngine&, const juce::File& project);

    /** What is open now, as a project. activePlace: where the editor is in the file shown. */
    State gather (const OpenFiles&, const FilePlace& activePlace, const AudioEngine&, bool multitrack, const juce::File& session);

    /** Closes every open file (ask about changes first) and opens the project's, each with its video
        and place, the active one shown. Gives the active file's place and the files that couldn't be
        read. The session and rack are left to the caller (they load asynchronously). */
    struct Restored
    {
        FilePlace place;
        juce::StringArray missing;
    };

    Restored restoreFiles (const State&, OpenFiles&, AudioEngine&);
}
