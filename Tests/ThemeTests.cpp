#include <JuceHeader.h>
#include "../Source/Theme.h"
#include "TestUtils.h"
#include "ThemeTests.h"

namespace
{
    using TestUtils::check;

    void testContrast()
    {
        check (std::abs (Theme::contrast (juce::Colours::black, juce::Colours::white) - 21.0) < 1.0e-9
                   && std::abs (Theme::contrast (Theme::base, Theme::base) - 1.0) < 1.0e-9,
               "theme: contrast runs from 1 (the same colour) to 21 (black on white)");

        auto least = 100.0;

        for (auto back : { Theme::inset, Theme::base, Theme::surface, Theme::raised, Theme::hover })
            least = juce::jmin (least, Theme::contrast (Theme::text, back));

        check (least >= 7.0, "theme: body text reads at 7:1 or better on every surface (least " + juce::String (least, 1) + ")");

        least = 100.0;

        for (auto back : { Theme::inset, Theme::base, Theme::surface, Theme::raised })
            least = juce::jmin (least, Theme::contrast (Theme::dimText, back));

        check (least >= 4.5, "theme: secondary text reads at 4.5:1 or better on every surface but hover (least " + juce::String (least, 1) + ")");

        const auto selectedRow = Theme::surface.overlaidWith (Theme::selection);
        const auto pairs = { Theme::contrast (Theme::textStrong, Theme::accentDeep), Theme::contrast (Theme::textStrong, selectedRow),
                             Theme::contrast (Theme::accentLight, Theme::inset), Theme::contrast (Theme::accent, Theme::base),
                             Theme::contrast (Theme::cursor, Theme::base) };
        check (std::all_of (pairs.begin(), pairs.end(), [] (double c) { return c >= 4.5; }),
               "theme: text on a button that is on, on a selected row, the clock, the accent and the cursor all read at 4.5:1 or better");

        check (Theme::contrast (Theme::lineStrong, Theme::raised) >= 1.5 && Theme::contrast (Theme::faintText, Theme::surface) >= 3.0,
               "theme: button edges stand off their fill, and disabled text still reads at 3:1");
    }

    void testControls()
    {
        Theme::LookAndFeel look;
        juce::TextButton button ("Open...");
        button.setLookAndFeel (&look);
        button.setSize (90, 26);
        const auto off = button.createComponentSnapshot (button.getLocalBounds()).getPixelAt (45, 4);
        button.setToggleState (true, juce::dontSendNotification);
        const auto on = button.createComponentSnapshot (button.getLocalBounds()).getPixelAt (45, 4);
        const auto edge = button.createComponentSnapshot (button.getLocalBounds()).getPixelAt (45, 0);
        check (off == Theme::raised && on == Theme::accentDeep && edge.getBlue() > edge.getRed() + 40,
               "theme: a button is the raised colour, and when on the deep accent with an accent edge");

        juce::ComboBox box;
        box.setLookAndFeel (&look);
        box.addItem ("Analog 1 / Analog 2", 1);
        box.setSelectedId (1);
        box.setSize (200, 26);
        juce::TextEditor field;
        field.setLookAndFeel (&look);
        field.setSize (200, 24);
        juce::ScrollBar bar (true);
        bar.setLookAndFeel (&look);
        bar.setSize (10, 200);
        bar.setRangeLimits (0.0, 100.0);
        bar.setCurrentRange (0.0, 25.0);
        const auto fieldPixel = field.createComponentSnapshot (field.getLocalBounds()).getPixelAt (100, 12);
        const auto boxPixel = box.createComponentSnapshot (box.getLocalBounds()).getPixelAt (4, 4);
        const auto thumb = bar.createComponentSnapshot (bar.getLocalBounds()).getPixelAt (5, 20);
        check (fieldPixel == Theme::inset && boxPixel == Theme::raised && thumb == Theme::lineStrong,
               "theme: text fields sit in the inset colour, menus on the raised one, and scroll bars a quiet grey");

        const auto tip = look.getTooltipBounds ("Show or hide the panel metering what goes to the outputs", { 100, 100 }, { 0, 0, 1000, 800 });
        check (tip.getWidth() > 100 && tip.getWidth() <= 360 + 2 * Theme::gap + 2 && tip.getX() > 100 && tip.getY() > 100,
               "theme: a tooltip sits beside the mouse, its lines kept to a readable width");

        for (auto* c : std::initializer_list<juce::Component*> { &button, &box, &field, &bar })
            c->setLookAndFeel (nullptr);
    }
}

void runThemeTests()
{
    testContrast();
    testControls();
}
