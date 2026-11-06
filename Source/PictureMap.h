#pragma once

#include <JuceHeader.h>

class AudioSnapshot;

/** Which part of a file's video goes with each stretch of its sound, kept through edits that
    change the sound's length: after a cut the picture jumps where the sound does, audio pasted from
    elsewhere in the file brings its picture with it, and audio from nowhere in the file (inserted
    silence, generated or stretched audio) has no picture (black). Times are in seconds, so a sample
    rate conversion leaves it as it is.

    Untouched (the sound as opened, or edited only in place), the picture is the sound's time. */
class PictureMap
{
public:
    struct Piece
    {
        double start = 0.0, length = 0.0;   // in the sound
        double picture = 0.0;               // where in the video it starts; below 0: no picture
    };

    /** Whether the picture is the sound's time throughout. */
    bool isIdentity() const noexcept                  { return pieces.empty(); }

    /** The pieces, in order, covering the sound from 0 (empty: the identity). The last one runs on
        past its end. */
    const std::vector<Piece>& getPieces() const noexcept   { return pieces; }

    /** Where in the video the sound at this time is (nothing: no picture there). */
    std::optional<double> pictureAt (double soundTime) const;

    /** The map after an edit from `before` to `after` (at sampleRate): each stretch of the new sound
        that shares audio with the old one goes with that audio's picture; a stretch of new audio
        between two kept ones, as long as what it replaced, is taken as processed in place and keeps
        that picture; other new audio has none. An edit that keeps the length leaves it as it is. */
    PictureMap afterEdit (const AudioSnapshot& before, const AudioSnapshot& after, double sampleRate) const;

    /** From pieces (merged where they join up; the identity when they are). */
    static PictureMap fromPieces (std::vector<Piece>);

    juce::String toString() const;
    static PictureMap fromString (const juce::String&);

    bool operator== (const PictureMap& other) const;
    bool operator!= (const PictureMap& other) const   { return ! operator== (other); }

private:
    std::vector<Piece> pieces;
};
