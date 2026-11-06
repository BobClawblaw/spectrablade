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

    Each file's undo history is kept too, unless turned off: the audio blocks its steps share are
    written once each, as 32-bit float WAVs, into "<project> Files/History", with each step's
    markers, tags, sample rate and picture map, where the file was in its history and the step it
    was saved at. That can take a lot of room: every version of the audio the history holds. */
namespace Project
{
    inline constexpr const char* extension = "sbproject";

    /** One file's undo history: its steps, each as pieces of audio blocks that the steps share. */
    struct History
    {
        struct Piece
        {
            int block = 0;
            juce::int64 offset = 0, length = 0;
        };

        struct Step
        {
            int channels = 0;
            std::vector<Piece> pieces;
            Markers markers;
            double sampleRate = 44100.0;
            juce::String description;
            FileInfo info;   // its cover by `cover`
            int cover = -1;
            PictureMap picture;
        };

        int position = 0, saved = -1, bitDepth = 24;
        std::vector<Step> steps;
        std::vector<std::shared_ptr<const AudioBlock>> blocks;              // gathered, to be written
        std::vector<std::shared_ptr<const juce::MemoryBlock>> covers;
        std::vector<juce::File> blockFiles, coverFiles;                     // written or read
    };

    struct OpenFile
    {
        juce::File file, video;
        FilePlace place;
        std::shared_ptr<History> history;   // none: opened as the file is
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

    /** A document's undo history, to keep in a project (nothing when it has no steps but its start). */
    std::shared_ptr<History> historyOf (const AudioDocument&);

    /** The folder histories are written into, and each file's within it. */
    juce::File historyFolder (const juce::File& project);

    /** Writes a history's blocks and covers into a folder (any thread). */
    juce::String writeHistory (History&, const juce::File& folder, juce::AudioFormatManager&, const std::function<bool()>& keepGoing = {});

    /** A history's steps, their audio read back from its block files (empty when one can't be read). */
    std::vector<AudioDocument::Step> readHistory (const History&, juce::AudioFormatManager&);

    /** Gives the gathered files their histories (taken by historyOf before saving, by open file), with
        where each file now is in it and the step it is saved at; a file edited meanwhile goes without. */
    void attachHistories (State&, const OpenFiles&, const std::vector<std::shared_ptr<History>>&);

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
