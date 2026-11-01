#pragma once

#include <JuceHeader.h>

/** The metronome: its beat grid and click, clicks through the real session player (on, during a
    count-in, through a loop), snapping to its beats, and the tempo in session files. */
void runMetronomeTests (juce::AudioFormatManager&);
