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

    /** A time as Go to Time reads it back: "m:ss.mmm", or "h:mm:ss.mmm" from an hour on. */
    inline juce::String format (juce::int64 frames, double sampleRate)
    {
        const auto ms = (juce::int64) std::llround ((double) juce::jmax ((juce::int64) 0, frames) / juce::jmax (1.0, sampleRate) * 1000.0);
        const auto minutes = ms / 60000, hours = minutes / 60;
        const auto rest = juce::String::formatted ("%02d.%03d", (int) ((ms / 1000) % 60), (int) (ms % 1000));
        return hours > 0 ? juce::String::formatted ("%d:%02d:", (int) hours, (int) (minutes % 60)) + rest
                         : juce::String ((int) minutes) + ":" + rest;
    }

    /** A time that may start with "-" (earlier) or "+" (later), as frames; otherwise as parse. */
    inline std::optional<juce::int64> parseOffset (const juce::String& typed, double sampleRate)
    {
        const auto text = typed.trim();
        const auto sign = text.startsWithChar ('-') ? -1 : 1;
        const auto frames = parse (text.startsWithChar ('-') || text.startsWithChar ('+') ? text.substring (1) : text, sampleRate);
        return frames ? std::optional<juce::int64> (sign * *frames) : std::nullopt;
    }

    /** A range as one line for the clipboard: start, end and length, tab-separated. */
    inline juce::String describe (juce::Range<juce::int64> range, double sampleRate)
    {
        return format (range.getStart(), sampleRate) + "\t" + format (range.getEnd(), sampleRate) + "\t" + format (range.getLength(), sampleRate);
    }

    /** The range a typed start and length name in a file `total` frames long, its end cut to the
        file's; nothing when either can't be read, the length is 0, or the start is at or past the end. */
    inline std::optional<juce::Range<juce::int64>> parseRange (const juce::String& start, const juce::String& length,
                                                                double sampleRate, juce::int64 total)
    {
        const auto from = parse (start, sampleRate), frames = parse (length, sampleRate);

        if (! from || ! frames || *frames <= 0 || *from >= total)
            return {};

        return juce::Range<juce::int64> (*from, juce::jmin (total, *from + *frames));
    }
}
