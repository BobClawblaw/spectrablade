#pragma once

#include <JuceHeader.h>

/** File > Open Recent: the last files and sessions opened, newest first, kept in the settings
    between launches. */
namespace RecentFiles
{
    inline constexpr int maxFiles = 10;
    inline constexpr const char* settingsKey = "recentFiles";

    /** Reads the list kept in the settings. */
    inline void restore (juce::RecentlyOpenedFilesList& list, juce::PropertiesFile& settings)
    {
        list.setMaxNumberOfItems (maxFiles);
        list.restoreFromString (settings.getValue (settingsKey));
    }

    /** Puts a file at the top of the list (moving it there when it is already on it) and keeps
        the list in the settings. */
    inline void note (juce::RecentlyOpenedFilesList& list, const juce::File& file, juce::PropertiesFile& settings)
    {
        list.setMaxNumberOfItems (maxFiles);
        list.addFile (file);
        settings.setValue (settingsKey, list.toString());
    }

    inline void clear (juce::RecentlyOpenedFilesList& list, juce::PropertiesFile& settings)
    {
        list.clear();
        settings.setValue (settingsKey, list.toString());
    }

    /** The Open Recent submenu: an item for each file that still exists, with the id baseId plus
        its place in the list, then Clear Menu (baseId + maxFiles). */
    inline juce::PopupMenu createMenu (juce::RecentlyOpenedFilesList& list, int baseId)
    {
        juce::PopupMenu menu;
        list.createPopupMenuItems (menu, baseId, false, true);

        if (list.getNumFiles() > 0)
        {
            menu.addSeparator();
            menu.addItem (baseId + maxFiles, "Clear Menu");
        }

        return menu;
    }

    inline bool isSession (const juce::File& file)        { return file.hasFileExtension ("sbsession"); }
}
