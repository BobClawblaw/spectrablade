#include "TransportBar.h"
#include "Theme.h"

namespace TransportBar
{
juce::String tooltipFor (juce::ApplicationCommandManager& commands, int id)
{
    const auto* info = commands.getCommandForID (id);

    if (info == nullptr)
        return {};

    juce::StringArray keys;

    for (auto& k : commands.getKeyMappings()->getKeyPressesAssignedToCommand (id))
        keys.add (k.getTextDescriptionWithIcons());

    return info->shortName + (keys.isEmpty() ? juce::String() : " (" + keys.joinIntoString (", ") + ")");
}

//==============================================================================
namespace Icons
{
    namespace
    {
        juce::Path triangle (float left, float right, bool pointsRight)
        {
            juce::Path p;

            if (pointsRight)
                p.addTriangle (left, 0.15f, left, 0.85f, right, 0.5f);
            else
                p.addTriangle (right, 0.15f, right, 0.85f, left, 0.5f);

            return p;
        }

        /** A magnifier: its lens and handle, with the sign (or frame) drawn by the caller. */
        juce::Path magnifier()
        {
            juce::Path p;
            p.addEllipse (0.08f, 0.08f, 0.6f, 0.6f);
            juce::Path inner;
            inner.addEllipse (0.17f, 0.17f, 0.42f, 0.42f);
            p.addPath (inner);
            p.setUsingNonZeroWinding (false);
            juce::Path handle;
            handle.addRoundedRectangle (-0.06f, 0.0f, 0.12f, 0.36f, 0.05f);
            p.addPath (handle, juce::AffineTransform::rotation (-juce::MathConstants<float>::pi / 4.0f).translated (0.62f, 0.62f));
            return p;
        }
    }

    juce::Path goToStart()
    {
        auto p = triangle (0.3f, 0.85f, false);
        p.addRectangle (0.15f, 0.15f, 0.12f, 0.7f);
        return p;
    }

    juce::Path goToEnd()
    {
        auto p = triangle (0.15f, 0.7f, true);
        p.addRectangle (0.73f, 0.15f, 0.12f, 0.7f);
        return p;
    }

    juce::Path play()                { return triangle (0.22f, 0.88f, true); }

    juce::Path pause()
    {
        juce::Path p;
        p.addRectangle (0.2f, 0.15f, 0.2f, 0.7f);
        p.addRectangle (0.6f, 0.15f, 0.2f, 0.7f);
        return p;
    }

    juce::Path stop()
    {
        juce::Path p;
        p.addRectangle (0.2f, 0.2f, 0.6f, 0.6f);
        return p;
    }

    juce::Path record()
    {
        juce::Path p;
        p.addEllipse (0.18f, 0.18f, 0.64f, 0.64f);
        return p;
    }

    juce::Path loop()
    {
        // Two arcs chasing each other round, each ending in an arrowhead.
        juce::Path arcs;
        arcs.addCentredArc (0.5f, 0.5f, 0.32f, 0.32f, 0.0f, 0.35f, 2.8f, true);
        arcs.addCentredArc (0.5f, 0.5f, 0.32f, 0.32f, 0.0f, 3.5f, 5.95f, true);
        juce::Path p;
        juce::PathStrokeType (0.11f).createStrokedPath (p, arcs);
        p.addTriangle (0.62f, 0.66f, 0.92f, 0.62f, 0.78f, 0.9f);
        p.addTriangle (0.38f, 0.34f, 0.08f, 0.38f, 0.22f, 0.1f);
        return p;
    }

    juce::Path zoomIn()
    {
        auto p = magnifier();
        p.addRectangle (0.24f, 0.345f, 0.28f, 0.07f);
        p.addRectangle (0.345f, 0.24f, 0.07f, 0.28f);
        return p;
    }

    juce::Path zoomOut()
    {
        auto p = magnifier();
        p.addRectangle (0.24f, 0.345f, 0.28f, 0.07f);
        return p;
    }

    juce::Path zoomSelection()
    {
        // A span between two bars, arrows out to them.
        juce::Path p;
        p.addRectangle (0.08f, 0.15f, 0.08f, 0.7f);
        p.addRectangle (0.84f, 0.15f, 0.08f, 0.7f);
        p.addRectangle (0.3f, 0.46f, 0.4f, 0.08f);
        p.addTriangle (0.2f, 0.5f, 0.36f, 0.36f, 0.36f, 0.64f);
        p.addTriangle (0.8f, 0.5f, 0.64f, 0.36f, 0.64f, 0.64f);
        return p;
    }

