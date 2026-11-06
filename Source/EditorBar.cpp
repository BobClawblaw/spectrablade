#include "EditorBar.h"
#include "Theme.h"
#include "TransportBar.h"

EditorBar::EditorBar (juce::ApplicationCommandManager& c, WaveformView& v, const AudioDocument& d, Commands commandIds)
    : commands (c), view (v), document (d), ids (commandIds)
{
    fields = { { "Cursor", {}, ids.goToTime, {} }, { "Selection", {}, ids.setSelection, {} }, { "View", {}, 0, {} } };

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
    // The times.
    const auto rate = document.getSampleRate();
    const auto has = document.hasAudio() && rate > 0.0;
    const auto time = [rate] (double frames) { return TransportBar::SelectionPanel::format ((juce::int64) std::llround (frames), rate); };
    const auto dash = juce::String::fromUTF8 ("\xe2\x80\x93");
    const auto selection = view.getSelection();
    const auto visible = view.getVisibleFrames();
    const juce::String texts[] = {
        has ? time ((double) view.getCursor()) : juce::String ("-"),
        ! has ? juce::String ("-") : selection.isEmpty() ? juce::String ("none")
                                   : time ((double) selection.getStart()) + " " + dash + " " + time ((double) selection.getEnd())
                                         + "  (" + time ((double) selection.getLength()) + ")",
        has ? time (visible.getStart()) + " " + dash + " " + time (juce::jmin (visible.getEnd(), (double) document.getSnapshot()->getNumFrames()))
            : juce::String ("-")
    };
    bool changed = false;

    for (size_t i = 0; i < fields.size(); ++i)
    {
        if (fields[i].text != texts[i])
        {
            fields[i].text = texts[i];
            changed = true;
        }
    }

    if (changed)
    {
        layOutFields();
        repaint();
    }

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

namespace
{
    juce::Font labelFont()   { return juce::FontOptions (Theme::smallText); }
    juce::Font timeFont()    { return juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain); }
}

void EditorBar::layOutFields()
{
    // Left to right while they fit before the zoom controls.
    const auto limit = horizontal.getFirst()->getX() - 52;
    int x = 8;

    for (auto& f : fields)
    {
        const auto w = juce::GlyphArrangement::getStringWidthInt (labelFont(), f.name) + 6
                       + juce::GlyphArrangement::getStringWidthInt (timeFont(), f.text) + 8;
        f.bounds = x + w <= limit ? juce::Rectangle<int> (x, 1, w, getHeight() - 1) : juce::Rectangle<int>();
        x = f.bounds.isEmpty() ? limit + 1 : x + w + 14;
    }
}

void EditorBar::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasDraggedSinceMouseDown())
        return;

    for (auto& f : fields)
        if (f.command != 0 && f.bounds.contains (e.getPosition()) && run)
            run (f.command);
}

void EditorBar::mouseMove (const juce::MouseEvent& e)
{
    for (auto& f : fields)
    {
        if (f.command != 0 && f.bounds.contains (e.getPosition()))
        {
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            setTooltip (TransportBar::tooltipFor (commands, f.command));
            return;
        }
    }

    setMouseCursor (juce::MouseCursor::NormalCursor);
    setTooltip ({});
}

void EditorBar::paint (juce::Graphics& g)
{
    g.fillAll (Theme::surface);
    g.setColour (Theme::line);
    g.fillRect (0, 0, getWidth(), 1);

    for (auto& f : fields)
    {
        if (f.bounds.isEmpty())
            continue;

        auto r = f.bounds;
        g.setFont (labelFont());
        g.setColour (Theme::dimText);
        g.drawText (f.name, r.removeFromLeft (juce::GlyphArrangement::getStringWidthInt (labelFont(), f.name) + 6),
                    juce::Justification::centredLeft, false);
        g.setFont (timeFont());
        g.setColour (Theme::text);
        g.drawText (f.text, r, juce::Justification::centredLeft, false);
    }

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
    layOutFields();
}
