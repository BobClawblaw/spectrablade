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
    // MPEG-4 (.m4a): iTunes-style items in moov/udta/meta/ilst.

    void readMp4 (const juce::File&, FileInfo&);

    /** Adds the tags to an MPEG-4 file whose moov box is followed by a free box with room for them,
        as the files written here are, so no audio moves. */
    juce::String addMp4Tags (const juce::File&, const FileInfo&);

    //==============================================================================
    // Vorbis comments, read only: in Ogg files (Opus, Vorbis) and in FLAC's metadata.

    void readVorbisComments (const juce::File&, FileInfo&);

    //==============================================================================
    /** The tags as ffmpeg -metadata arguments. */
    juce::StringArray ffmpegArguments (const FileInfo&);

    /** Reads whatever the file's type holds (WAV, MP3, .m4a, FLAC, Ogg); the broadcast details from metadata. */
    FileInfo read (const juce::File&, const juce::StringPairArray& metadata);
}
