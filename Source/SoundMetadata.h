#pragma once

#include <JuceHeader.h>

/** Writes a sound's UCS category and tags into its WAV file, where other apps read them: the
    iXML chunk's USER block (CATID, CATEGORY, SUBCATEGORY, CATEGORYFULL and KEYWORDS, the tags
    joined with commas) and its ASWG block (catId, category, subCategory). The rest of the iXML is
    kept (as XML: its layout may change); a file without one gets one at its end.

    Safely: the new file is written beside the old one as a hidden temporary file, with every
    other chunk (the audio among them) copied byte for byte and in its order; it is then read
    back and checked (the same chunks, the audio and every other chunk identical, the iXML giving
    the category and tags written and keeping every other element, attribute and text of the old
    one, the audio readable at the same length, rate and channels), and
    only then renamed over the old file, in one step. Anything that fails leaves the old file as
    it was and no temporary file behind.

    Refused, saying why: files that aren't RIFF WAVE (AIFF, FLAC, MP3...), RF64 files, files whose
    chunks run past their end, an iXML that isn't XML or isn't plain UTF-8 text (a NUL or other
    control byte in it, as some recorders leave), and a file that would pass 4 GB. */
namespace SoundMetadata
{
    struct Fields
    {
        juce::String catId;             // a UCS CatID; empty leaves the file's category alone
        juce::StringArray tags;         // empty leaves the file's keywords alone
    };

    /** Empty when written; otherwise why not. */
    juce::String write (const juce::File& wav, const Fields&);

    /** The chunks of a RIFF WAVE file, in order: their ids and where their contents are. Empty
        for anything else (and for a file whose chunks run past its end). */
    struct Chunk
    {
        juce::String id;
        juce::int64 start = 0, size = 0;
    };

    std::vector<Chunk> chunksOf (const juce::File&);
}
