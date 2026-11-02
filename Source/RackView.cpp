#include "RackView.h"

//==============================================================================
class RackView::SlotRow final : public juce::Component
{
public:
    SlotRow (RackView& o, int slotIndex) : owner (o), index (slotIndex)
    {
        auto& rackEngine = owner.engine;
        auto* plugin = rackEngine.getRackPlugin (index);
        const auto description = plugin != nullptr ? plugin->getPluginDescription() : juce::PluginDescription();

        name.setText (juce::String (index + 1) + ". " + description.name, juce::dontSendNotification);
        name.setTooltip (description.manufacturerName + " - " + description.pluginFormatName);
        format.setText (description.pluginFormatName, juce::dontSendNotification);
        format.setColour (juce::Label::textColourId, juce::Colours::grey);

        bypass.setToggleState (! rackEngine.isRackSlotBypassed (index), juce::dontSendNotification);
        bypass.setTooltip ("On/off (bypass)");
        bypass.onClick = [this] { owner.engine.setRackSlotBypassed (index, ! bypass.getToggleState()); };

        edit.onClick   = [this] { if (owner.onEditSlot != nullptr) owner.onEditSlot (index); };
        up.onClick     = [this] { owner.engine.moveRackSlot (index, index - 1); };
        down.onClick   = [this] { owner.engine.moveRackSlot (index, index + 1); };
        remove.onClick = [this] { owner.engine.removeRackSlot (index); };

        up.setEnabled (index > 0);
        down.setEnabled (index < rackEngine.getNumRackSlots() - 1);

        for (auto* c : std::initializer_list<juce::Component*> { &bypass, &name, &format, &edit, &up, &down, &remove })
            addAndMakeVisible (c);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff23272e));
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 4);
        bypass.setBounds (r.removeFromLeft (28));
        remove.setBounds (r.removeFromRight (28));
        down.setBounds (r.removeFromRight (28));
        up.setBounds (r.removeFromRight (28));
        edit.setBounds (r.removeFromRight (48).reduced (2, 0));
        format.setBounds (r.removeFromRight (44));
        name.setBounds (r);
    }

private:
    RackView& owner;
    const int index;
    juce::ToggleButton bypass;
    juce::Label name, format;
    juce::TextButton edit { "Edit" }, up { juce::String::fromUTF8 ("\xe2\x96\xb2") },
                     down { juce::String::fromUTF8 ("\xe2\x96\xbc") }, remove { "x" };
};

//==============================================================================
RackView::RackView (AudioEngine& e) : engine (e)
{
    title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    addAndMakeVisible (title);
    addAndMakeVisible (addButton);
    addButton.onClick = [this] { showAddMenu(); };

    engine.addChangeListener (this);
    rebuildRows();
}

RackView::~RackView()
{
    engine.removeChangeListener (this);
}

void RackView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1b1e23));

    if (rows.isEmpty())
    {
        g.setColour (juce::Colours::grey);
        g.drawFittedText ("No effects.\nScan your plug-ins, then add VST3 or Audio Unit effects here.",
                          getLocalBounds().reduced (16).withTrimmedTop (80), juce::Justification::centredTop, 4);
    }
}

void RackView::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto header = r.removeFromTop (28);
    addButton.setBounds (header.removeFromRight (110));
    title.setBounds (header);
    r.removeFromTop (8);

    for (auto* row : rows)
    {
        row->setBounds (r.removeFromTop (36));
        r.removeFromTop (4);
    }
}

void RackView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // The engine also speaks for device changes. Rows are rebuilt only when the rack itself changed,
    // since a row replaced between a button's press and release loses the click.
    std::vector<std::pair<const void*, bool>> now;

    for (int i = 0; i < engine.getNumRackSlots(); ++i)
        now.emplace_back (engine.getRackPlugin (i), engine.isRackSlotBypassed (i));

    if (now != shown)
        rebuildRows();
}

void RackView::rebuildRows()
{
    rows.clear();
    shown.clear();

    for (int i = 0; i < engine.getNumRackSlots(); ++i)
        shown.emplace_back (engine.getRackPlugin (i), engine.isRackSlotBypassed (i));

    for (int i = 0; i < engine.getNumRackSlots(); ++i)
        addAndMakeVisible (rows.add (new SlotRow (*this, i)));

    resized();
    repaint();
}

void RackView::showAddMenu()
{
    // Effects only: instruments need MIDI and belong on tracks, which come in a later milestone.
    juce::Array<juce::PluginDescription> effects;

    for (auto& type : engine.knownPlugins.getTypes())
        if (! type.isInstrument)
            effects.add (type);

    if (effects.isEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "No effects found",
                                                "Use Plug-ins... to scan for VST3 and Audio Unit effects first.");
        return;
    }

    juce::PopupMenu menu;
    juce::KnownPluginList::addToMenu (menu, effects, juce::KnownPluginList::sortByManufacturer);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addButton),
        [this, effects] (int result)
        {
            const auto chosen = juce::KnownPluginList::getIndexChosenByMenu (effects, result);

            if (! juce::isPositiveAndBelow (chosen, effects.size()))
                return;

            const auto name = effects[chosen].name;

            engine.addPlugin (effects[chosen], [name] (const juce::String& error)
            {
                if (error.isNotEmpty())
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                            "Could not load " + name, error);
            });
        });
}
