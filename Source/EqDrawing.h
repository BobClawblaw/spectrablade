#pragma once

#include <JuceHeader.h>
#include "Equalizer.h"

/** What the EQ drawings share: the well they are drawn in, the frequency and gain grid, the spectrum
    of what is playing behind the curves, the glowing curve and the numbered handles. Frequency runs
    across on a log scale from 20 Hz to 20 kHz; each drawing gives its own scales. */
namespace EqDrawing
{
    constexpr double minHz = 20.0, maxHz = 20000.0;

    /** Each band's colour, the same in every theme (five, then round again). */
    juce::Colour bandColour (int index);

    /** The drawing's frequency scale across an area: 20 Hz at its left edge, 20 kHz at its right. */
    float xForFrequency (juce::Rectangle<float> area, double frequency);
    double frequencyForX (juce::Rectangle<float> area, float x);

    /** A spectrum held for drawing: each new reading's bins, or the held ones fallen back 0.6 dB (15 dB
        a second at 25 readings), whichever is louder, so peaks linger long enough to read. */
    struct HeldSpectrum
    {
        std::vector<float> levels;   // dB by bin, as Analysis::spectrumDb
        double rate = 0.0;

        void take (const std::vector<float>& reading, double sampleRate);

        /** Reads the newest output through read (2 channels, Analysis::fftSize frames; it returns the
            rate, 0 for none) into the held spectrum. False when there was nothing to read. */
        bool read (const std::function<double (juce::AudioBuffer<float>&)>& read, juce::AudioBuffer<float>& scratch);
    };

    /** The well, darker toward the bottom. */
    void paintWell (juce::Graphics&, juce::Rectangle<float> area);

    /** The spectrum as a shaded shape, -90 dBFS at the area's bottom to 0 at its top. Where a point
        covers less than a bin (the lows) it is drawn between the two bins round it, so it is smooth;
        where it covers several, the loudest of them. */
    void paintSpectrum (juce::Graphics&, juce::Rectangle<float> area, const HeldSpectrum&);

    /** Lines at 30 Hz to 10 kHz with their frequencies, and every 6 dB within rangeDb (labelled at
        each 12), the 0 dB line stronger. */
    void paintGrid (juce::Graphics&, juce::Rectangle<float> area, double rangeDb);

    /** A curve glowing in the accent colour. */
    void paintGlow (juce::Graphics&, const juce::Path&);

    /** A numbered handle: a disc in its band's colour with a halo, larger when strong, ringed when
        picked out. */
    void paintHandle (juce::Graphics&, juce::Point<float>, int index, bool strong, bool picked);

    /** A small tag by a handle (its upper right) naming the channel a band works on: M, S, L or R;
        nothing for a band on every channel. */
    void paintChannelTag (juce::Graphics&, juce::Point<float> handle, int index, Equalizer::Channel);

    /** A band switched off: a ring with its number. */
    void paintOffRing (juce::Graphics&, juce::Point<float>, int index, bool strong);

    /** A box of text beside a point (above it, or under it near the top), kept inside the area. */
    void paintReadout (juce::Graphics&, juce::Rectangle<float> area, juce::Point<float> at, const juce::String& text, juce::Colour edge);
}
