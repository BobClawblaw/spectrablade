#pragma once

#include <JuceHeader.h>

/** Words that differ between the systems SpectraBlade runs on, for text the user reads. */
namespace Platform
{
    /** The system's file browser: "Finder" on macOS, "Explorer" on Windows. */
    inline juce::String fileManager()
    {
       #if JUCE_MAC
        return "Finder";
       #elif JUCE_WINDOWS
        return "Explorer";
       #else
        return "File Manager";
       #endif
    }

    /** The menu item that shows a file where it is kept. */
    inline juce::String showInFileManager()     { return "Show in " + fileManager(); }

    /** The system, as a name in a sentence. */
    inline juce::String systemName()
    {
       #if JUCE_MAC
        return "macOS";
       #elif JUCE_WINDOWS
        return "Windows";
       #else
        return "the system";
       #endif
    }

    /** The plug-in formats SpectraBlade hosts here, as a phrase: "VST3 and Audio Unit" or "VST3". */
    inline juce::String pluginFormats()
    {
       #if JUCE_MAC
        return "VST3 and Audio Unit";
       #else
        return "VST3";
       #endif
    }

    /** Text naming keys the Mac's way ("Cmd-K", "Option-drag"), with this system's names for them:
        on Windows and Linux the command key is Ctrl and Option is Alt. */
    inline juce::String keys (const juce::String& text)
    {
       #if JUCE_MAC
        return text;
       #else
        return text.replace ("Cmd", "Ctrl").replace ("Option", "Alt");
       #endif
    }
}
