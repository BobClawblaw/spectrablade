#pragma once

#include <JuceHeader.h>

class AudioEngine;

/** Batch processing: collecting files, naming what is written, each step on real files, and the report. */
void runBatchTests (AudioEngine&);
