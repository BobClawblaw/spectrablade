#pragma once

#include <JuceHeader.h>

/** Named presets for effects. A preset is a set of named values (each a setting's plain value: Hz,
    dB, ms, a percentage, a choice's index, 1 or 0 for on or off), kept under the effect's key in a
    file of its own. An effect's dialog and its live built-in effect use the same key and the same
    names for their settings (the live effect's parameter IDs), so a preset saved in one loads in
    the other. Values round-trip exactly. */
namespace EffectPresets
{
    using Values = std::map<juce::String, double>;

    class Store
    {
    public:
        /** Presets kept as one file an effect in this folder (made when the first is saved). */
        explicit Store (juce::File folder);

        /** The effect's presets' names, sorted, ignoring case. */
        juce::StringArray getNames (const juce::String& effectKey) const;

        std::optional<Values> load (const juce::String& effectKey, const juce::String& name) const;

        /** Saves (or replaces) the named preset; false if the file can't be written or the name is empty. */
        bool save (const juce::String& effectKey, const juce::String& name, const Values&);

        /** Removes the named preset; false if there was none or the file can't be written. */
        bool remove (const juce::String& effectKey, const juce::String& name);

        juce::File getFolder() const                    { return folder; }

    private:
        juce::File fileFor (const juce::String& effectKey) const;
        std::unique_ptr<juce::XmlElement> read (const juce::String& effectKey) const;

        juce::File folder;
    };

    /** The user's presets, in the app's data folder (Application Support/SpectraBlade/Presets). */
    Store& userStore();

    /** A row for a dialog or window: the effect's presets in a menu (choosing one loads it through
        `apply`), Save... (asks for a name and saves `current()`) and Delete (asks, then removes the
        one chosen). */
    class Bar final : public juce::Component
    {
    public:
        Bar (Store&, juce::String effectKey, std::function<Values()> current, std::function<void (const Values&)> apply);
        ~Bar() override;

        /** Reads the names again (another window may have saved one). */
        void refresh();

        void resized() override;

    private:
        void saveAs();
        void removeChosen();

        Store& store;
        juce::String key;
        std::function<Values()> currentValues;
        std::function<void (const Values&)> applyValues;
        juce::ComboBox list;
        juce::TextButton save { "Save..." }, remove { "Delete" };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Bar)
    };
}
