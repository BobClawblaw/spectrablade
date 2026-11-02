#pragma once

#include <JuceHeader.h>

/** The user's own keyboard shortcuts, kept in the settings as the changes from the defaults, so
    shortcuts added in later versions still arrive; and the window for changing them. */
namespace KeyShortcuts
{
    constexpr const char* settingsKey = "keyMappings";
    constexpr const char* fileSuffix = ".sbkeys";

    /** Saves the mappings that differ from the defaults (removing the entry when none do). */
    void store (juce::KeyPressMappingSet&, juce::PropertiesFile&);

    /** Applies the saved changes over the defaults. False when there were none to apply. */
    bool restore (juce::KeyPressMappingSet&, juce::PropertiesFile&);

    /** A command as the window lists it. */
    struct Row
    {
        juce::CommandID id = 0;
        juce::String name, menu, description;
        juce::Array<juce::KeyPress> keys;
        bool changed = false;    // its keys differ from its defaults
        bool readOnly = false;   // the command's keys can't be changed
    };

    /** The commands, by menu then name, that match every word of the query (in the name, the menu,
        the description or a key, ignoring case); only those changed from the defaults if asked.
        Commands hidden from key editors are left out. */
    std::vector<Row> rows (juce::ApplicationCommandManager&, const juce::String& query, bool changedOnly);

    /** How a key reads, e.g. "Shift + Cmd + S". */
    juce::String describe (const juce::KeyPress&);

    /** Gives the key to the command (taking it from any command that had it), replacing the key at
        `replaceIndex`, or adding it when that is -1. Returns the command it was taken from, or 0. */
    juce::CommandID assign (juce::KeyPressMappingSet&, juce::CommandID, const juce::KeyPress&, int replaceIndex = -1);

    /** Puts back the command's default keys. A default now used by another command stays with it;
        those keys are returned. */
    juce::Array<juce::KeyPress> resetCommand (juce::ApplicationCommandManager&, juce::CommandID);

    /** Writes the changes from the defaults to a file (an empty set when there are none). */
    juce::Result exportTo (juce::KeyPressMappingSet&, const juce::File&);

    /** Replaces the shortcuts with a file's: the defaults with its changes over them. A file that
        isn't a shortcut set changes nothing. */
    juce::Result importFrom (juce::KeyPressMappingSet&, const juce::File&);

    /** The window's contents: a search box, the commands with their keys, and for the chosen one
        its keys to change, add or remove; Import, Export and Reset All. */
    class Editor final : public juce::Component,
                         private juce::ListBoxModel,
                         private juce::ChangeListener
    {
    public:
        explicit Editor (juce::ApplicationCommandManager&);
        ~Editor() override;

        /** Lists the commands again (after a key change elsewhere or a new search). */
        void refresh();

        juce::TextEditor& getSearchBox() noexcept                 { return search; }
        juce::ListBox& getList() noexcept                         { return list; }
        int getNumShown() const noexcept                          { return (int) shown.size(); }
        void choose (juce::CommandID);
        juce::CommandID getChosen() const noexcept                { return chosen; }

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        class KeyEntry;

        int getNumRows() override                                 { return (int) shown.size(); }
        void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
        void selectedRowsChanged (int lastRowSelected) override;
        void changeListenerCallback (juce::ChangeBroadcaster*) override  { refresh(); }

        void showDetails();
        void askForKey (int replaceIndex);
        void importSet();
        void exportSet();

        juce::ApplicationCommandManager& commands;
        juce::TextEditor search;
        juce::ToggleButton changedOnly { "Changed only" };
        juce::ListBox list { "Commands", this };
        juce::Label title, description;
        juce::OwnedArray<juce::TextButton> keyButtons;
        juce::TextButton addKey { "Add Key..." }, resetOne { "Reset" }, importButton { "Import..." }, exportButton { "Export..." },
                         resetAll { "Reset All..." };
        std::vector<Row> shown;
        juce::CommandID chosen = 0;
        std::unique_ptr<juce::FileChooser> chooser;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Editor)
    };
}
