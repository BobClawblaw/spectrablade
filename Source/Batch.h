#pragma once

#include <JuceHeader.h>
#include "LossyFiles.h"
#include "Ffmpeg.h"
#include "Sequences.h"

/** Batch processing: the same steps run over many files, each written as a new file.

    Each file is read whole (any type the app opens, through ffmpeg where it must), then, in
    order: run through the effects rack (fresh plug-ins, latency compensated, as Apply Rack does),
    played through a recorded sequence (on the whole file), converted to another sample rate, fitted to one or two channels, brought to a level (its peak,
    its integrated loudness under a true-peak ceiling, or a delivery spec, with what it measures in the notes), and written in the chosen type. Markers,
    tags and cover art go with it where the new type holds them. The source is never changed:
    the new file goes in the chosen folder (or beside the source), named after the source with
    any suffix, and numbered when that name is taken. */
namespace Batch
{
    enum class Level { none, peak, loudness, spec };

    struct Settings
    {
        bool applyRack = false;        // the rack as it was when the batch started
        std::shared_ptr<const Sequences::Sequence> sequence;   // none: no sequence
        double sampleRate = 0.0;       // 0 keeps each file's own
        int channels = 0;              // 0 keeps each file's own; 1 or 2
        Level level = Level::none;
        float peakDb = -1.0f;          // dBFS, for Level::peak
        double lufs = -16.0;           // integrated loudness, for Level::loudness
        double ceilingDbtp = -1.0;     // the true peak it may not pass
        bool limit = false;            // reach the loudness with a true-peak limiter where gain alone can't
        double specLufs = -23.0, specToleranceLu = 0.5, specCeilingDbtp = -1.0;   // for Level::spec, as Meet Delivery Spec does it

        /** "wav", "aiff", "flac", "mp3", "aac", or "ffmpeg:" and an ffmpeg target's codec. */
        juce::String type = "wav";
        int bitDepth = 0;              // WAV, AIFF, FLAC: 0 keeps the file's own when the type holds it, else the best
        LossyFiles::Settings lossy;    // MP3 and AAC
        int ffmpegKbps = 0;            // an ffmpeg target's bit rate: the nearest it offers (0: its default)

        juce::File folder;             // none: beside each source
        juce::String suffix;           // added to each name, e.g. " (mastered)"

        /** An empty string when the settings can be run, otherwise why not. */
        juce::String problem() const;
    };

    /** What happened to one file. */
    struct Outcome
    {
        juce::File source, written;    // written: none when it failed
        juce::String error;            // why it failed
        juce::StringArray notes;       // what was done that wasn't asked for, or couldn't be
        juce::String delivery;         // with a delivery spec as the level: the file's report (Delivery::report)
        bool meetsSpec = false;        // and whether it meets the spec
    };

    /** The types offered: WAV, AIFF, FLAC, MP3 and AAC, then ffmpeg's when it is installed. */
    juce::StringArray types (bool withFfmpeg);

    /** A type's name to show ("AAC (.m4a)", "Opus (ffmpeg)"), and its file extension ("m4a"). */
    juce::String nameOf (const juce::String& type);
    juce::String extensionOf (const juce::String& type);

    /** The file a source would be written to: in the folder (or beside the source), named after
        the source with the suffix and the type's extension, numbered " 2", " 3"... when that name
        is taken or is the source itself. */
    juce::File targetFor (const juce::File& source, const Settings&);

    /** Runs the steps on one file. rack is the effects rack's description (used when
        settings.applyRack); plugins makes its plug-ins, on this thread. progress gets 0 to 1 and
        returns false to stop. Call from a background thread. */
    Outcome process (const juce::File& source, const Settings&, juce::AudioFormatManager&,
                     const juce::XmlElement* rack, juce::AudioPluginFormatManager* plugins,
                     const std::function<bool (float)>& progress = {});

    /** The files that would be processed from what was dropped or chosen: audio files as they are,
        and those in folders (and their folders), in name order, each once. */
    juce::Array<juce::File> collect (const juce::Array<juce::File>& chosen, juce::AudioFormatManager&);

    /** A report of the outcomes, one line each, after a summary line. */
    juce::String report (const std::vector<Outcome>&);

    /** With a delivery spec as the level: how many files meet it, then each file's report (or
        why it wasn't written). Empty for any other level. */
    juce::String deliveryReport (const std::vector<Outcome>&, const Settings&);

    /** Writes deliveryReport() as "Delivery Report <when>.txt" (numbered when that is taken) in
        the batch's folder, or beside the first file written. Returns the file, or none when
        there is no report, nothing was written, or it couldn't be saved. */
    juce::File writeDeliveryReport (const std::vector<Outcome>&, const Settings&, juce::Time when);
}
