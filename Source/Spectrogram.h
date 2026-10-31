#pragma once

#include "AudioDocument.h"

/** Draws a spectrogram: time across, frequency up (log scale, 20 Hz to Nyquist), level as colour
    from -130 dBFS (black) to 0 dBFS (white). Each pixel column is one Blackman-Harris windowed FFT
    centred on that column's time; a row that covers several FFT bins shows the loudest of them. */
namespace Spectrogram
{
    constexpr double minFrequency = 20.0;
    constexpr float floorDb = -130.0f;

    /** Columns cover document frames viewStart + x * framesPerPixel. */
    juce::Image render (const AudioSnapshot&, int channel, double sampleRate,
                        double viewStart, double framesPerPixel, int width, int height);

    /** Vertical position (0 = top) of a frequency, and back. */
    float frequencyToY (double frequency, double sampleRate, int height);
    double yToFrequency (float y, double sampleRate, int height);

    /** Level in dBFS of a sine of the given frequency and amplitude, as the analysis sees it
        (for tests: the scale is calibrated so a full-scale sine reads 0 dB). */
    float measurePeakDb (const juce::AudioBuffer<float>&, int channel, int centreFrame, double sampleRate, double frequency);
}
