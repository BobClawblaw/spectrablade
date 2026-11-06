#pragma once

class AudioEngine;

/** A session's 5.1 master: the surround panner's gains, the master in playback (folded down or
    to separate outputs) and in a mixdown, buses and interface pairs in it, the edits, the session
    file and track templates, bounces and freezing, and the mixer strips' surround controls. */
void runSurroundSessionTests (AudioEngine&);
