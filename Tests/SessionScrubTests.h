#pragma once

class AudioEngine;

/** Scrubbing a stopped session: its mix against scrubbing a file of the same audio, effects left
    out, frozen tracks and a 5.1 master, the player, and the multitrack view's time ruler. */
void runSessionScrubTests (AudioEngine&);
