#pragma once

#include <JuceHeader.h>

/** The Sound Library's index: scanning folders, rescanning what changed, searching, sorting and
    paging, and how fast a search is on an index of a million sounds. */
void runSoundLibraryTests();

/** A real library: scans the folder into a new index (in a temporary folder, deleted after),
    scans it again with nothing changed, and times searches, printing as it goes. Only reads the
    library's files. */
void runSoundLibraryOn (const juce::File& folder);
