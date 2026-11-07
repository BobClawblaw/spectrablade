#pragma once

#include <JuceHeader.h>

/** The Sound Library's index: scanning folders, rescanning what changed, searching, sorting and
    paging, and how fast a search is on an index of a million sounds. */
void runSoundLibraryTests();

/** A real library: scans the folder into a new index (in a temporary folder, deleted after),
    scans it again with nothing changed, and times searches, printing as it goes. Only reads the
    library's files. */
void runSoundLibraryOn (const juce::File& folder);

/** Real files: copies the first two WAV files in each folder at the top of this one into a
    temporary folder (deleted after), writes a category and tags into each copy, and checks every
    other chunk is kept byte for byte. The library's own files are only read. */
void runWriteCheckOn (const juce::File& folder);
