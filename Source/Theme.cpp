#include "Theme.h"

namespace Theme
{
namespace
{
    juce::Colour c (juce::uint32 argb)    { return juce::Colour (argb); }

    Palette dark()
    {
        Palette p;
        p.name = "Dark";
        p.inset = c (0xff0c0e11); p.base = c (0xff111317); p.surface = c (0xff16191d); p.raised = c (0xff1d2126);
        p.hover = c (0xff262b32); p.line = c (0xff2a2f36); p.lineStrong = c (0xff434a54);
        p.textStrong = c (0xffffffff); p.text = c (0xffd8dde3); p.dimText = c (0xff8b939e); p.faintText = c (0xff6b737d);
        p.good = c (0xff4cc38a); p.warn = c (0xffe3c74f); p.danger = c (0xffe5484d); p.cursor = c (0xffffd34d);
        p.meterLow = c (0xff2e9e5b); p.meterMid = c (0xffe3c74f); p.meterHigh = c (0xffe07a40); p.clipOff = c (0xff3a2020);
        p.canvas = c (0xff15181d); p.lane = c (0xff191c22); p.laneAlternate = c (0xff1c2027);
        p.waveform = c (0xff4fc3a1); p.overlay = c (0xffffffff);
        return p;
    }

    Palette darker()
    {
        auto p = dark();
        p.name = "Darker";
        p.inset = c (0xff000000); p.base = c (0xff08090b); p.surface = c (0xff0d0f11); p.raised = c (0xff15171a);
        p.hover = c (0xff1e2124); p.line = c (0xff23272c); p.lineStrong = c (0xff3b4149);
        p.clipOff = c (0xff2e1818);
        p.canvas = c (0xff0a0b0d); p.lane = c (0xff0e1013); p.laneAlternate = c (0xff121418);
        p.waveform = c (0xff45b896);
        return p;
    }

    Palette light()
    {
        Palette p;
        p.name = "Light";
        p.light = true;
        p.inset = c (0xffffffff); p.base = c (0xffeceef1); p.surface = c (0xfff6f7f9); p.raised = c (0xffe2e5e9);
        p.hover = c (0xffd6dade); p.line = c (0xffc9ced4); p.lineStrong = c (0xff9aa2ab);
        p.textStrong = c (0xff0b0d10); p.text = c (0xff1f2328); p.dimText = c (0xff525b65); p.faintText = c (0xff7d868f);
        p.good = c (0xff1a7f4b); p.warn = c (0xff9a6700); p.danger = c (0xffc9262b); p.cursor = c (0xffb35a00);
        p.meterLow = c (0xff2e9e5b); p.meterMid = c (0xffd4a72c); p.meterHigh = c (0xffe07a40); p.clipOff = c (0xfff2d4d4);
        p.canvas = c (0xfff9fafb); p.lane = c (0xfff3f4f6); p.laneAlternate = c (0xffeceff2);
        p.waveform = c (0xff1b8a6b); p.overlay = c (0xff000000);
        return p;
    }

    Palette highContrast()
    {
        Palette p;
        p.name = "High Contrast";
        p.inset = c (0xff000000); p.base = c (0xff000000); p.surface = c (0xff0a0a0a); p.raised = c (0xff1a1a1a);
        p.hover = c (0xff2e2e2e); p.line = c (0xff707070); p.lineStrong = c (0xffc0c0c0);
        p.textStrong = c (0xffffffff); p.text = c (0xffffffff); p.dimText = c (0xffd0d0d0); p.faintText = c (0xff9a9a9a);
        p.good = c (0xff5ee08f); p.warn = c (0xffffd84d); p.danger = c (0xffff5c5c); p.cursor = c (0xffffe14d);
        p.meterLow = c (0xff38c46e); p.meterMid = c (0xffffd84d); p.meterHigh = c (0xffff8f4d); p.clipOff = c (0xff401818);
        p.canvas = c (0xff000000); p.lane = c (0xff0a0a0a); p.laneAlternate = c (0xff141414);
        p.waveform = c (0xff5ef0c4); p.overlay = c (0xffffffff);
        return p;
    }

    // Each accent in three tones: for dark themes, light ones and high contrast.
    struct AccentTones { const char* name; juce::uint32 dark, light, high; };
    constexpr AccentTones accents[] { { "Blue",   0xff4a9fe0, 0xff0969da, 0xff5cb8ff },
                                      { "Teal",   0xff2fb8a6, 0xff0b7a6e, 0xff3fe0c8 },
                                      { "Orange", 0xffe8893a, 0xffb4540a, 0xffffa24d },
                                      { "Violet", 0xffa47ae8, 0xff7a46c9, 0xffc39bff } };

