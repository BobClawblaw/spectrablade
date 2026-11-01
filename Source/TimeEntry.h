#pragma once

#include <JuceHeader.h>

#include "Metronome.h"

/** Times typed in by hand: "83.5" or "83.5 s" (seconds), "1:23.5" (minutes and seconds),
    "1:01:23.5" (hours too), or with a tempo map "5|3" or "5|3.5" (bar and beat, counted from 1,
    with a fraction of a beat). Spaces around the parts are ignored. */
namespace TimeEntry
{
    /** The frame a typed time names, or nothing when it can't be read, is negative, or names a beat
        past the end of its bar. */
    inline std::optional<juce::int64> parse (const juce::String& typed, double sampleRate, const Metronome::TempoMap* beats = nullptr)
    {
        const auto text = typed.trim().toLowerCase();

        if (text.isEmpty() || sampleRate <= 0.0)
            return {};

        const auto number = [] (const juce::String& part) -> std::optional<double>
        {
            const auto t = part.trim();

            if (t.isEmpty() || ! t.containsOnly ("0123456789.") || t.indexOfChar ('.') != t.lastIndexOfChar ('.'))
                return {};

            return t.getDoubleValue();
        };

        if (text.containsChar ('|'))
        {
            if (beats == nullptr)
                return {};

            const auto parts = juce::StringArray::fromTokens (text, "|", {});
            const auto bar = parts.size() == 2 ? number (parts[0]) : std::nullopt;
            const auto beat = parts.size() == 2 ? number (parts[1]) : std::nullopt;

            if (! bar || ! beat || *bar < 1.0 || *beat < 1.0 || ! juce::exactlyEqual (*bar, std::floor (*bar)))
                return {};

            const auto barIndex = (juce::int64) *bar - 1;
            const auto first = beats->barStartBeat (barIndex);
            const auto whole = (juce::int64) std::floor (*beat) - 1;

            if (whole >= beats->barStartBeat (barIndex + 1) - first)
                return {};

            const auto at = beats->beatFrame (first + whole), next = beats->beatFrame (first + whole + 1);
            return at + (juce::int64) std::llround ((*beat - std::floor (*beat)) * (double) (next - at));
        }

        auto seconds = text.endsWithChar ('s') ? text.dropLastCharacters (1) : text;
        const auto parts = juce::StringArray::fromTokens (seconds, ":", {});

        if (parts.size() > 3)
            return {};

        double total = 0.0;

        for (int i = 0; i < parts.size(); ++i)
        {
            const auto value = number (parts[i]);

            // Only the last part may have a fraction; minutes and seconds after a colon stay below 60.
            if (! value || (i < parts.size() - 1 && parts[i].containsChar ('.')) || (i > 0 && *value >= 60.0))
                return {};

            total = total * 60.0 + *value;
        }

        return (juce::int64) std::llround (total * sampleRate);
    }
}
