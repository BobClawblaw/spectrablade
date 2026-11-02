#pragma once

#include <JuceHeader.h>

class AudioEngine;

/** Auditioning files in the open panel: the player's audition output, the engine's file
    audition, and the panel's Preview strip. */
void runAuditionTests (AudioEngine&);
