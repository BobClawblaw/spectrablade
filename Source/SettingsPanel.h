#pragma once

#include <JuceHeader.h>

/** One place for every setting of the app: sections down the left, each a page of rows. A row is
    a switch, a choice, a number or a button, each reading and writing its setting through the
    functions it is given, so the panel holds no settings of its own: what it shows is what the
    app does, and a change takes effect at once (there is no OK). */
namespace Settings
{
    struct Item
    {
        enum class Kind { toggle, choice, number, button, component };

        Kind kind = Kind::toggle;
        juce::String label;                          // beside the control
        juce::String help;                           // the tooltip, and a line under the label when it is long
        std::function<bool()> getBool;               // toggle
        std::function<void (bool)> setBool;
        juce::StringArray choices;                   // choice
        std::function<int()> getChoice;
        std::function<void (int)> setChoice;
        std::function<double()> getNumber;           // number
        std::function<void (double)> setNumber;
        double lowest = 0.0, highest = 1.0, step = 1.0;
        int decimals = 0;
        juce::String unit;                           // after a number
        std::function<void()> onClick;               // button
        std::function<std::unique_ptr<juce::Component>()> makeComponent;   // component: made when the page shows
        int componentHeight = 0;

        static Item toggle (juce::String label, juce::String help, std::function<bool()> get, std::function<void (bool)> set);
        static Item choice (juce::String label, juce::String help, juce::StringArray choices, std::function<int()> get, std::function<void (int)> set);
        static Item number (juce::String label, juce::String help, double lowest, double highest, double step, int decimals, juce::String unit,
                            std::function<double()> get, std::function<void (double)> set);
        static Item button (juce::String label, juce::String help, std::function<void()> onClick);
        static Item component (juce::String label, int height, std::function<std::unique_ptr<juce::Component>()> make);
    };

    struct Section
    {
        juce::String name;
        juce::String intro;   // a line at the top of the page
        std::vector<Item> items;
    };

    using Model = std::vector<Section>;

    class Panel final : public juce::Component,
                        private juce::ListBoxModel
    {
    public:
        explicit Panel (Model model);
        ~Panel() override;

        /** Shows a section by its name (the first when it isn't there). */
        void showSection (const juce::String& name);
        int getShownSection() const noexcept       { return shown; }
        juce::String getShownSectionName() const;
        int getNumSections() const noexcept        { return (int) model.size(); }

        /** Reads every row of the shown page again from its setting (after a change made elsewhere). */
        void refresh();

        /** The row's control, for the tests: a ToggleButton, ComboBox, Slider or TextButton, or
            the component made for a component row; nullptr when not on the shown page. */
        juce::Component* getControl (const juce::String& label) const;

        void paint (juce::Graphics&) override;
        void resized() override;

        static constexpr int sidebarWidth = 160, rowHeight = 30, pageMargin = 16;

    private:
        class Row;

        int getNumRows() override                   { return (int) model.size(); }
        void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
        void selectedRowsChanged (int lastRowSelected) override;

        void buildPage();

        Model model;
        int shown = -1;
        juce::ListBox sections;
        juce::Label intro;
        juce::Viewport viewport;
        juce::Component page;
        juce::OwnedArray<Row> rows;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
    };
}
