#include <JuceHeader.h>
#include "../Source/Theme.h"
#include "../Source/WaveformView.h"
#include "../Source/AudioEdits.h"
#include "../Source/AudioEngine.h"
#include "../Source/TransportBar.h"
#include "../Source/OpenFiles.h"
#include "../Source/Dock.h"
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

        // Every theme with every accent: text is checked as text (WCAG 4.5:1, body text 7:1), and
        // lines, handles, the cursor and the waveform as graphics (3:1).
        juce::StringArray failures;
        auto leastBody = 100.0, leastDim = 100.0;

        for (auto& name : Theme::themeNames())
            for (auto& accentName : Theme::accentNames())
            {
                const auto p = Theme::make (name, accentName);
                const auto fail = [&] (const juce::String& what, double ratio, double needed)
                {
                    if (ratio < needed)
                        failures.add (name + "/" + accentName + " " + what + " " + juce::String (ratio, 2));
                };

                for (auto back : { p.inset, p.base, p.surface, p.raised, p.hover })
                {
                    leastBody = juce::jmin (leastBody, Theme::contrast (p.text, back));
                    fail ("text", Theme::contrast (p.text, back), 7.0);
                }

                for (auto back : { p.inset, p.base, p.surface, p.raised })
                {
                    leastDim = juce::jmin (leastDim, Theme::contrast (p.dimText, back));
                    fail ("secondary text", Theme::contrast (p.dimText, back), 4.5);
                }

                fail ("text on a button that is on", Theme::contrast (p.textStrong, p.accentDeep), 4.5);
                fail ("text on a selected row", Theme::contrast (p.textStrong, p.surface.overlaidWith (p.selection)), 4.5);
                fail ("the clock", Theme::contrast (p.accentLight, p.inset), 4.5);
                fail ("disabled text", Theme::contrast (p.faintText, p.surface), 3.0);
                fail ("the accent", Theme::contrast (p.accent, p.base), 3.0);
                fail ("the cursor", Theme::contrast (p.cursor, p.canvas), 3.0);
                fail ("the waveform", Theme::contrast (p.waveform, p.canvas), 3.0);
                fail ("danger", Theme::contrast (p.danger, p.base), 3.0);
                fail ("button edges", Theme::contrast (p.lineStrong, p.raised), 1.5);
            }

        check (failures.isEmpty(), "theme: in all 4 themes with all 4 accents, body text reads at 7:1 or better (least "
                                       + juce::String (leastBody, 1) + "), secondary text at 4.5:1 (least " + juce::String (leastDim, 1)
                                       + "), and the accent, cursor, waveform and edges stand out" + (failures.isEmpty() ? juce::String() : ": " + failures.joinIntoString ("; ")));

        const auto unknown = Theme::make ("Sepia", "Plaid");
        check (unknown.name == "Dark" && unknown.accent == Theme::make ("Dark", "Blue").accent && Theme::make ("Light", "Teal").light
                   && Theme::make ("Light", "Teal").overlay == juce::Colours::black && Theme::make ("Darker", "Teal").overlay == juce::Colours::white,
               "theme: an unknown theme or accent falls back to Dark and Blue, and overlays are black on Light and white on the dark themes");
    }

    void testWaveformColours()
    {
        juce::StringArray failures;
        auto least = 100.0;

        for (auto& name : Theme::themeNames())
            for (auto& waveform : Theme::waveformNames())
            {
                const auto p = Theme::make (name, "Blue", waveform);

                for (auto back : { p.canvas, p.lane, p.laneAlternate })
                {
                    const auto ratio = Theme::contrast (p.waveform, back);
                    least = juce::jmin (least, ratio);

                    if (ratio < 3.0)
                        failures.add (name + "/" + waveform + " " + juce::String (ratio, 2));
                }
            }

        check (failures.isEmpty() && Theme::waveformNames().size() == 6 && Theme::waveformNames()[0] == "Theme",
               "theme: each of the 6 waveform colours stands out at 3:1 or better on the canvas and lanes of all 4 themes (least "
                   + juce::String (least, 1) + ")" + (failures.isEmpty() ? juce::String() : ": " + failures.joinIntoString ("; ")));

        const auto own = Theme::make ("Light", "Teal");
        const auto amberLight = Theme::make ("Light", "Teal", "Amber"), amberDark = Theme::make ("Dark", "Teal", "Amber");
        check (Theme::make ("Light", "Teal", "Theme").waveform == own.waveform && Theme::make ("Light", "Teal", "Plaid").waveform == own.waveform
                   && amberLight.waveform != own.waveform && amberLight.waveform != amberDark.waveform && amberLight.accent == own.accent,
               "theme: a chosen waveform colour replaces the theme's own, in a darker tone on Light; Theme or an unknown name keeps the theme's");
    }

    void testSwitching()
    {
        Theme::LookAndFeel look;
        juce::Label label;
        label.setLookAndFeel (&look);
        Theme::follow (label, juce::Label::textColourId, Theme::dimText);
        auto gone = std::make_unique<juce::Label>();
        Theme::follow (*gone, juce::Label::textColourId, Theme::text);
        gone.reset();   // a follower that has gone is passed over

        Theme::apply (Theme::make ("Light", "Orange"));
        const auto light = Theme::make ("Light", "Orange");
        const auto switched = Theme::base == light.base && Theme::accent == light.accent && Theme::current().name == "Light"
                              && label.findColour (juce::Label::textColourId) == light.dimText
                              && look.findColour (juce::TextButton::buttonColourId) == light.raised
                              && look.findColour (juce::PopupMenu::highlightedBackgroundColourId) == light.accentDeep;

        AudioEngine engine;
        juce::AudioBuffer<float> b (1, 4800);
        b.clear();
        engine.document.reset (AudioEdits::fromBuffer (std::move (b)), 48000.0, {}, 24);
        WaveformView view (engine);
        view.setBounds (0, 0, 600, 300);
        view.zoomToFit();
        const auto area = view.getWaveArea();
        const auto drawnLight = view.createComponentSnapshot (view.getLocalBounds()).getPixelAt (area.getCentreX(), area.getY() + 30);

        Theme::apply (Theme::make ("Dark", "Blue"));
        view.sendLookAndFeelChange();
        const auto drawnDark = view.createComponentSnapshot (view.getLocalBounds()).getPixelAt (area.getCentreX(), area.getY() + 30);
        const auto back = Theme::base == Theme::make ("Dark", "Blue").base && label.findColour (juce::Label::textColourId) == Theme::dimText;
        label.setLookAndFeel (nullptr);

        check (switched && back, "theme: switching sets every colour, the controls' look and the colours controls follow, and switching back restores them");
        check (drawnLight == light.canvas && drawnDark == Theme::canvas,
               "theme: the waveform view draws its background in the theme in use, redrawn when the theme changes");
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

    void testDensity()
    {
        juce::ApplicationCommandManager commands;
        TransportBar::Bar bar (commands, { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 });
        bar.setBounds (0, 0, bar.getIdealWidth(), TransportBar::Bar::height);
        const auto last = bar.getButton (10).getBounds();
        check (last.getRight() <= bar.getWidth() && bar.getWidth() - last.getRight() <= 4 && last.getHeight() == TransportBar::Bar::height
                   && bar.getButton (1).getX() == 0,
               "layout: the transport bar's ideal width holds its ten buttons and the time with no more than a gap to spare, each button as tall as the bar");
        check (Dock::Area::tabHeight == 22 && FileTabs::height == 24 && TransportBar::Bar::height == 28,
               "layout: panel tabs 22 pixels high, file tabs 24 and the transport 28");
    }
}

void runThemeTests()
{
    testDensity();
    testContrast();
    testWaveformColours();
    testSwitching();
    testControls();
}
