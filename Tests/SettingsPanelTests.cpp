#include <JuceHeader.h>
#include "../Source/SettingsPanel.h"
#include "TestUtils.h"
#include "SettingsPanelTests.h"

using TestUtils::check;

void runSettingsPanelTests()
{
    std::cout << "Settings panel:" << std::endl;

    // A model of two sections: the first with one row of each kind, reading and writing values
    // kept here; the second with a button. The panel shows the first to start with.
    bool on = false;
    int chosen = 1;
    double seconds = 2.5;
    int clicks = 0, made = 0;

    Settings::Model model
    {
        { "Editing", "How edits behave.",
          { Settings::Item::toggle ("Snap", "Snaps to things", [&] { return on; }, [&] (bool v) { on = v; }),
            Settings::Item::choice ("Shape", "The fade's shape", { "S-curve", "Linear", "Fast" }, [&] { return chosen; }, [&] (int v) { chosen = v; }),
            Settings::Item::number ("Pre-roll", "Before a punch-in", 0.0, 10.0, 0.1, 1, "s", [&] { return seconds; }, [&] (double v) { seconds = v; }),
            Settings::Item::component ("Device", 60, [&] { ++made; return std::make_unique<juce::Label> ("made", "device"); }) } },
        { "Keys", {},
          { Settings::Item::button ("Keyboard Shortcuts...", "Opens the keys", [&] { ++clicks; }) } }
    };

    Settings::Panel panel (std::move (model));
    panel.setSize (720, 480);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (20);   // the list box picks its first row

    auto* toggle = dynamic_cast<juce::ToggleButton*> (panel.getControl ("Snap"));
    auto* combo = dynamic_cast<juce::ComboBox*> (panel.getControl ("Shape"));
    auto* slider = dynamic_cast<juce::Slider*> (panel.getControl ("Pre-roll"));
    auto* device = dynamic_cast<juce::Label*> (panel.getControl ("Device"));
    check (panel.getNumSections() == 2 && panel.getShownSectionName() == "Editing" && toggle != nullptr && combo != nullptr && slider != nullptr
               && device != nullptr && made == 1 && panel.getControl ("Keyboard Shortcuts...") == nullptr,
           "settings panel: shows the first section with a switch, a choice, a number and a made component, and not the other page's button");

    // Each control starts from its setting, and a change writes through at once.
    const auto started = toggle != nullptr && combo != nullptr && slider != nullptr && ! toggle->getToggleState()
                         && combo->getSelectedItemIndex() == 1 && juce::exactlyEqual (slider->getValue(), 2.5);

    if (toggle != nullptr)
        toggle->triggerClick();

    if (combo != nullptr)
        combo->setSelectedItemIndex (2, juce::sendNotificationSync);

    if (slider != nullptr)
        slider->setValue (7.0, juce::sendNotificationSync);

    juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    check (started && on && chosen == 2 && juce::exactlyEqual (seconds, 7.0),
           "settings panel: each control starts from its setting, and a change writes through at once");

    // A change made elsewhere shows after a refresh.
    on = false;
    chosen = 0;
    seconds = 1.0;
    panel.refresh();
    check (toggle != nullptr && ! toggle->getToggleState() && combo != nullptr && combo->getSelectedItemIndex() == 0
               && slider != nullptr && juce::exactlyEqual (slider->getValue(), 1.0),
           "settings panel: a refresh reads every row again from its setting");

    // The other section by name: its button works; a name not there shows the first section.
    panel.showSection ("Keys");
    juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    auto* button = dynamic_cast<juce::TextButton*> (panel.getControl ("Keyboard Shortcuts..."));

    if (button != nullptr)
        button->triggerClick();

    juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    const auto keys = panel.getShownSectionName() == "Keys" && button != nullptr && clicks == 1 && panel.getControl ("Snap") == nullptr;
    panel.showSection ("Nowhere");
    juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
    check (keys && panel.getShownSectionName() == "Editing" && made == 2,
           "settings panel: a section shows by name with its button working, a name not there shows the first, and a page's component is made again when it shows");
}
