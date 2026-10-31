#pragma once

#include "../Source/AudioEngine.h"

/** Third-party VST3 and Audio Unit hosting through the built app: the scan worker's load-and-play
    check, a broken plug-in marked not working without a prompt, and real plug-ins processing audio
    in the rack. */
void runPluginTests (AudioEngine&);

/** Scans every plug-in on this machine the way the app does and writes a report (opt-in: it takes
    a while, and some plug-ins open licence windows). Returns the number not working. */
int runPluginSweep (AudioEngine&, const juce::File& report);
