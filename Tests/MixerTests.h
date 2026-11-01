#pragma once

#include <JuceHeader.h>

class AudioEngine;

/** Meters and the mixer window: track and bus levels measured as the session mixes, and the
    mixer's strips editing the session. */
void runMixerTests (AudioEngine&);
