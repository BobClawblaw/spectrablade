#pragma once

class AudioEngine;

/** Effects on a 5.1 track: three stereo copies of each (L R, C LFE, Ls Rs) against the same
    effects on stereo tracks, following settings, automation and state, in mixdowns and in the engine. */
void runSurroundEffectsTests (AudioEngine&);

/** The effects rack's copies for a 5.1 master's channels past L and R, played to separate outputs
    of the open device (after the device tests). */
void runMasterRackDeviceTests (AudioEngine&);
