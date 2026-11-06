#pragma once

#include <JuceHeader.h>

class AudioEngine;

/** 5.1 files: channel names, folding down, splitting and combining channels, saving, playing
    folded down or to separate outputs, and the outputs connected for them. */
void runSurroundTests (AudioEngine&);

/** With the interface open: the outputs a 5.1 file playing to separate outputs connects. */
void runSurroundDeviceTests (AudioEngine&);
