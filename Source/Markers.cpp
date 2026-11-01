#include "Markers.h"

#include <set>

namespace MarkerEdits
{
namespace
{
    juce::String clean (const juce::String& text)
    {
        // Label text is stored with a terminating null; a juce::String already stops there.
        return text.trim();
    }
}

void sort (Markers& markers)
{
    std::stable_sort (markers.begin(), markers.end(), [] (const Marker& a, const Marker& b)
    {
        return a.start != b.start ? a.start < b.start : a.length < b.length;
    });
}

Markers afterReplace (const Markers& markers, juce::int64 start, juce::int64 end, juce::int64 newLength)
{
    const auto delta = newLength - (end - start);
    Markers result;

    for (auto m : markers)
    {
        if (! m.isRegion())
        {
            if (m.start <= start)
                result.push_back (m);
            else if (m.start >= end)
                result.push_back ({ m.start + delta, 0, m.name, m.colour });

            continue;
        }

        // A start inside the replaced range moves to the end of the new material; an end inside it
        // moves to the start of the range. Whatever is left (if anything) is the region.
        const auto a = m.start <= start ? m.start : m.start >= end ? m.start + delta : start + newLength;
        const auto b = m.getEnd() <= start ? m.getEnd() : m.getEnd() >= end ? m.getEnd() + delta : start;

        if (b > a)
            result.push_back ({ a, b - a, m.name, m.colour });
    }

    sort (result);
    return result;
}

Markers afterCrop (const Markers& markers, juce::int64 start, juce::int64 end)
{
    Markers result;

    for (const auto& m : markers)
    {
        if (! m.isRegion())
        {
            if (m.start >= start && m.start <= end)
                result.push_back ({ m.start - start, 0, m.name, m.colour });

            continue;
        }

        const auto a = juce::jmax (m.start, start);
        const auto b = juce::jmin (m.getEnd(), end);

        if (b > a)
            result.push_back ({ a - start, b - a, m.name, m.colour });
    }

    sort (result);
    return result;
}

std::optional<juce::int64> nextStop (const Markers& markers, juce::int64 from, bool forwards)
{
    std::optional<juce::int64> target;

    for (const auto& m : markers)
        for (auto frame : { m.start, m.getEnd() })
            if (forwards ? frame > from && (! target || frame < *target)
                         : frame < from && (! target || frame > *target))
                target = frame;

    return target;
}

int regionsFromPoints (Markers& markers, juce::int64 end)
{
    std::vector<juce::int64> points;

    for (auto& m : markers)
        if (! m.isRegion())
            points.push_back (m.start);

    std::sort (points.begin(), points.end());
    auto turned = 0;

    for (auto& m : markers)
    {
        if (m.isRegion() || m.start >= end)
            continue;

        const auto next = std::upper_bound (points.begin(), points.end(), m.start);
        m.length = (next != points.end() ? juce::jmin (*next, end) : end) - m.start;
        ++turned;
    }

    sort (markers);
    return turned;
}

bool renameInSequence (Markers& markers, const juce::String& base)
{
    const auto trimmed = base.trim();

    if (trimmed.isEmpty() || markers.empty())
        return false;

    sort (markers);
    const auto digits = juce::jmax (2, (int) juce::String ((int) markers.size()).length());

    for (size_t i = 0; i < markers.size(); ++i)
        markers[i].name = trimmed + " " + juce::String ((int) i + 1).paddedLeft ('0', digits);

    return true;
}

int addAtIntervals (Markers& markers, juce::Range<juce::int64> range, juce::int64 step)
{
    if (step <= 0 || range.isEmpty() || (range.getLength() + step - 1) / step > maxIntervalMarkers)
        return 0;

    std::set<juce::int64> taken;

    for (auto& m : markers)
        if (! m.isRegion())
            taken.insert (m.start);

    auto added = 0;

    for (auto frame = range.getStart(); frame < range.getEnd(); frame += step)
    {
        if (taken.count (frame) > 0)
            continue;

        markers.push_back ({ frame, 0, nextName (markers, false) });
        ++added;
    }

    sort (markers);
    return added;
}

std::optional<juce::Range<juce::int64>> regionAt (const Markers& markers, juce::int64 frame)
{
    std::optional<juce::Range<juce::int64>> found;

    for (auto& m : markers)
        if (m.isRegion() && m.start <= frame && frame < m.getEnd() && (! found || m.length < found->getLength()))
            found = juce::Range<juce::int64> (m.start, m.getEnd());

    return found;
}

std::optional<juce::Range<juce::int64>> spanAround (const Markers& markers, juce::int64 frame, juce::int64 end)
{
    if (markers.empty())
        return {};

    const auto from = nextStop (markers, frame + 1, false).value_or (0);
    const auto to = nextStop (markers, frame, true).value_or (end);

    if (to <= from || frame >= to)
        return {};

    return juce::Range<juce::int64> (from, to);
}

juce::String nextName (const Markers& markers, bool region)
{
    const juce::String base (region ? "Region " : "Marker ");

    for (int n = 1;; ++n)
    {
        const auto name = base + juce::String (n).paddedLeft ('0', 2);

        if (std::none_of (markers.begin(), markers.end(), [&] (const Marker& m) { return m.name == name; }))
            return name;
    }
}

Markers fromMetadata (const juce::StringPairArray& metadata, juce::int64 numFrames)
{
    auto value = [&] (const juce::String& key) { return metadata.getValue (key, {}); };

    // Labels and regions refer to cue points by identifier.
    std::map<int, juce::String> labels;
    std::map<int, std::pair<juce::int64, juce::String>> regions;

    for (int i = 0; i < value ("NumCueLabels").getIntValue(); ++i)
        labels[value ("CueLabel" + juce::String (i) + "Identifier").getIntValue()] = clean (value ("CueLabel" + juce::String (i) + "Text"));

    for (int i = 0; i < value ("NumCueRegions").getIntValue(); ++i)
    {
        const auto prefix = "CueRegion" + juce::String (i);
        regions[value (prefix + "Identifier").getIntValue()] = { (juce::int64) value (prefix + "SampleLength").getLargeIntValue(),
                                                                 clean (value (prefix + "Text")) };
    }

    Markers markers;

    for (int i = 0; i < value ("NumCuePoints").getIntValue(); ++i)
    {
        const auto prefix = "Cue" + juce::String (i);
        const auto id = value (prefix + "Identifier").getIntValue();
        Marker m;
        m.start = juce::jlimit ((juce::int64) 0, numFrames, (juce::int64) value (prefix + "Offset").getLargeIntValue());

        if (const auto r = regions.find (id); r != regions.end())
        {
            m.length = juce::jlimit ((juce::int64) 0, numFrames - m.start, r->second.first);
            m.name = r->second.second;
        }

        if (const auto l = labels.find (id); l != labels.end() && l->second.isNotEmpty())
            m.name = l->second;

        if (m.name.isEmpty())
            m.name = nextName (markers, m.isRegion());

        markers.push_back (m);
    }

    sort (markers);
    return markers;
}

juce::String toCsv (const Markers& markers, double sampleRate)
{
    const auto seconds = [sampleRate] (juce::int64 frames) { return juce::String ((double) frames / sampleRate, 6); };
    juce::String csv ("Name,Start (s),End (s),Length (s),Start (frames),Length (frames)\r\n");

    for (auto& m : markers)
    {
        auto name = m.name;

        if (name.containsAnyOf (",\"\r\n"))
            name = "\"" + name.replace ("\"", "\"\"") + "\"";

        csv << name << "," << seconds (m.start) << "," << (m.isRegion() ? seconds (m.getEnd()) : juce::String()) << ","
            << (m.isRegion() ? seconds (m.length) : juce::String()) << "," << juce::String (m.start) << "," << juce::String (m.length) << "\r\n";
    }

    return csv;
}

std::unordered_map<juce::String, juce::String> toMetadata (const Markers& markers)
{
    std::unordered_map<juce::String, juce::String> values;

    if (markers.empty())
        return values;

    int regionIndex = 0;

    for (size_t i = 0; i < markers.size(); ++i)
    {
        const auto& m = markers[i];
        const juce::String id ((int) i + 1);
        const auto cue = "Cue" + juce::String ((int) i);
        const auto label = "CueLabel" + juce::String ((int) i);

        values[cue + "Identifier"] = id;
        values[cue + "Order"] = id;
        values[cue + "Offset"] = juce::String (m.start);
        values[cue + "BlockStart"] = "0";
        values[cue + "ChunkStart"] = "0";
        values[label + "Identifier"] = id;
        values[label + "Text"] = m.name;

        if (m.isRegion())
        {
            const auto region = "CueRegion" + juce::String (regionIndex++);
            values[region + "Identifier"] = id;
            values[region + "SampleLength"] = juce::String (m.length);
            values[region + "Purpose"] = juce::String ((int) juce::ByteOrder::littleEndianInt ("rgn "));
            values[region + "Text"] = m.name;
        }
    }

    values["NumCuePoints"] = juce::String ((int) markers.size());
    values["NumCueLabels"] = juce::String ((int) markers.size());

    if (regionIndex > 0)
        values["NumCueRegions"] = juce::String (regionIndex);

    return values;
}
}
