#pragma once

#include "AudioDocument.h"

/** The fades the editor's fade handles make: a fade in from the start of the file and a fade out
    to its end, each with a length and a curve. While they are the last edit they stay adjustable:
    dragging a handle again redoes them from the audio as it was before, still as one undo step. */
namespace FileFades
{
    /** How loud a fade in is at x, from 0 (its start) to 1 (its end), for a curve from -1 to +1:
        0 is a straight line, +1 rises quickly then levels off, -1 starts slowly. A fade out is the
        same curve played backwards. */
    float level (double x, float curve);

    struct Fade
    {
        juce::int64 frames = 0;
        float curve = 0.0f;
        bool operator== (const Fade& o) const    { return frames == o.frames && juce::exactlyEqual (curve, o.curve); }
        bool operator!= (const Fade& o) const    { return ! (*this == o); }
    };

    /** The audio with a fade in over its first `in.frames` and a fade out over its last
        `out.frames` (the two kept from overlapping). */
    std::shared_ptr<const AudioSnapshot> render (const AudioSnapshot&, Fade in, Fade out);

    /** The fades of one document, adjustable while they are its last edit. */
    class Draft
    {
    public:
        /** The fades as they stand: none unless they are still the document's last step. */
        Fade get (const AudioDocument&, bool out) const;

        /** Whether the document's last step is still these fades. */
        bool isLive (const AudioDocument&) const;

        /** Sets one fade, kept within what the file's length and the other fade leave, and puts
            both into the document: in place of the last step while it is still theirs, otherwise
            as a new step on top of the audio as it is. Returns the fade as set. */
        Fade set (AudioDocument&, bool out, Fade);

        /** The longest a fade can be, given the other one. */
        juce::int64 maxFrames (const AudioDocument&, bool out) const;

    private:
        std::shared_ptr<const AudioSnapshot> base;   // the audio before the fades
        Fade in, out;
        int stepPosition = -1;
        juce::uint32 stepVersion = 0;
    };
}
