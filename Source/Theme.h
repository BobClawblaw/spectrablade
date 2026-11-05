#pragma once

#include <JuceHeader.h>

/** SpectraBlade's look: every colour, size and corner of the interface in one place, and the
    look-and-feel that draws the standard controls with them. The colours come from the theme in
    use (Dark, Darker, Light or High Contrast) and its accent; switching either repaints the app.
    Colours that carry meaning in the audio itself (the spectrogram's scale, track, marker and
    automation colours) are the same in every theme. */
namespace Theme
{
    /** Every colour a theme sets. */
    struct Palette
    {
        juce::String name;
        bool light = false;

        // Surfaces, darkest (in a dark theme) first.
        juce::Colour inset;        // wells: meters, text fields, the bars between panels
        juce::Colour base;         // the window and the panels
        juce::Colour surface;      // lists, headers, the rack
        juce::Colour raised;       // buttons, tabs, rows
        juce::Colour hover;        // a raised thing under the mouse
        juce::Colour line;         // borders and grid lines
        juce::Colour lineStrong;   // borders that need to show: button edges

        // Text, strongest first.
        juce::Colour textStrong;   // the thing in front: the active file, a selected row
        juce::Colour text;
        juce::Colour dimText;      // secondary: details, units, hints
        juce::Colour faintText;    // what is disabled or can be redone

        // The accent and the colours made from it.
        juce::Colour accent;
        juce::Colour accentLight;  // accent text on wells: the clock, a hovered value
        juce::Colour accentDeep;   // a filled area marked with the accent: the active row, a button that is on
        juce::Colour selection;    // behind selected rows

        // States.
        juce::Colour good, warn, danger;
        juce::Colour cursor;       // the edit cursor

        // Meters: green to the warning zone, amber in it, orange above, red when clipped.
        juce::Colour meterLow, meterMid, meterHigh, clipOff;

        // Where the audio is drawn.
        juce::Colour canvas;       // behind waveforms and tracks
        juce::Colour lane, laneAlternate;
        juce::Colour waveform;
        juce::Colour overlay;      // white in a dark theme, black in a light one: thin grid lines and tints over the audio
    };

    // The colours in use: the current theme's. Read them when painting.
    inline juce::Colour inset, base, surface, raised, hover, line, lineStrong;
    inline juce::Colour textStrong, text, dimText, faintText;
    inline juce::Colour accent, accentLight, accentDeep, selection;
    inline juce::Colour good, warn, danger, cursor;
    inline juce::Colour meterLow, meterMid, meterHigh, clipOff;
    inline juce::Colour canvas, lane, laneAlternate, waveform, overlay;

    // Sizes.
    constexpr float radius = 4.0f;                   // corners of buttons, fields, rows
    constexpr float smallText = 11.0f, bodyText = 13.0f, labelText = 14.0f, headingText = 16.0f;
    constexpr int gap = 8;                           // between controls, and around a panel's contents

    /** The themes, in menu order ("Dark" first, the default), and the accents. */
    juce::StringArray themeNames();
    juce::StringArray accentNames();

    /** A theme with an accent ("Blue" if the name isn't known; "Dark" for an unknown theme). */
    Palette make (const juce::String& theme, const juce::String& accentName);

    /** Puts a palette into use: the colours above, the look-and-feel, the colours given to controls
        with follow(), and a repaint of every window. */
    void apply (const Palette&);

    /** The palette in use. */
    const Palette& current();

    /** Gives a control one of the theme's colours, kept up to date when the theme changes. */
    void follow (juce::Component&, int colourId, const juce::Colour& themeColour);

    /** The WCAG contrast ratio of two opaque colours, from 1 (the same) to 21 (black on white). */
    double contrast (juce::Colour, juce::Colour);

    /** Draws the standard controls in the theme: flat raised buttons with a thin edge, the accent
        for what is on, quiet scroll bars, and menus, tooltips and dialogs on the panels' colours. */
    class LookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel();
        ~LookAndFeel() override;

        /** Sets the controls' colours from the theme in use (apply() does this for each). */
        void refresh();

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
