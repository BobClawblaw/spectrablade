#pragma once

#include <JuceHeader.h>
#include "Markers.h"
#include "FileInfo.h"

//==============================================================================
/** Immutable multichannel audio. Edits never change a block; they make new snapshots
    that reference ranges of existing blocks, so undo and copy cost almost nothing. */
class AudioBlock
{
public:
    explicit AudioBlock (juce::AudioBuffer<float>&& samples);

    int getNumChannels() const noexcept           { return data.getNumChannels(); }
    juce::int64 getNumFrames() const noexcept     { return data.getNumSamples(); }
    const float* getChannel (int ch) const        { return data.getReadPointer (ch); }

    /** Min/max of one channel over frames [start, end), using precomputed peaks where possible. */
    juce::Range<float> getPeak (int channel, juce::int64 start, juce::int64 end) const;

    static constexpr int framesPerPeak = 256;

private:
    juce::AudioBuffer<float> data;
    std::vector<std::vector<juce::Range<float>>> peaks;   // [channel][chunk]
};

//==============================================================================
/** One state of the document: an ordered list of block ranges. */
class AudioSnapshot
{
public:
    struct Segment
    {
        std::shared_ptr<const AudioBlock> block;
        juce::int64 offset = 0;   // first frame used in the block
        juce::int64 length = 0;   // frames used
    };

    AudioSnapshot (int numChannels, std::vector<Segment> segments);

    int getNumChannels() const noexcept           { return numChannels; }
    juce::int64 getNumFrames() const noexcept     { return totalFrames; }
    const std::vector<Segment>& getSegments() const noexcept { return segments; }

    /** Copies frames [start, start + numFrames) into dest at destStart. Frames past the end read as silence. */
    void read (juce::AudioBuffer<float>& dest, int destStart, juce::int64 start, int numFrames) const;

    juce::Range<float> getPeak (int channel, juce::int64 start, juce::int64 end) const;

    /** The segments covering [start, end), trimmed to it. */
    std::vector<Segment> slice (juce::int64 start, juce::int64 end) const;

private:
    int findSegment (juce::int64 frame) const;

    int numChannels;
    std::vector<Segment> segments;
    std::vector<juce::int64> starts;   // document frame where each segment begins
    juce::int64 totalFrames = 0;
};

//==============================================================================
/** The open audio file: current snapshot and markers, sample rate, undo history and dirty state.
    Edit methods must be called on the message thread. getSnapshot() may be called from any thread. */
class AudioDocument final : public juce::ChangeBroadcaster
{
public:
    AudioDocument();

    using Snapshot = std::shared_ptr<const AudioSnapshot>;

    /** Replaces everything (new file); clears undo history. */
    void reset (Snapshot initial, double sampleRate, const juce::File& file, int sourceBitDepth, Markers = {}, FileInfo = {});

    Snapshot getSnapshot() const;
    juce::uint32 getVersion() const noexcept      { return version.load(); }

    /** Message thread only. */
    const Markers& getMarkers() const noexcept    { return history[(size_t) position].markers; }

    /** The file's title, artist and so on, and its broadcast details. Message thread only. */
    const FileInfo& getInfo() const noexcept      { return history[(size_t) position].info; }

    /** Increments each time a different file (or a new empty document) is loaded. */
    int getResetCount() const noexcept            { return resetCount; }

    bool hasAudio() const                         { return getSnapshot()->getNumFrames() > 0; }
    double getSampleRate() const noexcept         { return sampleRate.load(); }
    int getNumChannels() const                    { return getSnapshot()->getNumChannels(); }
    juce::int64 getNumFrames() const              { return getSnapshot()->getNumFrames(); }
    juce::File getFile() const                    { return file; }
    int getSourceBitDepth() const noexcept        { return sourceBitDepth; }

    //==============================================================================
    /** Applies a new state as one undoable step. The first form keeps the markers as they are
        (for edits that do not move audio in time). */
    void apply (Snapshot next, const juce::String& description);
    void apply (Snapshot next, const juce::String& description, Markers);

    /** Replaces the audio with a version at another sample rate, as one undoable step. */
    void applyNewRate (Snapshot next, double newSampleRate, Markers, const juce::String& description);

    /** Changes only the markers, as one undoable step. */
    void setMarkers (Markers, const juce::String& description);

    /** Changes only the file info, as one undoable step. */
    void setInfo (FileInfo, const juce::String& description);

    bool canUndo() const noexcept                 { return position > 0; }
    bool canRedo() const noexcept                 { return position + 1 < (int) history.size(); }
    juce::String getUndoDescription() const;
    juce::String getRedoDescription() const;
    void undo();
    void redo();

    /** The undo steps, oldest first: the first is where the file started (no description), and
        getPosition() is the one it is at. Steps after it can be redone. */
    juce::StringArray getHistory() const;
    int getPosition() const noexcept              { return position; }

    /** Goes back or forward to a step in getHistory(), as that many undos or redos would. */
    void goTo (int step);

    bool isDirty() const noexcept                 { return position != savedPosition; }
    void markSaved (const juce::File& savedAs, int bitDepth);

    /** Trades everything with another document (its audio, undo history, markers, file and saved
        point), as switching between open files does; counted as a new file by both. The audio
        leaving this one is held until the audio thread lets go of it. Message thread only. */
    void swapWith (AudioDocument& other);

    /** Frees snapshots dropped from history once the audio thread has let go of them. */
    void releaseUnusedSnapshots();

private:
    struct Entry
    {
        Snapshot snapshot;
        Markers markers;
        double sampleRate;
        juce::String description;
        FileInfo info {};
    };

    void setCurrent (const Entry&);
    void push (Entry, bool keepInfo = true);
    void retire (Snapshot);

    mutable juce::SpinLock lock;
    Snapshot current;
    std::atomic<juce::uint32> version { 0 };

    std::vector<Entry> history;
    int position = 0;
    int savedPosition = 0;
    std::vector<Snapshot> pendingRelease;

    std::atomic<double> sampleRate { 44100.0 };
    juce::File file;
    int sourceBitDepth = 24;
    int resetCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioDocument)
};
