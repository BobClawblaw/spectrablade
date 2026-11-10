#include "EditorBar.h"
#include "Theme.h"
#include "TransportBar.h"

/** A tool button: an icon drawn in the theme's colours, lit (the accent behind it) when its tool
    is in hand, faint when it can't be used. */
class EditorBar::ToolButton final : public juce::Button
{
public:
    ToolButton (Tool t, int commandId) : juce::Button ({}), tool (t)
    {
        setComponentID (juce::String (commandId));
        setClickingTogglesState (false);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        const auto on = getToggleState();

        if (on || highlighted || down)
        {
            g.setColour (on ? Theme::accentDeep : Theme::hover);
            g.fillRoundedRectangle (r, 3.0f);
        }

        const auto ink = ! isEnabled() ? Theme::faintText : on ? Theme::textStrong : Theme::text;
        g.setColour (ink);
        const auto box = juce::Rectangle<float> (16.0f, 16.0f).withCentre (r.getCentre());
        const auto stroke = juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

        switch (tool)
        {
            case Tool::box:
            {
                // A box of dashes.
                juce::Path p, dashed;
                p.addRectangle (box.reduced (2.0f, 3.0f));
                const float dashes[] { 2.5f, 2.0f };
                juce::PathStrokeType (1.5f).createDashedStroke (dashed, p, dashes, 2);
                g.fillPath (dashed);
                break;
            }

            case Tool::lasso:
            {
                // A loop of rope with its end trailing down to the left.
                juce::Path rope;
                rope.addEllipse (box.getX() + 4.5f, box.getY() + 2.0f, 10.0f, 6.5f);
                rope.startNewSubPath (box.getX() + 6.0f, box.getY() + 7.5f);
                rope.quadraticTo (box.getX() + 3.0f, box.getY() + 9.0f, box.getX() + 2.5f, box.getBottom() - 1.5f);
                g.strokePath (rope, stroke);
                break;
            }

            case Tool::brush:
            {
                // A stroke of paint ending in a round dab.
                juce::Path trail;
                trail.startNewSubPath (box.getX() + 2.5f, box.getBottom() - 2.5f);
                trail.quadraticTo (box.getX() + 5.0f, box.getY() + 6.0f, box.getRight() - 5.0f, box.getY() + 5.0f);
                g.strokePath (trail, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ box.getRight() - 5.0f, box.getY() + 5.0f }));
                break;
            }

            case Tool::heal:
            {
                // A plaster laid across: a rounded strip with its pad in the middle.
                const auto centre = box.getCentre();
                const auto turn = juce::AffineTransform::rotation (-juce::MathConstants<float>::pi / 4.0f, centre.x, centre.y);
                juce::Path strip;
                strip.addRoundedRectangle (juce::Rectangle<float> (15.0f, 6.5f).withCentre (centre), 3.0f);
                g.fillPath (strip, turn);
                g.setColour (on ? Theme::accentDeep : Theme::surface);
                juce::Path pad;
                pad.addRectangle (juce::Rectangle<float> (4.0f, 3.5f).withCentre (centre));
                g.fillPath (pad, turn);
                break;
            }
        }
    }

private:
    Tool tool;
};

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

    for (auto [tool, id] : { std::pair (Tool::box, ids.boxTool), std::pair (Tool::lasso, ids.lassoTool), std::pair (Tool::brush, ids.brushTool), std::pair (Tool::heal, ids.heal) })
    {
        auto* b = tools.add (new ToolButton (tool, id));
        b->onClick = [this, id] { if (run) run (id); update(); };
        addAndMakeVisible (b);
    }

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

juce::Button& EditorBar::getToolButton (Tool tool)
{
    return *tools[(int) tool];
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

    for (auto* b : tools)
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

    // The tools: the one in hand lit, Heal on when there is a drawing or a box of frequencies.
    const auto inHand = view.getSpectralTool();
    const bool lit[] = { inHand == WaveformView::SpectralTool::box, inHand == WaveformView::SpectralTool::lasso, inHand == WaveformView::SpectralTool::brush, false };

    for (int i = 0; i < 3; ++i)
        if (tools[i]->getToggleState() != lit[i])
            tools[i]->setToggleState (lit[i], juce::dontSendNotification);

    const auto canHeal = view.getSpectralShape().isEmpty() ? ! view.getSelection().isEmpty() && ! view.getSpectralBand().isEmpty() : true;

    if (tools[3]->isEnabled() != canHeal)
        tools[3]->setEnabled (canHeal);
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
    int x = tools.getLast()->getRight() + 14;

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

    g.setColour (Theme::line);
    g.fillRect (tools.getLast()->getRight() + 6, 5, 1, getHeight() - 9);

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

    // From the left: the tools, Heal a step apart from the three selection tools.
    auto left = getLocalBounds().withTrimmedTop (1).reduced (4, 1);

    for (int i = 0; i < tools.size(); ++i)
    {
        if (i == 3)
            left.removeFromLeft (6);

        tools[i]->setBounds (left.removeFromLeft (26));
        left.removeFromLeft (1);
    }

    layOutFields();
}
