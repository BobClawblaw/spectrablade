#pragma once

#include <JuceHeader.h>

/** Menus of plug-ins to add: a submenu a format (Audio Units, then VST3, then any other), each with
    a submenu a maker, the plug-ins sorted by name. Items take ids from 1 (one more than the
    plug-in's index in the list given), so a menu can put its own items above them. */
namespace PluginMenu
{
    inline juce::String formatTitle (const juce::String& format)
    {
        return format == "AudioUnit" ? juce::String ("Audio Units") : format;
    }

    inline void add (juce::PopupMenu& menu, const juce::Array<juce::PluginDescription>& types)
    {
        juce::StringArray formats;

        for (auto& t : types)
            formats.addIfNotAlreadyThere (t.pluginFormatName);

        const auto rank = [] (const juce::String& f) { return f == "AudioUnit" ? 0 : f == "VST3" ? 1 : 2; };
        std::stable_sort (formats.begin(), formats.end(), [&rank] (const juce::String& a, const juce::String& b)
        {
            return rank (a) != rank (b) ? rank (a) < rank (b) : a.compareIgnoreCase (b) < 0;
        });

        for (auto& format : formats)
        {
            std::map<juce::String, std::vector<int>, std::function<bool (const juce::String&, const juce::String&)>> makers (
                [] (const juce::String& a, const juce::String& b) { return a.compareIgnoreCase (b) < 0; });

            for (int i = 0; i < types.size(); ++i)
                if (types.getReference (i).pluginFormatName == format)
                    makers[types.getReference (i).manufacturerName.trim().isEmpty() ? juce::String ("Unknown maker")
                                                                                     : types.getReference (i).manufacturerName.trim()].push_back (i);

            juce::PopupMenu byMaker;

            for (auto& [maker, indices] : makers)
            {
                std::sort (indices.begin(), indices.end(), [&types] (int a, int b)
                {
                    return types.getReference (a).name.compareIgnoreCase (types.getReference (b).name) < 0;
                });

                juce::PopupMenu plugins;

                for (auto i : indices)
                    plugins.addItem (i + 1, types.getReference (i).name);

                byMaker.addSubMenu (maker, plugins);
            }

            menu.addSubMenu (formatTitle (format), byMaker);
        }
    }

    /** The index in types of the plug-in a menu result picked, or -1 for anything else. */
    inline int chosen (const juce::Array<juce::PluginDescription>& types, int result)
    {
        return juce::isPositiveAndBelow (result - 1, types.size()) ? result - 1 : -1;
    }
}
