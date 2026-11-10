#include "SettingsPanel.h"
#include "Theme.h"

namespace Settings
{
    Item Item::toggle (juce::String label, juce::String help, std::function<bool()> get, std::function<void (bool)> set)
    {
        Item i;
        i.kind = Kind::toggle;
        i.label = std::move (label);
        i.help = std::move (help);
        i.getBool = std::move (get);
        i.setBool = std::move (set);
        return i;
    }

    Item Item::choice (juce::String label, juce::String help, juce::StringArray choices, std::function<int()> get, std::function<void (int)> set)
    {
        Item i;
        i.kind = Kind::choice;
        i.label = std::move (label);
        i.help = std::move (help);
        i.choices = std::move (choices);
        i.getChoice = std::move (get);
        i.setChoice = std::move (set);
        return i;
    }

    Item Item::number (juce::String label, juce::String help, double lowest, double highest, double step, int decimals, juce::String unit,
                       std::function<double()> get, std::function<void (double)> set)
    {
        Item i;
        i.kind = Kind::number;
        i.label = std::move (label);
        i.help = std::move (help);
        i.lowest = lowest;
        i.highest = highest;
        i.step = step;
        i.decimals = decimals;
        i.unit = std::move (unit);
        i.getNumber = std::move (get);
        i.setNumber = std::move (set);
        return i;
    }

    Item Item::button (juce::String label, juce::String help, std::function<void()> onClick)
    {
        Item i;
        i.kind = Kind::button;
        i.label = std::move (label);
        i.help = std::move (help);
        i.onClick = std::move (onClick);
        return i;
    }

    Item Item::component (juce::String label, int height, std::function<std::unique_ptr<juce::Component>()> make)
    {
        Item i;
        i.kind = Kind::component;
        i.label = std::move (label);
        i.componentHeight = height;
        i.makeComponent = std::move (make);
        return i;
    }

    //==============================================================================
    /** One row of a page: the label and the control for one item. */
    class Panel::Row final : public juce::Component
    {
    public:
        explicit Row (const Item& i) : item (i)
        {
            switch (item.kind)
            {
                case Item::Kind::toggle:
                    toggle = std::make_unique<juce::ToggleButton> (item.label);
                    toggle->setTooltip (item.help);
                    toggle->onClick = [this] { if (item.setBool != nullptr) item.setBool (toggle->getToggleState()); };
                    addAndMakeVisible (*toggle);
                    break;

                case Item::Kind::choice:
                    makeLabel();
                    combo = std::make_unique<juce::ComboBox>();
                    combo->addItemList (item.choices, 1);
                    combo->setTooltip (item.help);
                    combo->onChange = [this] { if (item.setChoice != nullptr && combo->getSelectedItemIndex() >= 0) item.setChoice (combo->getSelectedItemIndex()); };
                    addAndMakeVisible (*combo);
                    break;

                case Item::Kind::number:
                    makeLabel();
                    slider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
                    slider->setRange (item.lowest, item.highest, item.step);
                    slider->setNumDecimalPlacesToDisplay (item.decimals);
                    slider->setTextValueSuffix (item.unit.isEmpty() ? juce::String() : " " + item.unit);
                    slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 22);
                    slider->setTooltip (item.help);
                    slider->onValueChange = [this] { if (item.setNumber != nullptr) item.setNumber (slider->getValue()); };
                    addAndMakeVisible (*slider);
                    break;

                case Item::Kind::button:
                    button = std::make_unique<juce::TextButton> (item.label);
                    button->setTooltip (item.help);
                    button->onClick = [this] { if (item.onClick != nullptr) item.onClick(); };
                    addAndMakeVisible (*button);
                    break;

                case Item::Kind::component:
                    makeLabel();

                    if (item.makeComponent != nullptr)
                        made = item.makeComponent();

                    if (made != nullptr)
                        addAndMakeVisible (*made);
                    break;
            }

            refresh();
        }

        int getWantedHeight() const
        {
            if (item.kind == Item::Kind::component)
                return (label != nullptr ? rowHeight : 0) + item.componentHeight + 8;

            return rowHeight + (helpBelow() ? 18 : 0);
        }

        /** Reads the setting again; the control is only touched when it differs. */
        void refresh()
        {
            if (toggle != nullptr && item.getBool != nullptr)
            {
                if (const auto on = item.getBool(); on != toggle->getToggleState())
                    toggle->setToggleState (on, juce::dontSendNotification);
            }
            else if (combo != nullptr && item.getChoice != nullptr)
            {
                if (const auto index = juce::jlimit (0, item.choices.size() - 1, item.getChoice()); index != combo->getSelectedItemIndex())
                    combo->setSelectedItemIndex (index, juce::dontSendNotification);
            }
            else if (slider != nullptr && item.getNumber != nullptr)
            {
                if (const auto value = item.getNumber(); ! juce::exactlyEqual (value, slider->getValue()))
                    slider->setValue (value, juce::dontSendNotification);
            }
        }

        juce::Component* getControl() const
        {
            if (toggle != nullptr) return toggle.get();
            if (combo != nullptr)  return combo.get();
            if (slider != nullptr) return slider.get();
            if (button != nullptr) return button.get();
            return made.get();
        }

        const juce::String& getLabel() const noexcept   { return item.label; }

        void paint (juce::Graphics& g) override
        {
            if (helpBelow())
            {
                g.setColour (Theme::dimText);
                g.setFont (juce::FontOptions (12.0f));
                g.drawText (item.help, getLocalBounds().removeFromBottom (18).withTrimmedLeft (item.kind == Item::Kind::toggle ? 26 : 0),
                            juce::Justification::centredLeft, true);
            }
        }

