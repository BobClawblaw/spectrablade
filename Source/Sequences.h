#pragma once

#include <JuceHeader.h>
#include "AudioDocument.h"
#include "Markers.h"

/** Recorded command sequences: the edits made with commands that take settings (or none), kept
    with their settings, to play back on another file, or another part of one, in one go.

    A step is one command and its settings, in the dialog's own units. Playing a step uses the same
    code the command does, so a step played back gives what the command gave with those settings.
    A sequence plays on the selection, or on the whole file without one; steps that work on the
    whole file (Mix to Mono, Make Stereo, Convert Sample Rate) do so in any case. */
namespace Sequences
{
    using Values = std::map<juce::String, double>;

    struct Step
    {
        juce::String kind;                              // one of kinds()
        Values values;                                  // its settings
        std::shared_ptr<const juce::XmlElement> rack;   // "rack": the effects rack as it was

        /** The command's name, as its undo step reads ("Gain 3.00 dB", "Compressor"). */
        juce::String name() const;

        /** The settings in a line ("threshold -20, ratio 4"), empty for a step without any. */
        juce::String settingsText() const;
    };

    struct Sequence
    {
        juce::String name;
        std::vector<Step> steps;
    };

    /** The commands that can be recorded, by kind. */
    const juce::StringArray& kinds();

    /** Why the step can't run on audio at this rate and with these channels, or empty. */
    juce::String problem (const Step&, double sampleRate);

    struct Result
    {
        AudioDocument::Snapshot snapshot;    // run: never null; apply: null when the audio was left as it was
        double sampleRate = 0.0;
        Markers markers;
        juce::int64 start = 0, end = 0;      // where the range is now (a rate change moves it)
        juce::String error;                  // a step that couldn't run: nothing is changed
        juce::StringArray notes;             // what a step left as it was, and why
    };

    /** One step on [start, end) of the audio. plugins makes the rack's plug-ins (on this thread);
        progress gets 0 to 1 and returns false to stop (the error is then "Stopped."). */
    Result apply (const Step&, const AudioDocument::Snapshot&, juce::int64 start, juce::int64 end, double sampleRate,
                  const Markers&, juce::AudioPluginFormatManager* plugins, const std::function<bool (float)>& progress = {});

    /** Every step in order, each on the range where the one before left it. */
    Result run (const Sequence&, const AudioDocument::Snapshot&, juce::int64 start, juce::int64 end, double sampleRate,
                const Markers&, juce::AudioPluginFormatManager* plugins, const std::function<bool (float)>& progress = {});

    std::unique_ptr<juce::XmlElement> toXml (const Sequence&);

    /** Nothing when the XML isn't a sequence, or holds a step this version doesn't know. */
    std::optional<Sequence> fromXml (const juce::XmlElement&);

    /** Sequences kept as files in a folder, one each, by name. */
    class Store
    {
    public:
        explicit Store (juce::File folder);

        juce::StringArray names() const;     // sorted, ignoring case
        std::optional<Sequence> load (const juce::String& name) const;
        bool contains (const juce::String& name) const;
        bool save (const Sequence&);         // replaces one of the same name
        bool remove (const juce::String& name);

    private:
        juce::File fileFor (const juce::String& name) const;
        juce::File folder;
    };

    /** The user's sequences, in the app's data folder. */
    Store& userStore();

    /** Collects steps while recording, and the edits that couldn't be recorded. */
    class Recorder
    {
    public:
        void start();
        bool isRecording() const noexcept                { return recording; }
        void add (Step);
        void skip (const juce::String& edit);            // an edit made while recording that isn't a step

        /** Stops, giving what was recorded (with no name). */
        Sequence stop();

        const std::vector<Step>& steps() const noexcept   { return recorded; }
        const juce::StringArray& skipped() const noexcept { return notRecorded; }

    private:
        bool recording = false;
        std::vector<Step> recorded;
        juce::StringArray notRecorded;
    };
}
