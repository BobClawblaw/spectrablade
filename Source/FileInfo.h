#pragma once

#include <JuceHeader.h>

/** What a file says about itself: the usual tags (title, artist and so on), a front cover image, and
    the broadcast-wave (BWF) details a WAV file can carry. */
struct FileInfo
{
    juce::String title, artist, album, genre, year, track, comment;

    /** The front cover: a JPEG or PNG image's bytes, or null. Shared, as the document's undo steps
        each hold the info. */
    std::shared_ptr<const juce::MemoryBlock> cover;
    static constexpr size_t maxCoverBytes = 8 << 20;

    // Broadcast wave (the bext chunk): WAV only.
    juce::String description, originator, originatorReference, originationDate, originationTime;
    juce::int64 timeReference = 0;    // frames since midnight

    /** Any text tag. */
    bool hasText() const;
    bool hasCover() const             { return cover != nullptr && cover->getSize() > 0; }

    /** Any text tag or a cover: what the tag formats hold. */
    bool hasTags() const              { return hasText() || hasCover(); }
    bool hasBroadcast() const;
    bool isEmpty() const              { return ! hasTags() && ! hasBroadcast(); }

    /** "image/jpeg" or "image/png" for the cover, or empty. */
    juce::String coverMime() const;

    /** Every field equal, and the covers' bytes. */
    bool operator== (const FileInfo&) const;
};

namespace FileInfoIO
{
    /** True when the bytes are a JPEG or PNG image no larger than FileInfo::maxCoverBytes. */
    bool isCoverImage (const void* data, size_t size);

    //==============================================================================
    // WAV: tags in a LIST INFO chunk (INAM, IART, IPRD, IGNR, ICRD, ITRK, ICMT), the broadcast details in
    // the bext chunk, which JUCE writes from these metadata values. A cover goes in an "id3 " chunk
    // (ID3v2.3, with the tags again), which is read too.

    /** The broadcast details from (and for) a WAV reader's or writer's metadata values. */
    void readBroadcast (const juce::StringPairArray& metadata, FileInfo&);
    juce::StringPairArray broadcastMetadata (const FileInfo&);

    /** The tags in a RIFF/WAVE file's LIST INFO chunk (UTF-8 or Latin-1 text), and its "id3 " chunk's
        cover and any tags INFO doesn't have. */
    void readRiffInfo (const juce::File&, FileInfo&);

    /** Adds a LIST INFO chunk with the tags (and with a cover, an "id3 " chunk) to the end of a
        RIFF/WAVE file and corrects its size. Fails (with a message) on RF64 files and on files that
        would pass 4 GB. */
    juce::String appendRiffInfo (const juce::File&, const FileInfo&);

    //==============================================================================
    // MP3: an ID3v2.3 tag at the start (Latin-1 text, or UTF-16 where needed, and the cover as an APIC
    // frame); version 2.4 is read too.

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
    // FLAC: a Vorbis comment block and a PICTURE block among the metadata blocks at the start.

    /** The Vorbis comments and front cover in a FLAC file's metadata blocks. */
    void readFlac (const juce::File&, FileInfo&);

    /** Rewrites a FLAC file with a Vorbis comment block holding the tags and a PICTURE block holding
        the cover (replacing any it had), after its other metadata blocks and before any padding; the
        audio frames are copied as they were. */
    juce::String addFlacComments (const juce::File&, const FileInfo&);

    //==============================================================================
    // MPEG-4 (.m4a): iTunes-style items in moov/udta/meta/ilst, the cover as covr.

    void readMp4 (const juce::File&, FileInfo&);

    /** Adds the tags to an MPEG-4 file whose moov box is followed by a free box with room for them,
        as the files written here are, so no audio moves. */
    juce::String addMp4Tags (const juce::File&, const FileInfo&);

    //==============================================================================
    // Vorbis comments: read from Ogg files (Opus, Vorbis; their covers aren't read).

    void readVorbisComments (const juce::File&, FileInfo&);

    //==============================================================================
    /** The tags as ffmpeg -metadata arguments. */
    juce::StringArray ffmpegArguments (const FileInfo&);

    /** Reads whatever the file's type holds (WAV, AIFF, MP3, .m4a, FLAC, Ogg); the broadcast details from metadata. */
    FileInfo read (const juce::File&, const juce::StringPairArray& metadata);
}
