#pragma once

class AudioEngine;

/** Instrument tracks: note clips in the session (their MIDI, the edits, the file), an instrument
    first in a track's chain played by them through the mixer, its automation, and the offline
    chains that bounces and mixdowns load. */
void runInstrumentTrackTests (AudioEngine&);
