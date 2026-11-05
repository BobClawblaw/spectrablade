#pragma once

#include <JuceHeader.h>

/** SpectraBlade's look: every colour, size and corner of the interface in one place, and the
    look-and-feel that draws the standard controls with them. Colours that carry meaning in the
    audio itself (waveforms, the spectrogram's scale, track and automation colours) live with the
    views that draw them. */
namespace Theme
{
    // Surfaces, darkest first.
    const juce::Colour inset      { 0xff0c0e11 };   // wells: meters, text fields, the bars between panels
    const juce::Colour base       { 0xff111317 };   // the window and the panels
    const juce::Colour surface    { 0xff16191d };   // lists, headers, the rack
    const juce::Colour raised     { 0xff1d2126 };   // buttons, tabs, rows
    const juce::Colour hover      { 0xff262b32 };   // a raised thing under the mouse
    const juce::Colour line       { 0xff2a2f36 };   // borders and grid lines
    const juce::Colour lineStrong { 0xff434a54 };   // borders that need to show: button edges

    // Text, strongest first.
    const juce::Colour textStrong { 0xffffffff };   // the thing in front: the active file, a selected row
    const juce::Colour text       { 0xffd8dde3 };
    const juce::Colour dimText    { 0xff8b939e };   // secondary: details, units, hints
    const juce::Colour faintText  { 0xff6b737d };   // what is disabled or can be redone

    // The one accent, and states.
    const juce::Colour accent     { 0xff4a9fe0 };
    const juce::Colour accentLight{ 0xff8fd3ff };   // accent text on dark wells: the clock, a hovered value
    const juce::Colour accentDeep { 0xff23384d };   // a filled area marked with the accent: the active row, a button that is on
    const juce::Colour selection  { 0x594a9fe0 };   // the accent at 35%, behind selected rows
    const juce::Colour good       { 0xff4cc38a };
    const juce::Colour warn       { 0xffe3c74f };
    const juce::Colour danger     { 0xffe5484d };   // record, clipping, mute
    const juce::Colour cursor     { 0xffffd34d };   // the edit cursor and play position

    // Meters: green to the warning zone, amber in it, orange above, red when clipped.
    const juce::Colour meterLow   { 0xff2e9e5b };
    const juce::Colour meterMid   { 0xffe3c74f };
    const juce::Colour meterHigh  { 0xffe07a40 };
    const juce::Colour clipOff    { 0xff3a2020 };   // a clip light that is out

    // Sizes.
    constexpr float radius = 4.0f;                   // corners of buttons, fields, rows
    constexpr float smallText = 11.0f, bodyText = 13.0f, labelText = 14.0f, headingText = 16.0f;
    constexpr int gap = 8;                           // between controls, and around a panel's contents

    /** The WCAG contrast ratio of two opaque colours, from 1 (the same) to 21 (black on white). */
    double contrast (juce::Colour, juce::Colour);

    /** Draws the standard controls in the theme: flat raised buttons with a thin edge, the accent
        for what is on, quiet scroll bars, and menus, tooltips and dialogs on the panels' colours. */
    class LookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel();

        void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                                   bool highlighted, bool down) override;
        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
        void drawComboBox (juce::Graphics&, int width, int height, bool down, int buttonX, int buttonY,
                           int buttonW, int buttonH, juce::ComboBox&) override;
        juce::Font getComboBoxFont (juce::ComboBox&) override;
        void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool vertical,
                            int thumbStart, int thumbSize, bool mouseOver, bool down) override;
        void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
        void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;
        juce::Rectangle<int> getTooltipBounds (const juce::String& text, juce::Point<int> screenPos, juce::Rectangle<int> parentArea) override;
        void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
        void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    };
}
