#pragma once

#include <JuceHeader.h>

/** A point marker (length 0) or a region (length > 0), in document frames. */
struct Marker
{
    juce::int64 start = 0;
    juce::int64 length = 0;
    juce::String name;
    int colour = -1;                               // a TrackColours palette index, or -1 for the default

    bool isRegion() const noexcept                { return length > 0; }
    juce::int64 getEnd() const noexcept           { return start + length; }

    bool operator== (const Marker& o) const       { return start == o.start && length == o.length && name == o.name && colour == o.colour; }
    bool operator!= (const Marker& o) const       { return ! operator== (o); }
};

/** Kept sorted by start, then length. */
using Markers = std::vector<Marker>;

namespace MarkerEdits
{
    void sort (Markers&);

    /** Where markers end up after frames [start, end) are replaced by newLength frames
        (a deletion when newLength is 0, an insert when start == end):
        - markers before the edit stay, markers after it move by the change in length;
        - point markers strictly inside the replaced range are removed;
        - a region keeps whatever part of it lies outside the replaced range, and grows or shrinks
          when it spans the edit. A region inside the range is removed. */
    Markers afterReplace (const Markers&, juce::int64 start, juce::int64 end, juce::int64 newLength);

    /** Markers for keeping only [start, end): points inside it (or at its end) and the parts of
        regions that overlap it, moved so start becomes frame 0. */
    Markers afterCrop (const Markers&, juce::int64 start, juce::int64 end);

    /** The nearest marker start or region end after from (or before it, going backwards). */
    std::optional<juce::int64> nextStop (const Markers&, juce::int64 from, bool forwards);

    /** The span from the nearest marker start or region end at or before frame to the nearest
        one after it: from 0 when there is none before, to end when there is none after. Nothing
        when there are no markers or frame is not inside the span (at or past end with no stop
        after it). */
    std::optional<juce::Range<juce::int64>> spanAround (const Markers&, juce::int64 frame, juce::int64 end);

    /** The region frame is in (from its start, up to but not at its end), the shortest where
        regions overlap and the earliest of those of equal length; nothing when it is in none. */
    std::optional<juce::Range<juce::int64>> regionAt (const Markers&, juce::int64 frame);

    /** The region starting next after `from` (or the last one starting before it, going
        backwards); among regions starting together, the shortest. Nothing when there is none. */
    std::optional<juce::Range<juce::int64>> adjacentRegion (const Markers&, juce::int64 from, bool forwards);

    /** Turns each point marker before end into a region running to the next point marker, or to
        end after the last, keeping its name and colour; regions are left as they are. Returns how
        many were turned. */
    int regionsFromPoints (Markers&, juce::int64 end);

    /** The pieces a file of `end` frames splits into at its point markers, as regions: one from
        each point marker to the next (or to end), named after it, and one named "Start" from frame
        0 to the first when that is later than 0. Regions in the markers are ignored. Nothing when
        there are no point markers before end. */
    Markers piecesBetweenPoints (const Markers&, juce::int64 end);

    /** Removes every marker and region that starts inside range; returns how many went. */
    int removeStartingIn (Markers&, juce::Range<juce::int64> range);

    /** Names every marker and region, in time order, "<base> 01", "<base> 02", ... (three digits
        from 100 on). The base is trimmed; an empty one changes nothing and returns false. */
    bool renameInSequence (Markers&, const juce::String& base);

    /** Most markers addAtIntervals adds at once. */
    constexpr int maxIntervalMarkers = 1000;

    /** Adds a point marker every step frames through [range start, range end), from its start,
        named as new markers are and kept sorted; frames that already hold a point marker are skipped.
        Returns how many were added: none when step is not positive or more than maxIntervalMarkers would be. */
    int addAtIntervals (Markers&, juce::Range<juce::int64> range, juce::int64 step);

    /** A name not used yet: "Marker 01", "Region 01", ... */
    juce::String nextName (const Markers&, bool region);

    /** The markers as CSV (RFC 4180, with a header line): name, start, end and length in seconds
        (end and length empty for a point), then start and length in frames. Names holding a
        comma, quote or line break are quoted, with quotes doubled. */
    juce::String toCsv (const Markers&, double sampleRate);

    /** Reads markers written by toCsv, placed by their times in seconds at this rate, so a list moves
        between files at different rates. Markers starting past numFrames are left out and regions
        are cut to it. Nothing when the header isn't toCsv's or a line can't be read. */
    std::optional<Markers> fromCsv (const juce::String& csv, double sampleRate, juce::int64 numFrames);

    /** Adds the markers not already in `into` (same place, length and name), kept sorted; returns how many. */
    int merge (Markers& into, const Markers& added);

    /** Reads WAV cue points with their labels and regions (cue + ltxt), or AIFF markers. */
    Markers fromMetadata (const juce::StringPairArray& metadata, juce::int64 numFrames);

    /** Metadata for a WAV cue chunk (points and regions) or AIFF MARK chunk (points; a region
        keeps only its start there, since AIFF has no regions). */
    std::unordered_map<juce::String, juce::String> toMetadata (const Markers&);
}