    // The waveform colours to choose from, in two tones: for dark themes (High Contrast too) and light ones.
    struct WaveformTones { const char* name; juce::uint32 dark, light; };
    constexpr WaveformTones waveforms[] { { "Green", 0xff6cc04a, 0xff3d7a1f },
                                          { "Blue",  0xff5aa8f0, 0xff1f66b8 },
                                          { "Amber", 0xffe8b33a, 0xff8a5a00 },
                                          { "Rose",  0xffec6f9a, 0xffb0285a },
                                          { "Grey",  0xffb8bec6, 0xff5b636c } };

    Palette inUse;

    void setColours (const Palette& p)
    {
        inUse = p;
        inset = p.inset; base = p.base; surface = p.surface; raised = p.raised; hover = p.hover; line = p.line; lineStrong = p.lineStrong;
        textStrong = p.textStrong; text = p.text; dimText = p.dimText; faintText = p.faintText;
        accent = p.accent; accentLight = p.accentLight; accentDeep = p.accentDeep; selection = p.selection;
        good = p.good; warn = p.warn; danger = p.danger; cursor = p.cursor;
        meterLow = p.meterLow; meterMid = p.meterMid; meterHigh = p.meterHigh; clipOff = p.clipOff;
        canvas = p.canvas; lane = p.lane; laneAlternate = p.laneAlternate; waveform = p.waveform; overlay = p.overlay;
    }

    // Dark with blue until a theme is applied, so every colour has a value from the start.
    const bool started = [] { setColours (make ("Dark", "Blue")); return true; }();

    struct Follower
    {
        juce::Component::SafePointer<juce::Component> component;
        int colourId;
        const juce::Colour* colour;
    };