        void resized() override
        {
            auto r = getLocalBounds();

            if (helpBelow())
                r.removeFromBottom (18);

            switch (item.kind)
            {
                case Item::Kind::toggle:    toggle->setBounds (r); break;
                case Item::Kind::button:    button->setBounds (r.removeFromLeft (juce::jmin (r.getWidth(), 240)).reduced (0, 3)); break;
                case Item::Kind::choice:
                    label->setBounds (r.removeFromLeft (labelWidth));
                    combo->setBounds (r.removeFromLeft (juce::jmin (r.getWidth(), 260)).reduced (0, 3));
                    break;
                case Item::Kind::number:
                    label->setBounds (r.removeFromLeft (labelWidth));
                    slider->setBounds (r.removeFromLeft (juce::jmin (r.getWidth(), 320)));
                    break;
                case Item::Kind::component:
                    if (label != nullptr)
                        label->setBounds (r.removeFromTop (rowHeight));

                    if (made != nullptr)
                        made->setBounds (r.withTrimmedBottom (8));
                    break;
            }
        }

    private:
        static constexpr int labelWidth = 200;

        void makeLabel()
        {
            label = std::make_unique<juce::Label> (juce::String(), item.label);
            label->setTooltip (item.help);
            addAndMakeVisible (*label);
        }

        /** A toggle's help shows under it; a choice's or number's too, since the tooltip alone is
            easy to miss. A button's is its tooltip. */
        bool helpBelow() const                      { return item.help.isNotEmpty() && item.kind != Item::Kind::button && item.kind != Item::Kind::component; }

        Item item;
        std::unique_ptr<juce::Label> label;
        std::unique_ptr<juce::ToggleButton> toggle;
        std::unique_ptr<juce::ComboBox> combo;
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::TextButton> button;
        std::unique_ptr<juce::Component> made;
    };

    //==============================================================================
    Panel::Panel (Model m) : model (std::move (m))
    {
        sections.setModel (this);
        sections.setRowHeight (28);
        sections.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (sections);

        intro.setFont (juce::FontOptions (13.0f));
        intro.setMinimumHorizontalScale (1.0f);
        intro.setJustificationType (juce::Justification::topLeft);
        Theme::follow (intro, juce::Label::textColourId, Theme::dimText);
        addAndMakeVisible (intro);

        viewport.setViewedComponent (&page, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        if (! model.empty())
            sections.selectRow (0);
    }

    Panel::~Panel()
    {
        sections.setModel (nullptr);
    }

    void Panel::showSection (const juce::String& name)
    {
        for (size_t i = 0; i < model.size(); ++i)
        {
            if (model[i].name == name)
            {
                sections.selectRow ((int) i);
                return;
            }
        }

        if (! model.empty())
            sections.selectRow (0);
    }

    juce::String Panel::getShownSectionName() const
    {
        return juce::isPositiveAndBelow (shown, (int) model.size()) ? model[(size_t) shown].name : juce::String();
    }

    void Panel::refresh()
    {
        for (auto* row : rows)
            row->refresh();
    }

    juce::Component* Panel::getControl (const juce::String& label) const
    {
        for (auto* row : rows)
            if (row->getLabel() == label)
                return row->getControl();

        return nullptr;
    }

    void Panel::paint (juce::Graphics& g)
    {
        g.fillAll (Theme::base);
        g.setColour (Theme::surface);
        g.fillRect (getLocalBounds().removeFromLeft (sidebarWidth));
        g.setColour (Theme::line);
        g.drawVerticalLine (sidebarWidth, 0.0f, (float) getHeight());
    }

    void Panel::resized()
    {
        auto r = getLocalBounds();
        sections.setBounds (r.removeFromLeft (sidebarWidth).reduced (0, 8));
        r.reduce (pageMargin, pageMargin);

        const auto introHeight = juce::isPositiveAndBelow (shown, (int) model.size()) && model[(size_t) shown].intro.isNotEmpty() ? 40 : 0;
        intro.setBounds (r.removeFromTop (introHeight));

        if (introHeight > 0)
            r.removeFromTop (8);

        viewport.setBounds (r);
        auto y = 0;
        const auto width = r.getWidth() - (viewport.isVerticalScrollBarShown() ? viewport.getScrollBarThickness() : 0);

        for (auto* row : rows)
        {
            row->setBounds (0, y, width, row->getWantedHeight());
            y += row->getWantedHeight() + 4;
        }

        page.setSize (width, y);
    }

    void Panel::paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height, bool selected)
    {
        if (! juce::isPositiveAndBelow (rowNumber, (int) model.size()))
            return;

        if (selected)
        {
            g.setColour (Theme::accentDeep);
            g.fillRoundedRectangle (juce::Rectangle<int> (6, 2, width - 12, height - 4).toFloat(), 5.0f);
        }

        g.setColour (selected ? Theme::textStrong : Theme::text);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText (model[(size_t) rowNumber].name, 16, 0, width - 20, height, juce::Justification::centredLeft, true);
    }

    void Panel::selectedRowsChanged (int lastRowSelected)
    {
        if (lastRowSelected == shown || ! juce::isPositiveAndBelow (lastRowSelected, (int) model.size()))
            return;

        shown = lastRowSelected;
        buildPage();
    }

    void Panel::buildPage()
    {
        rows.clear();
        page.removeAllChildren();

        if (! juce::isPositiveAndBelow (shown, (int) model.size()))
            return;

        const auto& section = model[(size_t) shown];
        intro.setText (section.intro, juce::dontSendNotification);

        for (const auto& item : section.items)
            page.addAndMakeVisible (rows.add (new Row (item)));

        resized();
    }
}
