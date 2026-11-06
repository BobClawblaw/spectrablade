#include "EditorBar.h"
#include "Theme.h"
#include "TransportBar.h"

EditorBar::EditorBar (juce::ApplicationCommandManager& c, WaveformView& v, Commands commandIds)
    : commands (c), view (v), ids (commandIds)
{
    run = [this] (int id) { commands.invokeDirectly (id, true); };

    const auto add = [this] (juce::OwnedArray<juce::TextButton>& group, int id, const juce::String& text)
    {
        auto* b = group.add (new juce::TextButton (text));
        b->setComponentID (juce::String (id));
        b->onClick = [this, id] { if (run) run (id); };
        addAndMakeVisible (b);
    };

    const auto minus = juce::String::fromUTF8 ("\xe2\x88\x92");   // a real minus sign
    add (horizontal, ids.zoomOut, minus);
    add (horizontal, ids.zoomIn, "+");
    add (horizontal, ids.zoomFit, "Fit");
    add (vertical, ids.verticalOut, minus);
    add (vertical, ids.verticalIn, "+");
    add (vertical, ids.verticalReset, "1x");

    commands.getKeyMappings()->addChangeListener (this);
    refreshTooltips();
    update();
    startTimerHz (10);
}

EditorBar::~EditorBar()
{
    commands.getKeyMappings()->removeChangeListener (this);
}

juce::TextButton& EditorBar::getButton (int id)
{
    for (auto* group : { &horizontal, &vertical })
        for (auto* b : *group)
            if (b->getComponentID() == juce::String (id))
                return *b;

    jassertfalse;
    return *horizontal.getFirst();
}

juce::String EditorBar::verticalText (double zoom)
{
    return juce::String (zoom, zoom < 10.0 && ! juce::exactlyEqual (zoom, std::round (zoom)) ? 1 : 0) + "x";
}

void EditorBar::refreshTooltips()
{
    for (auto* group : { &horizontal, &vertical })
        for (auto* b : *group)
            b->setTooltip (TransportBar::tooltipFor (commands, b->getComponentID().getIntValue()));
}

void EditorBar::timerCallback()
{
    update();
}

void EditorBar::update()
{
    const auto zoom = view.getVerticalZoom();
    const auto text = verticalText (zoom);
    getButton (ids.verticalOut).setEnabled (zoom > 1.0);
    getButton (ids.verticalReset).setEnabled (zoom > 1.0);
    getButton (ids.verticalIn).setEnabled (zoom < WaveformView::maxVerticalZoom);

    if (text != shownVertical)
    {
        shownVertical = text;
        repaint (verticalReadout());
    }
}

juce::Rectangle<int> EditorBar::verticalReadout() const
{
    const auto first = vertical.getFirst()->getBounds();
    return { first.getX() - 86, 0, 82, getHeight() };
}

void EditorBar::paint (juce::Graphics& g)
{
    g.fillAll (Theme::surface);
    g.setColour (Theme::line);
    g.fillRect (0, 0, getWidth(), 1);

    g.setFont (juce::FontOptions (Theme::smallText));
    g.setColour (Theme::dimText);
    const auto firstH = horizontal.getFirst()->getBounds();
    g.drawText ("Zoom", juce::Rectangle<int> (firstH.getX() - 44, 0, 40, getHeight()), juce::Justification::centredRight, false);
    g.drawText ("Level " + shownVertical, verticalReadout(), juce::Justification::centredRight, false);
}

void EditorBar::resized()
{
    // From the right: the vertical buttons, the level readout, then the horizontal ones.
    auto r = getLocalBounds().withTrimmedTop (1).reduced (4, 2);
    const auto place = [&r] (juce::TextButton& b, int width)
    {
        b.setBounds (r.removeFromRight (width));
        r.removeFromRight (2);
    };

    place (getButton (ids.verticalReset), 30);
    place (getButton (ids.verticalIn), 24);
    place (getButton (ids.verticalOut), 24);
    r.removeFromRight (90);
    place (getButton (ids.zoomFit), 34);
    place (getButton (ids.zoomIn), 24);
    place (getButton (ids.zoomOut), 24);
}
