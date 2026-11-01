#pragma once

#include <JuceHeader.h>

/** The user's own keyboard shortcuts, kept in the settings as the changes from the defaults, so
    shortcuts added in later versions still arrive. */
namespace KeyShortcuts
{
    constexpr const char* settingsKey = "keyMappings";

    /** Saves the mappings that differ from the defaults (removing the entry when none do). */
    void store (juce::KeyPressMappingSet&, juce::PropertiesFile&);

    /** Applies the saved changes over the defaults. False when there were none to apply. */
    bool restore (juce::KeyPressMappingSet&, juce::PropertiesFile&);
}
