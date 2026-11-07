#include "SurroundPad.h"
#include "Theme.h"

namespace
{
    /** The point on the square's edge in this direction (degrees, 0 in front, negative to the left),
        in pan coordinates. */
    juce::Point<float> onEdge (double degrees)
    {
        const auto a = juce::degreesToRadians (degrees);
        const auto x = std::sin (a), y = std::cos (a);
        const auto reach = 1.0 / juce::jmax (std::abs (x), std::abs (y));
        return { (float) (x * reach), (float) (y * reach) };
    }
}

SurroundPad::SurroundPad()
{
    setTooltip ("Where it plays in the surround master: drag the point, double-click to put it back in front");
    setRepaintsOnMouseActivity (false);
}

void SurroundPad::setSevenOne (bool seven)
{
    if (std::exchange (sevenOne, seven) != seven)
        repaint();
}

void SurroundPad::setPan (const SurroundPan& p)
{
    if (p == pan)
        return;

    pan = p;
    repaint();
}

juce::Rectangle<float> SurroundPad::getSquare() const noexcept
{
    const auto side = (float) juce::jmin (getWidth(), getHeight()) - 4.0f;
    return getLocalBounds().toFloat().withSizeKeepingCentre (side, side);
}

juce::Point<float> SurroundPad::toPad (float x, float y, juce::Rectangle<float> square) noexcept
{
    const auto c = square.getCentre();
    return { c.x + x * square.getWidth() * 0.5f, c.y - y * square.getHeight() * 0.5f };
}

juce::Point<float> SurroundPad::fromPad (juce::Point<float> p, juce::Rectangle<float> square) noexcept
{
    const auto c = square.getCentre();
    const auto halfWidth = juce::jmax (1.0f, square.getWidth() * 0.5f), halfHeight = juce::jmax (1.0f, square.getHeight() * 0.5f);
    return { juce::jlimit (-1.0f, 1.0f, (p.x - c.x) / halfWidth), juce::jlimit (-1.0f, 1.0f, (c.y - p.y) / halfHeight) };
}

void SurroundPad::paint (juce::Graphics& g)
{
    const auto square = getSquare();

    g.setColour (Theme::inset);
    g.fillRect (square);
    g.setColour (isEnabled() ? Theme::lineStrong : Theme::line);
    g.drawRect (square, 1.0f);

    // The listener, and the speakers on the edge: L R C in front, Ls Rs behind (in 7.1, Lss Rss at
    // the sides and Lrs Rrs behind).
    g.setColour (Theme::faintText);
    g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (square.getCentre()));
    static const std::vector<double> fiveOne { -30.0, 30.0, 0.0, -110.0, 110.0 }, sevenOneSpeakers { -30.0, 30.0, 0.0, -90.0, 90.0, -135.0, 135.0 };

    for (const auto degrees : sevenOne ? sevenOneSpeakers : fiveOne)
    {
        const auto e = onEdge (degrees);
        g.setColour (Theme::dimText);
        g.fillRect (juce::Rectangle<float> (5.0f, 5.0f).withCentre (toPad (e.x, e.y, square)));
    }

    // Where the left and right channels land, then the point (dimmed when it follows envelopes).
    const auto alpha = isEnabled() ? 1.0f : 0.6f;
    const auto offset = juce::jlimit (0.0f, 1.0f, pan.width) * std::tan (juce::degreesToRadians (30.0f));
    const auto point = toPad (pan.x, pan.y, square);

    if (offset > 0.0f)
    {
        g.setColour (Theme::accentLight.withAlpha (0.8f * alpha));

        for (const auto side : { -1.0f, 1.0f })
        {
            const auto channel = toPad (juce::jlimit (-1.0f, 1.0f, pan.x + side * offset), pan.y, square);
            g.drawLine ({ point, channel }, 1.0f);
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (channel));
        }
    }

    g.setColour (Theme::accent.withAlpha (alpha));
    g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (point));
}

void SurroundPad::moveTo (juce::Point<float> p, bool dragging)
{
    const auto at = fromPad (p, getSquare());
    auto next = pan;
    next.x = at.x;
    next.y = at.y;

    if (next == pan)
        return;

    pan = next;
    repaint();

    if (onChange != nullptr)
        onChange (pan, dragging);
}

void SurroundPad::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        moveTo (e.position, false);
}

void SurroundPad::mouseDrag (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        moveTo (e.position, true);
}

void SurroundPad::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() && onRelease != nullptr)
        onRelease();
}

void SurroundPad::mouseDoubleClick (const juce::MouseEvent&)
{
    if (juce::exactlyEqual (pan.x, 0.0f) && juce::exactlyEqual (pan.y, 1.0f))
        return;

    pan.x = 0.0f;
    pan.y = 1.0f;
    repaint();

    if (onChange != nullptr)
        onChange (pan, false);
}
