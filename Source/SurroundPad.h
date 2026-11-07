#pragma once

#include "Session.h"

/** A track's or bus's place in a 5.1 or 7.1 master, seen from above: the room as a square with the
    listener in the middle and the front at the top, the five (or seven) speakers on its edge, and the point
    (with, for a width over 0, where its left and right channels land). Click or drag to move the
    point; double-click to put it back in front. Width and LFE are set by sliders beside it. */
class SurroundPad final : public juce::Component,
                          public juce::SettableTooltipClient
{
public:
    SurroundPad();

    void setPan (const SurroundPan& p);

    /** Shows a 7.1 master's seven speakers rather than 5.1's five. */
    void setSevenOne (bool);
    const SurroundPan& getPan() const noexcept       { return pan; }

    /** Called with the new pan as the point moves: dragging is false for a click or double-click
        and true for the moves after it, so a drag can be one undo step. */
    std::function<void (const SurroundPan&, bool dragging)> onChange;

    /** Called when the mouse is let go, ending a click or drag. */
    std::function<void()> onRelease;

    /** Where a pan point is in the pad's square, and back (kept inside the room). */
    static juce::Point<float> toPad (float x, float y, juce::Rectangle<float> square) noexcept;
    static juce::Point<float> fromPad (juce::Point<float>, juce::Rectangle<float> square) noexcept;

    /** The square the room is drawn in. */
    juce::Rectangle<float> getSquare() const noexcept;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    SurroundPan pan;
    bool sevenOne = false;

    void moveTo (juce::Point<float>, bool dragging);
};
