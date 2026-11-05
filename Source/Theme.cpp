#include "Theme.h"

namespace Theme
{
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