    std::vector<Follower>& followers()          { static std::vector<Follower> list; return list; }
    std::vector<LookAndFeel*>& looks()          { static std::vector<LookAndFeel*> list; return list; }
}

juce::StringArray themeNames()
{
    return { "Dark", "Darker", "Light", "High Contrast" };
}

juce::StringArray waveformNames()
{
    juce::StringArray names { "Theme" };

    for (auto& w : waveforms)
        names.add (w.name);

    return names;
}

juce::StringArray accentNames()
{
    juce::StringArray names;

    for (auto& a : accents)
        names.add (a.name);

    return names;
}

Palette make (const juce::String& theme, const juce::String& accentName, const juce::String& waveformName)
{
    auto p = theme == "Darker" ? darker() : theme == "Light" ? light() : theme == "High Contrast" ? highContrast() : dark();
    const auto* tones = &accents[0];

    for (auto& a : accents)
        if (accentName == a.name)
            tones = &a;

    const auto high = p.name == "High Contrast";
    p.accent = juce::Colour (p.light ? tones->light : high ? tones->high : tones->dark);
    p.accentLight = p.light ? p.accent.interpolatedWith (juce::Colours::black, 0.15f) : p.accent.interpolatedWith (juce::Colours::white, 0.4f);
    p.accentDeep = p.surface.interpolatedWith (p.accent, p.light ? 0.22f : 0.3f);
    p.selection = p.accent.withAlpha (p.light ? 0.22f : 0.35f);

    for (auto& w : waveforms)
        if (waveformName == w.name)
            p.waveform = juce::Colour (p.light ? w.light : w.dark);

    return p;
}

const Palette& current()
{
    return inUse;
}

void apply (const Palette& p)
{
    setColours (p);

    for (auto* look : looks())
        look->refresh();

    auto& list = followers();
    list.erase (std::remove_if (list.begin(), list.end(), [] (const Follower& f) { return f.component == nullptr; }), list.end());

    for (auto& f : list)
        f.component->setColour (f.colourId, *f.colour);

    auto& desktop = juce::Desktop::getInstance();

    for (int i = 0; i < desktop.getNumComponents(); ++i)
    {
        auto* window = desktop.getComponent (i);

        if (auto* resizable = dynamic_cast<juce::ResizableWindow*> (window))
            resizable->setBackgroundColour (base);

        window->sendLookAndFeelChange();   // views drop images drawn in the old colours
        window->repaint();
    }
}

void follow (juce::Component& component, int colourId, const juce::Colour& themeColour)
{
    component.setColour (colourId, themeColour);
    followers().push_back ({ &component, colourId, &themeColour });
}

double contrast (juce::Colour a, juce::Colour b)
{
    const auto luminance = [] (juce::Colour c)
    {
        const auto channel = [] (juce::uint8 v)
        {
            const auto s = v / 255.0;
            return s <= 0.04045 ? s / 12.92 : std::pow ((s + 0.055) / 1.055, 2.4);
        };

        return 0.2126 * channel (c.getRed()) + 0.7152 * channel (c.getGreen()) + 0.0722 * channel (c.getBlue());
    };

    const auto la = luminance (a), lb = luminance (b);
    return (juce::jmax (la, lb) + 0.05) / (juce::jmin (la, lb) + 0.05);
}

LookAndFeel::LookAndFeel()
{
    looks().push_back (this);
    refresh();
}

LookAndFeel::~LookAndFeel()
{
    auto& list = looks();
    list.erase (std::remove (list.begin(), list.end(), this), list.end());
}

void LookAndFeel::refresh()
{
    setColourScheme ({ base,            // window background
                       raised,          // widget background
                       surface,         // menu background
                       line,            // outline
                       text,            // default text
                       accent,          // default fill
                       textStrong,      // highlighted text
                       selection,       // highlighted fill
                       text });         // menu text

    setColour (juce::ResizableWindow::backgroundColourId, base);
    setColour (juce::DocumentWindow::backgroundColourId, base);
    setColour (juce::TextButton::buttonColourId, raised);
    setColour (juce::TextButton::buttonOnColourId, accentDeep);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, textStrong);
    setColour (juce::ComboBox::backgroundColourId, raised);
    setColour (juce::ComboBox::outlineColourId, lineStrong);
    setColour (juce::ComboBox::arrowColourId, dimText);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::TextEditor::backgroundColourId, inset);
    setColour (juce::TextEditor::outlineColourId, line);
    setColour (juce::TextEditor::focusedOutlineColourId, accent);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::highlightColourId, selection);
    setColour (juce::CaretComponent::caretColourId, accent);
    setColour (juce::Label::textColourId, text);
    setColour (juce::ListBox::backgroundColourId, surface);
    setColour (juce::ListBox::textColourId, text);
    setColour (juce::ListBox::outlineColourId, line);
    setColour (juce::ScrollBar::thumbColourId, lineStrong);
    setColour (juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::thumbColourId, text);
    setColour (juce::Slider::trackColourId, accent);
    setColour (juce::Slider::backgroundColourId, inset);
    setColour (juce::Slider::rotarySliderFillColourId, accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, line);
    setColour (juce::Slider::textBoxBackgroundColourId, inset);
    setColour (juce::Slider::textBoxOutlineColourId, line);
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::ToggleButton::textColourId, text);
    setColour (juce::ToggleButton::tickColourId, accent);
    setColour (juce::ToggleButton::tickDisabledColourId, faintText);
    setColour (juce::PopupMenu::backgroundColourId, surface);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::headerTextColourId, dimText);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accentDeep);
    setColour (juce::PopupMenu::highlightedTextColourId, textStrong);
    setColour (juce::TooltipWindow::backgroundColourId, raised);
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, lineStrong);
    setColour (juce::AlertWindow::backgroundColourId, surface);
    setColour (juce::AlertWindow::textColourId, text);
    setColour (juce::AlertWindow::outlineColourId, line);
    setColour (juce::TableHeaderComponent::backgroundColourId, raised);
    setColour (juce::TableHeaderComponent::textColourId, dimText);
    setColour (juce::TableHeaderComponent::outlineColourId, line);
    setColour (juce::TableHeaderComponent::highlightColourId, hover);
    setColour (juce::GroupComponent::outlineColourId, line);
    setColour (juce::GroupComponent::textColourId, dimText);
    setColour (juce::TabbedButtonBar::tabOutlineColourId, line);
    setColour (juce::TabbedComponent::backgroundColourId, base);
    setColour (juce::TabbedComponent::outlineColourId, line);
    setColour (juce::HyperlinkButton::textColourId, accent);
    setColour (juce::ProgressBar::backgroundColourId, inset);
    setColour (juce::ProgressBar::foregroundColourId, accent);
}

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                        bool highlighted, bool down)
{
    const auto r = button.getLocalBounds().toFloat().reduced (0.5f);
    const auto on = button.getToggleState();
    auto fill = backgroundColour;

    if (down)
        fill = fill.brighter (0.18f);
    else if (highlighted)
        fill = on ? fill.brighter (0.1f) : hover;

    if (! button.isEnabled())
        fill = fill.withMultipliedAlpha (0.5f);

    g.setColour (fill);
    g.fillRoundedRectangle (r, radius);

    // A button that is on has the accent's edge; others a quiet one, a little brighter under the mouse.
    const auto usesThemeOn = backgroundColour == findColour (juce::TextButton::buttonOnColourId);
    g.setColour (on ? (usesThemeOn ? accent : backgroundColour.brighter (0.3f)) : highlighted ? lineStrong.brighter (0.15f) : lineStrong);
    g.drawRoundedRectangle (r, radius, 1.0f);
}