    juce::Path zoomFit()
    {
        // Four corners of a frame, for "everything".
        juce::Path p;
        const float t = 0.08f, l = 0.28f;

        for (auto [x, y, dx, dy] : { std::tuple { 0.12f, 0.15f, 1.0f, 1.0f }, std::tuple { 0.88f, 0.15f, -1.0f, 1.0f },
                                     std::tuple { 0.12f, 0.85f, 1.0f, -1.0f }, std::tuple { 0.88f, 0.85f, -1.0f, -1.0f } })
        {
            p.addRectangle (juce::Rectangle<float> (juce::Point<float> (x, y), juce::Point<float> (x + dx * l, y + dy * t)));
            p.addRectangle (juce::Rectangle<float> (juce::Point<float> (x, y), juce::Point<float> (x + dx * t, y + dy * l)));
        }

        return p;
    }
}

//==============================================================================
IconButton::IconButton (const juce::String& name, juce::Path i, juce::Colour on)
    : Button (name), onColour (on), icon (std::move (i))
{
}

void IconButton::setIcon (juce::Path p)
{
    icon = std::move (p);
    repaint();
}

void IconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const auto on = getToggleState();
    auto fill = Theme::line;

    if (on)
        fill = onColour.withMultipliedBrightness (0.55f);
    else if (down)
        fill = fill.brighter (0.25f);
    else if (highlighted)
        fill = fill.brighter (0.12f);

    g.setColour (fill);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (Theme::lineStrong);
    g.drawRoundedRectangle (r, 5.0f, 1.0f);

    const auto side = juce::jmin (r.getWidth(), r.getHeight()) * 0.52f;
    const auto square = r.withSizeKeepingCentre (side, side);
    // White on the coloured fill of a button that is on; otherwise the theme's text.
    auto colour = on ? juce::Colours::white : tintedWhenOff ? onColour : Theme::text;

    if (! isEnabled())
        colour = colour.withAlpha (0.35f);

    g.setColour (colour);
    g.fillPath (icon, juce::AffineTransform::scale (square.getWidth(), square.getHeight()).translated (square.getX(), square.getY()));
}

//==============================================================================
Bar::Bar (juce::ApplicationCommandManager& c, Commands commandIds) : commands (c), ids (commandIds)
{
    const auto plain = juce::Colours::transparentBlack;
    const auto add = [this] (juce::OwnedArray<IconButton>& group, int id, juce::Path icon, juce::Colour on)
    {
        auto* b = group.add (new IconButton (commands.getNameOfCommand (id), std::move (icon), on));
        b->setComponentID (juce::String (id));
        b->onClick = [this, id] { commands.invokeDirectly (id, true); };
        addAndMakeVisible (b);
    };

    add (transport, ids.goToStart, Icons::goToStart(), plain);
    add (transport, ids.playPause, Icons::play(), Theme::good);
    add (transport, ids.stop, Icons::stop(), plain);
    add (transport, ids.record, Icons::record(), Theme::danger);
    add (transport, ids.loop, Icons::loop(), Theme::accent);
    add (transport, ids.goToEnd, Icons::goToEnd(), plain);
    add (zoom, ids.zoomIn, Icons::zoomIn(), plain);
    add (zoom, ids.zoomOut, Icons::zoomOut(), plain);
    add (zoom, ids.zoomSelection, Icons::zoomSelection(), plain);
    add (zoom, ids.zoomFit, Icons::zoomFit(), plain);

    // Play and Record show their colour on the icon while off; the rest light up only when on.
    getButton (ids.playPause).tintedWhenOff = true;
    getButton (ids.record).tintedWhenOff = true;
    commands.getKeyMappings()->addChangeListener (this);
    refreshTooltips();
    update (false, false, false, "0:00.000");
}

Bar::~Bar()
{
    commands.getKeyMappings()->removeChangeListener (this);
}

IconButton& Bar::getButton (int id)
{
    for (auto* group : { &transport, &zoom })
        for (auto* b : *group)
            if (b->getComponentID() == juce::String (id))
                return *b;

    jassertfalse;
    return *transport.getFirst();
}

void Bar::refreshTooltips()
{
    for (auto* group : { &transport, &zoom })
        for (auto* b : *group)
            b->setTooltip (tooltipFor (commands, b->getComponentID().getIntValue()));
}

