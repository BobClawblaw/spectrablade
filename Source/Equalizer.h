#pragma once

#include "AudioEdits.h"

/** Equalizer bands: second-order filters made from their analogue prototypes by the bilinear
    transform, with the frequency prewarped so each band's frequency lands where it is asked. */
namespace Equalizer
{
    enum class BandType
    {
        peak,       // a bell: gainDb at the frequency, q sets the width
        lowShelf,   // gainDb below the frequency, half of it at the frequency
        highShelf,  // gainDb above the frequency, half of it at the frequency
        highPass,   // -3 dB at the frequency with q = 0.7071, 12 dB an octave below it
        lowPass     // -3 dB at the frequency with q = 0.7071, 12 dB an octave above it
    };

    struct Band
    {
        BandType type = BandType::peak;
        double frequency = 1000.0;
        double gainDb = 0.0;    // unused by the passes
        double q = 0.7071067811865476;
    };

    constexpr double maxGainDb = 48.0, minQ = 0.05, maxQ = 50.0;

    /** Whether the band can be made at this rate: a frequency above 0 and below half the rate, a q
        from minQ to maxQ and a gain within maxGainDb either way. */
    bool isValid (const Band&, double sampleRate);

    /** Whether the band changes nothing: a bell or shelf at 0 dB (a pass always changes something). */
    bool isFlat (const Band&);

    /** One band on one channel, run a block at a time: its state carries from block to block, so
        any split of the audio into blocks gives the same samples as one pass. */
    class Filter
    {
    public:
        /** New coefficients for the band at this rate; the state is kept, so a change mid-stream
            doesn't click more than the change itself. The band must be valid. */
        void setBand (const Band&, double sampleRate);
        void reset() noexcept                       { z1 = z2 = 0.0; }
        void process (float* samples, int numSamples) noexcept;

    private:
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
    };

    /** The band's gain at a frequency, in dB; 0 for a band that isn't valid. */
    double responseDb (const Band&, double sampleRate, double frequency);

    /** Runs the bands, in order, over [start, end) on every channel. The filters first run over up
        to a second of the audio before the range, so the range starts as it would in a longer
        pass; that audio is kept as it was. Nothing rings past the range's end. Nullptr for no
        bands, a band that isn't valid or an empty range. */
    AudioEdits::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const std::vector<Band>&);
}
