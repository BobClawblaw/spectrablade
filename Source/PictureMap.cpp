#include "PictureMap.h"
#include "AudioDocument.h"

namespace
{
    constexpr double tolerance = 1.0e-9;

    bool near (double a, double b)   { return std::abs (a - b) <= tolerance * juce::jmax (1.0, std::abs (a), std::abs (b)); }

    /** A stretch of the new sound and where it was in the old one (oldStart < 0: nowhere). In frames. */
    struct Run
    {
        juce::int64 start = 0, length = 0, oldStart = -1;
    };

    /** Where each part of `after` came from in `before`, by the audio blocks they share. */
    std::vector<Run> runsOf (const AudioSnapshot& before, const AudioSnapshot& after)
    {
        struct Old { const void* block; juce::int64 offset, length, at; };
        std::vector<Old> old;
        juce::int64 at = 0;

        for (auto& s : before.getSegments())
        {
            old.push_back ({ s.block.get(), s.offset, s.length, at });
            at += s.length;
        }

        std::vector<Run> runs;
        const auto add = [&runs] (juce::int64 start, juce::int64 length, juce::int64 oldStart)
        {
            if (length <= 0)
                return;

            // Joined to the one before when it carries straight on.
            if (! runs.empty())
            {
                auto& last = runs.back();
                const auto continues = (last.oldStart < 0 && oldStart < 0)
                                    || (last.oldStart >= 0 && oldStart >= 0 && last.oldStart + last.length == oldStart);

                if (continues && last.start + last.length == start)
                {
                    last.length += length;
                    return;
                }
            }

            runs.push_back ({ start, length, oldStart });
        };

        juce::int64 position = 0;

        for (auto& s : after.getSegments())
        {
            // The parts of this segment found in the old sound (the first place each part is), in order.
            juce::int64 from = s.offset;
            const auto end = s.offset + s.length;

            while (from < end)
            {
                const Old* best = nullptr;
                juce::int64 bestStart = end;

                for (auto& o : old)
                {
                    if (o.block != s.block.get())
                        continue;

                    const auto overlapStart = juce::jmax (from, o.offset);

                    if (overlapStart < juce::jmin (end, o.offset + o.length) && overlapStart < bestStart)
                    {
                        best = &o;
                        bestStart = overlapStart;
                    }
                }

                if (best == nullptr)
                {
                    add (position + (from - s.offset), end - from, -1);
                    break;
                }

                add (position + (from - s.offset), bestStart - from, -1);
                const auto overlapEnd = juce::jmin (end, best->offset + best->length);
                add (position + (bestStart - s.offset), overlapEnd - bestStart, best->at + (bestStart - best->offset));
                from = overlapEnd;
            }

            position += s.length;
        }

        // New audio between two kept stretches, as long as the old audio between them: processed in place.
        const auto oldLength = before.getNumFrames();

        for (size_t i = 0; i < runs.size(); ++i)
        {
            auto& r = runs[i];

            if (r.oldStart >= 0)
                continue;

            const auto prevEnd = i > 0 ? runs[i - 1].oldStart + runs[i - 1].length : 0;
            const auto prevKnown = i == 0 || runs[i - 1].oldStart >= 0;
            const auto nextStart = i + 1 < runs.size() ? runs[i + 1].oldStart : oldLength;
            const auto nextKnown = i + 1 >= runs.size() || runs[i + 1].oldStart >= 0;

            if (prevKnown && nextKnown && nextStart - prevEnd == r.length)
                r.oldStart = prevEnd;
        }

        return runs;
    }
}

std::optional<double> PictureMap::pictureAt (double t) const
{
    if (pieces.empty())
        return juce::jmax (0.0, t);

    const Piece* found = &pieces.front();

    for (auto& p : pieces)
        if (p.start <= t)
            found = &p;

    if (found->picture < 0.0)
        return std::nullopt;

    return found->picture + juce::jmax (0.0, t - found->start);
}

PictureMap PictureMap::fromPieces (std::vector<Piece> in)
{
    std::vector<Piece> out;

    for (auto& p : in)
    {
        if (p.length <= 0.0)
            continue;

        if (! out.empty())
        {
            auto& last = out.back();
            const auto bothBlank = last.picture < 0.0 && p.picture < 0.0;
            const auto carriesOn = last.picture >= 0.0 && p.picture >= 0.0 && near (last.picture + last.length, p.picture);

            if (near (last.start + last.length, p.start) && (bothBlank || carriesOn))
            {
                last.length += p.length;
                continue;
            }
        }

        out.push_back (p);
    }

    PictureMap m;

    // One piece from 0 showing the picture from 0: the identity.
    if (! (out.size() == 1 && near (out[0].start, 0.0) && near (out[0].picture, 0.0)) && ! out.empty())
        m.pieces = std::move (out);

    return m;
}

PictureMap PictureMap::afterEdit (const AudioSnapshot& before, const AudioSnapshot& after, double sampleRate) const
{
    if (before.getNumFrames() == after.getNumFrames() || sampleRate <= 0.0)
        return *this;

    std::vector<Piece> out;

    for (auto& r : runsOf (before, after))
    {
        const auto start = (double) r.start / sampleRate, length = (double) r.length / sampleRate;

        if (r.oldStart < 0)
        {
            out.push_back ({ start, length, -1.0 });
            continue;
        }

        // The old map over [oldStart, oldStart + length), piece by piece.
        const auto oldStart = (double) r.oldStart / sampleRate;

        if (pieces.empty())
        {
            out.push_back ({ start, length, oldStart });
            continue;
        }

        for (size_t i = 0; i < pieces.size(); ++i)
        {
            const auto& p = pieces[i];
            const auto pEnd = i + 1 < pieces.size() ? pieces[i + 1].start : std::numeric_limits<double>::infinity();
            const auto from = juce::jmax (oldStart, p.start), to = juce::jmin (oldStart + length, pEnd);

            if (to - from <= tolerance)
                continue;

            out.push_back ({ start + (from - oldStart), to - from, p.picture < 0.0 ? -1.0 : p.picture + (from - p.start) });
        }
    }

    return fromPieces (std::move (out));
}

juce::String PictureMap::toString() const
{
    juce::StringArray parts;

    for (auto& p : pieces)
        parts.add (juce::String (p.start, 9) + ":" + juce::String (p.length, 9) + ":" + juce::String (p.picture, 9));

    return parts.joinIntoString (" ");
}

PictureMap PictureMap::fromString (const juce::String& text)
{
    std::vector<Piece> in;

    for (auto& part : juce::StringArray::fromTokens (text, " ", {}))
    {
        const auto values = juce::StringArray::fromTokens (part, ":", {});

        if (values.size() == 3)
            in.push_back ({ values[0].getDoubleValue(), values[1].getDoubleValue(), values[2].getDoubleValue() });
    }

    return fromPieces (std::move (in));
}

bool PictureMap::operator== (const PictureMap& other) const
{
    if (pieces.size() != other.pieces.size())
        return false;

    for (size_t i = 0; i < pieces.size(); ++i)
        if (! near (pieces[i].start, other.pieces[i].start) || ! near (pieces[i].length, other.pieces[i].length)
            || ! near (pieces[i].picture, other.pieces[i].picture))
            return false;

    return true;
}
