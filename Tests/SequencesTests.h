#pragma once

#include <JuceHeader.h>

class AudioEngine;

/** Recorded command sequences: each step against its command's own code, running a sequence,
    saving and loading, the store, the recorder, and a sequence in a batch. */
void runSequencesTests (AudioEngine&);
