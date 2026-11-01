#pragma once

#include <JuceHeader.h>

/** Recording onto session tracks: clearing a range, placing a take, a punch-in and a loop
    recording through the real session player looped back into the recorder, and takes. */
void runTrackRecordingTests (juce::AudioFormatManager&);