juce::Font LookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::FontOptions (juce::jmin (labelText, (float) buttonHeight * 0.55f));
}

void LookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto r = juce::Rectangle<int> (width, height).toFloat().reduced (0.5f);
    g.setColour (box.isMouseOver (true) ? hover : box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (r, radius);
    g.setColour (box.hasKeyboardFocus (true) ? accent : box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (r, radius, 1.0f);

    // A small chevron.
    const auto arrow = juce::Rectangle<float> ((float) width - 22.0f, 0.0f, 12.0f, (float) height).withSizeKeepingCentre (9.0f, 5.0f);
    juce::Path chevron;
    chevron.startNewSubPath (arrow.getX(), arrow.getY());
    chevron.lineTo (arrow.getCentreX(), arrow.getBottom());
    chevron.lineTo (arrow.getRight(), arrow.getY());
    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withAlpha (box.isEnabled() ? 1.0f : 0.4f));
    g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font LookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    return juce::FontOptions (juce::jmin (labelText, (float) box.getHeight() * 0.55f));
}

void LookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar& bar, int x, int y, int width, int height, bool vertical,
                                 int thumbStart, int thumbSize, bool mouseOver, bool down)
{
    g.setColour (bar.findColour (juce::ScrollBar::trackColourId));
    g.fillRect (x, y, width, height);

    if (thumbSize <= 0)
        return;

    auto thumb = vertical ? juce::Rectangle<int> (x, thumbStart, width, thumbSize) : juce::Rectangle<int> (thumbStart, y, thumbSize, height);
    const auto across = (float) (vertical ? width : height);
    const auto thick = mouseOver || down ? across * 0.7f : across * 0.5f;
    const auto shaped = vertical ? thumb.toFloat().withSizeKeepingCentre (thick, (float) thumb.getHeight() - 2.0f)
                                 : thumb.toFloat().withSizeKeepingCentre ((float) thumb.getWidth() - 2.0f, thick);
    const auto colour = bar.findColour (juce::ScrollBar::thumbColourId);
    g.setColour (down ? colour.brighter (0.3f) : mouseOver ? colour.brighter (0.15f) : colour);
    g.fillRoundedRectangle (shaped, thick * 0.5f);
}

void LookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (findColour (juce::PopupMenu::backgroundColourId));
    g.setColour (line);
    g.drawRect (0, 0, width, height);
}

void LookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& tip, int width, int height)
{
    const auto r = juce::Rectangle<int> (width, height).toFloat();
    g.setColour (findColour (juce::TooltipWindow::backgroundColourId));
    g.fillRoundedRectangle (r, radius);
    g.setColour (findColour (juce::TooltipWindow::outlineColourId));
    g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
    g.setColour (findColour (juce::TooltipWindow::textColourId));
    g.setFont (juce::FontOptions (bodyText));
    g.drawFittedText (tip, juce::Rectangle<int> (width, height).reduced (gap, 4), juce::Justification::centredLeft, 6);
}

juce::Rectangle<int> LookAndFeel::getTooltipBounds (const juce::String& tip, juce::Point<int> screenPos, juce::Rectangle<int> parentArea)
{
    juce::AttributedString s;
    s.append (tip, juce::FontOptions (bodyText), text);
    juce::TextLayout layout;
    layout.createLayoutWithBalancedLineLengths (s, 360.0f);
    const auto w = (int) std::ceil (layout.getWidth()) + gap * 2 + 2;
    const auto h = (int) std::ceil (layout.getHeight()) + 10;

    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6, w, h)
        .constrainedWithin (parentArea);
}

void LookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    g.setColour (editor.findColour (juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle (juce::Rectangle<int> (width, height).toFloat(), radius);
}

void LookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (! editor.isEnabled())
        return;

    const auto focused = editor.hasKeyboardFocus (true) && ! editor.isReadOnly();
    g.setColour (editor.findColour (focused ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId));
    g.drawRoundedRectangle (juce::Rectangle<int> (width, height).toFloat().reduced (0.5f), radius, focused ? 1.5f : 1.0f);
}
}
