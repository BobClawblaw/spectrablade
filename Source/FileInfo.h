#pragma once

#include <JuceHeader.h>

/** What a file says about itself: the usual tags (title, artist and so on) and the broadcast-wave
    (BWF) details a WAV file can carry. */
struct FileInfo
{
    juce::String title, artist, album, genre, year, track, comment;

    // Broadcast wave (the bext chunk): WAV only.
    juce::String description, originator, originatorReference, originationDate, originationTime;
    juce::int64 timeReference = 0;    // frames since midnight

    bool hasTags() const;
    bool hasBroadcast() const;
    bool isEmpty() const              { return ! hasTags() && ! hasBroadcast(); }

    bool operator== (const FileInfo&) const = default;
};

namespace FileInfoIO
{
    //==============================================================================
    // WAV: tags in a LIST INFO chunk (INAM, IART, IPRD, IGNR, ICRD, ITRK, ICMT), the broadcast details in
    // the bext chunk, which JUCE writes from these metadata values.

    /** The broadcast details from (and for) a WAV reader's or writer's metadata values. */
    void readBroadcast (const juce::StringPairArray& metadata, FileInfo&);
    juce::StringPairArray broadcastMetadata (const FileInfo&);

    /** The tags in a RIFF/WAVE file's LIST INFO chunk (UTF-8 or Latin-1 text). */
    void readRiffInfo (const juce::File&, FileInfo&);

    /** Adds a LIST INFO chunk with the tags to the end of a RIFF/WAVE file and corrects its size.
        Fails (with a message) on RF64 files and on files that would pass 4 GB. */
    juce::String appendRiffInfo (const juce::File&, const FileInfo&);

    //==============================================================================
    // MP3: an ID3v2.3 tag at the start (Latin-1 text, or UTF-16 where needed); version 2.4 is read too.

    juce::MemoryBlock id3v2 (const FileInfo&);
    void readId3v2 (const juce::File&, FileInfo&);

    /** The size of the ID3v2 tag at the start of a file's data, or 0. */
    size_t id3v2Size (const void* data, size_t size);

    //==============================================================================
    // AIFF and AIFF-C: an ID3v2.3 tag in an "ID3 " chunk, as Apple's apps and ffmpeg keep them; the
    // older NAME, AUTH and ANNO chunks are read too.

    void readAiff (const juce::File&, FileInfo&);

    /** Adds an "ID3 " chunk with the tags to the end of an AIFF or AIFF-C file and corrects its size.
        Fails (with a message) on files that would pass 4 GB. */
    juce::String appendAiffTags (const juce::File&, const FileInfo&);

    //==============================================================================
    // FLAC: a Vorbis comment block among the metadata blocks at the start.

    /** Rewrites a FLAC file with a Vorbis comment block holding the tags (replacing any it had), after
        its other metadata blocks and before any padding; the audio frames are copied as they were. */
    juce::String addFlacComments (const juce::File&, const FileInfo&);

    //==============================================================================
    // MPEG-4 (.m4a): iTunes-style items in moov/udta/meta/ilst.

    void readMp4 (const juce::File&, FileInfo&);

    /** Adds the tags to an MPEG-4 file whose moov box is followed by a free box with room for them,
        as the files written here are, so no audio moves. */
    juce::String addMp4Tags (const juce::File&, const FileInfo&);

    //==============================================================================
    // Vorbis comments: read from Ogg files (Opus, Vorbis) and FLAC's metadata; written to FLAC above.

    void readVorbisComments (const juce::File&, FileInfo&);

    //==============================================================================
    /** The tags as ffmpeg -metadata arguments. */
    juce::StringArray ffmpegArguments (const FileInfo&);

    /** Reads whatever the file's type holds (WAV, AIFF, MP3, .m4a, FLAC, Ogg); the broadcast details from metadata. */
    FileInfo read (const juce::File&, const juce::StringPairArray& metadata);
}
