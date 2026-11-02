#pragma once

class AudioEngine;

/** The built-in effects as plug-ins: created through the format manager, played block by block
    against the offline edits, saved and restored, and in the rack. */
void runBuiltInEffectsTests (AudioEngine&);
