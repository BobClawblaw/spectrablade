#pragma once

#include <array>

#include "Session.h"

/** The colours tracks are shown in: one chosen from the palette, or by default one by the
    track's place in the list (buses grey). */
namespace TrackColours
{
    struct Choice
    {
        const char* name;
        juce::uint32 argb;
    };

    inline constexpr std::array<Choice, 8> palette { { { "Teal", 0xff4fc3a1 }, { "Blue", 0xff58a6ff }, { "Violet", 0xffd98cff },
                                                       { "Orange", 0xffffa657 }, { "Green", 0xff7ee787 }, { "Red", 0xffff7b72 },
                                                       { "Yellow", 0xffe3c74f }, { "Pink", 0xffff8ccf } } };

    /** Tracks without a colour of their own take the first six in turn. */
    inline constexpr int automaticCount = 6;

    inline juce::Colour of (const SessionTrack& t, int index)
    {
        if (juce::isPositiveAndBelow (t.colour, (int) palette.size()))
            return juce::Colour (palette[(size_t) t.colour].argb);

        return t.isBus ? juce::Colour (0xff9aa3ad) : juce::Colour (palette[(size_t) (juce::jmax (0, index) % automaticCount)].argb);
    }

    /** A clip's colour: its own, or else its track's. */
    inline juce::Colour of (const SessionClip& c, const SessionTrack& t, int index)
    {
        return juce::isPositiveAndBelow (c.colour, (int) palette.size()) ? juce::Colour (palette[(size_t) c.colour].argb) : of (t, index);
    }

    /** A marker's colour: its own, or else orange for a point and blue for a region. */
    inline juce::Colour of (const Marker& m)
    {
        if (juce::isPositiveAndBelow (m.colour, (int) palette.size()))
            return juce::Colour (palette[(size_t) m.colour].argb);

        return m.isRegion() ? juce::Colour (0xff58a6ff) : juce::Colour (0xffff8a3d);
    }
}
