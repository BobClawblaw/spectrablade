#pragma once

#include "AudioDocument.h"

/** Takes out, or keeps alone, what is panned to the middle of a stereo mix (a voice, dialogue, a
    lead instrument), band by band in short-time Fourier frames (square-root Hann windows, 75%
    overlap, about 43 ms long).

    In each band, the part the two channels share is estimated from how alike they are: the power
    they have in common (their cross-spectrum) against each one's own power, both averaged over
    the band's neighbours and smoothed over time. What is in the middle is the mid channel,
    (L + R) / 2, weighted by the share of it that is common power (a Wiener-style gain, squared to
    be surer of what it keeps). Removing lowers that from both channels; extracting lowers the rest
    instead, so the middle is left alone in both.

    Only bands between the low and high frequency are changed, easing out over a third of an
    octave past each; the rest of the mix passes as it was. A source panned to the middle and one
    off to the side in the same band at the same moment can't be told apart fully, so some of
    each stays with the other. */
namespace CentreChannel
{
    struct Settings
    {
        bool keepCentre = false;   // extract (keep the middle alone) rather than remove it
        float lowHz = 100.0f;      // the band worked on
        float highHz = 8000.0f;
        float reductionDb = 20.0f; // how far what goes is lowered

        static constexpr float lowest = 20.0f, highest = 20000.0f, mostReduction = 40.0f;
    };

    bool isValid (const Settings&);

    /** The frame length at a sample rate: about 43 ms, a power of two. */
    int frameSize (double sampleRate);

    /** Works on frames [start, end) of a stereo snapshot, easing in and out over about a frame at
        the edges; audio more than a frame outside the range is untouched. progress gets 0..1 and
        returns false to cancel; then (and for an empty range, invalid settings, or audio that isn't
        two channels) this returns nullptr. */
    AudioDocument::Snapshot process (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate,
                                     Settings, const std::function<bool (float)>& progress = {});
}
