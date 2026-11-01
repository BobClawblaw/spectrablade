#pragma once

#include <JuceHeader.h>

/** Recording onto session tracks: clearing a range, placing a take, a punch-in and a loop
    recording through the real session player looped back into the recorder, takes, input
    monitoring through the player, and recording several tracks at once. */
void runTrackRecordingTests (juce::AudioFormatManager&);