void Bar::update (bool playing, bool recording, bool looping, const juce::String& time)
{
    auto& play = getButton (ids.playPause);

    if (play.getToggleState() != playing)
    {
        play.setIcon (playing ? Icons::pause() : Icons::play());
        play.setToggleState (playing, juce::dontSendNotification);
    }

    getButton (ids.record).setToggleState (recording, juce::dontSendNotification);
    getButton (ids.loop).setToggleState (looping, juce::dontSendNotification);

    // Each button as usable as its menu item is now.
    for (auto* group : { &transport, &zoom })
    {
        for (auto* b : *group)
        {
            const auto id = b->getComponentID().getIntValue();
            juce::ApplicationCommandInfo info (id);
            const auto* target = commands.getTargetForCommand (id, info);
            b->setEnabled (target != nullptr && (info.flags & juce::ApplicationCommandInfo::isDisabled) == 0);
        }
    }

    if (time != timeText || recording != showingRecording)
    {
        timeText = time;
        showingRecording = recording;
        repaint (timeArea);
    }
}

int Bar::getIdealWidth() const
{
    return (transport.size() + zoom.size()) * 38 + 12 + 190 + 24;
}

void Bar::paint (juce::Graphics& g)
{
    // The time, large, in a panel of its own; red while recording.
    const auto r = timeArea.toFloat();
    g.setColour (Theme::inset);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (showingRecording ? Theme::danger : Theme::lineStrong);
    g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
    g.setColour (showingRecording ? juce::Colour (0xffff7a7e) : Theme::accentLight);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), r.getHeight() * 0.62f, juce::Font::plain));
    g.drawText (timeText, timeArea.reduced (8, 0), juce::Justification::centred, false);
}

void Bar::resized()
{
    auto r = getLocalBounds();
    const auto side = r.getHeight();

    for (auto* b : transport)
    {
        b->setBounds (r.removeFromLeft (side));
        r.removeFromLeft (4);
    }

    r.removeFromLeft (8);
    timeArea = r.removeFromLeft (190);
    r.removeFromLeft (12);

    for (auto* b : zoom)
    {
        b->setBounds (r.removeFromLeft (side));
        r.removeFromLeft (4);
    }
}

//==============================================================================
SelectionPanel::SelectionPanel()
{
    set ({}, {}, 0.0);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setTooltip ("Click to set the selection by its times");
}

juce::String SelectionPanel::format (juce::int64 frames, double rate)
{
    if (rate <= 0.0)
        return "-";

    const auto total = (juce::int64) std::llround ((double) frames / rate * 1000.0);
    const auto hours = (int) (total / 3600000);
    const auto text = juce::String::formatted ("%d:%02d.%03d", (int) ((total / 60000) % 60), (int) ((total / 1000) % 60), (int) (total % 1000));
    return hours > 0 ? juce::String (hours) + ":" + text.paddedLeft ('0', 9) : text;
}

void SelectionPanel::set (juce::Range<juce::int64> selection, juce::Range<juce::int64> view, double rate)
{
    std::array<std::array<juce::String, 3>, 2> next;
    const std::array<juce::Range<juce::int64>, 2> ranges { selection, view };

    for (size_t row = 0; row < 2; ++row)
    {
        const auto& range = ranges[row];
        const auto shown = ! range.isEmpty() && rate > 0.0;
        next[row] = { shown ? format (range.getStart(), rate) : "-", shown ? format (range.getEnd(), rate) : "-",
                      shown ? format (range.getLength(), rate) : "-" };
    }

    if (next != cells)
    {
        cells = next;
        repaint();
    }
}

void SelectionPanel::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (Theme::raised);
    g.fillRoundedRectangle (r, 4.0f);

    auto area = getLocalBounds().reduced (8, 2);
    const auto rowHeight = area.getHeight() / 3;
    auto header = area.removeFromTop (rowHeight);
    const auto labelWidth = 70;
    const auto columnWidth = (area.getWidth() - labelWidth) / 3;
    g.setFont (juce::FontOptions (11.0f));
    g.setColour (Theme::dimText);
    header.removeFromLeft (labelWidth);

    for (auto name : { "Start", "End", "Duration" })
        g.drawText (name, header.removeFromLeft (columnWidth), juce::Justification::centredRight, false);

    const char* rowNames[] = { "Selection", "View" };

    for (int row = 0; row < 2; ++row)
    {
        auto line = area.removeFromTop (rowHeight);
        g.setFont (juce::FontOptions (12.0f));
        g.setColour (Theme::text);
        g.drawText (rowNames[row], line.removeFromLeft (labelWidth), juce::Justification::centredLeft, false);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
        g.setColour (row == 0 ? Theme::accentLight : Theme::text);

        for (size_t column = 0; column < 3; ++column)
            g.drawText (cells[(size_t) row][column], line.removeFromLeft (columnWidth), juce::Justification::centredRight, false);
    }
}

void SelectionPanel::mouseUp (const juce::MouseEvent& e)
{
    if (onClick != nullptr && getLocalBounds().contains (e.getPosition()))
        onClick();
}
}
