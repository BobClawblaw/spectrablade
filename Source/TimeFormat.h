#pragma once

#include <JuceHeader.h>

/** How times are shown: minutes and seconds (the default), decimal seconds, samples, or SMPTE
    timecode at 24, 25, 29.97 (drop or non-drop) or 30 frames a second. One format for the whole
    app, chosen in View > Time Format and kept in the settings; everything that shows a time draws
    it from here, and the rulers pick their marks to suit it. */
namespace TimeFormat
{
    enum class Kind
    {
        minutesSeconds,   // 1:23.456, 1:01:23.456 from an hour on
        decimalSeconds,   // 83.456
        samples,          // 3998592
        smpte24,          // 00:01:23:10
        smpte25,
        smpte2997Drop,    // 00:01:23;10 (semicolons mark drop frame)
        smpte2997,
        smpte30,
        count
    };

    /** The menu names, in Kind order. */
    juce::StringArray names();

    /** The settings key of a kind and the kind for a key (minutesSeconds for one not known). */
    juce::String key (Kind);
    Kind fromKey (const juce::String&);

    /** Frames a second of a SMPTE kind (29.97 is 30000/1001), 0 for the others. */
    double framesPerSecond (Kind);
    bool isDropFrame (Kind);
    bool isSmpte (Kind);

    /** The format in use, and changing it: listeners hear of a change. */
    Kind get();
    void set (Kind);
    juce::ChangeBroadcaster& broadcaster();

    /** A time in a format: the frame count in samples; seconds to the millisecond for minutes and
        seconds (hours when there are any) and decimal seconds; hh:mm:ss:ff for timecode (the frame
        the time falls in). Negative frames read as 0. */
    juce::String format (juce::int64 frames, double sampleRate, Kind kind = get());

    /** The frame a time in a format names: an integer for samples; hh:mm:ss:ff or hh:mm:ss;ff
        (hours optional) for timecode, with each part in range (and the dropped frames 00 and 01
        of a minute not a multiple of ten refused for drop frame); a number of seconds, m:ss or
        h:mm:ss for the rest. Nothing when the text isn't one. */
    std::optional<juce::int64> parse (const juce::String& text, double sampleRate, Kind kind = get());

    /** The seconds between a ruler's marks for a format, the finest that leaves at least
        minPixels between them: a 1-2-5 series of seconds, or of samples, or frames of timecode
        then seconds. */
    double rulerTick (double secondsPerPixel, double sampleRate, Kind kind = get(), double minPixels = 100.0);

    /** A ruler mark's label: as short as the spacing allows (minutes and seconds with the decimals
        the tick needs, whole or decimal seconds, the sample count, or timecode without the hours
        when there are none). */
    juce::String rulerLabel (double seconds, double secondsPerTick, double sampleRate, Kind kind = get());

    /** The timecode frame count for a time at fps frames a second with drop-frame counting
        (29.97 drop: 2 frames skipped at the start of each minute but every tenth), and back. */
    juce::int64 dropFrameCount (juce::int64 frames);        // from real frames at 30000/1001 to counted frames
    juce::int64 fromDropFrameCount (juce::int64 counted);   // from a timecode's counted frames to real frames
}
