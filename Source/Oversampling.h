#pragma once

#include <JuceHeader.h>

/** Four times oversampling round a waveshaper: the signal is raised to four times the rate through a
    windowed-sinc low-pass (taps long at the high rate, a Kaiser window, cut at the original Nyquist),
    shaped sample by sample up there, and brought back down through the same low-pass, so the
    harmonics the shaping makes above the original Nyquist are filtered off instead of folding back.
    Each pass delays by half the filter, so what comes out is `latency` frames late at the original
    rate (an exact number of frames: the filter's half is a multiple of four). Run a sample at a
    time with its history carried, so any split of the audio into blocks gives the same samples. */
class Oversampler4
{
public:
    static constexpr int factor = 4, taps = 161, latency = (taps - 1) / 4;   // 40 frames

    void reset() noexcept;

    /** Shapes a block in place: every high-rate sample through `shape` (a double in, a double out);
        the result lands `latency` frames late. */
    template <typename Shape>
    void process (float* samples, int numFrames, Shape&& shape) noexcept
    {
        for (int i = 0; i < numFrames; ++i)
            samples[i] = (float) tick ((double) samples[i], shape);
    }

    /** One frame in, one out (late by `latency`). */
    template <typename Shape>
    double tick (double in, Shape&& shape) noexcept
    {
        // The frame into the low-rate history (newest first); four high-rate samples out of the
        // upsampling filter's four phases, each shaped, into the high-rate history; one frame out
        // of the downsampling filter over that history, taken on the first phase (the one in step
        // with the frames, so the two filters' delays add up to whole frames).
        const auto& kernel = kernelTaps();
        lowPos = lowPos == 0 ? (int) lowHistory.size() - 1 : lowPos - 1;
        lowHistory[(size_t) lowPos] = in;
        auto out = 0.0;

        for (int phase = 0; phase < factor; ++phase)
        {
            auto y = 0.0;
            auto index = lowPos;

            for (int k = phase; k < taps; k += factor)
            {
                y += kernel[(size_t) k] * lowHistory[(size_t) index];

                if (++index == (int) lowHistory.size())
                    index = 0;
            }

            highPos = highPos == 0 ? taps - 1 : highPos - 1;
            highHistory[(size_t) highPos] = shape (y * factor);

            if (phase == 0)
            {
                index = highPos;

                for (int k = 0; k < taps; ++k)
                {
                    out += kernel[(size_t) k] * highHistory[(size_t) index];

                    if (++index == taps)
                        index = 0;
                }
            }
        }

        return out;
    }

    /** One frame in; out comes the largest magnitude among the four high-rate samples the
        upsampling filter makes for the frame (taps - 1) / 8 frames back (upLatency): the frame's
        true peak, as near as the filter draws the waveform between samples. No shaping, no
        downsampling. */
    double peakTick (double in) noexcept;
    static constexpr int upLatency = (taps - 1) / 8;   // 20 frames

    /** The filter's response at a frequency (relative to the high rate, 0 to 0.5), in dB. */
    static double responseDb (double frequencyOverHighRate);

private:
    static const std::array<double, taps>& kernelTaps();

    std::array<double, (taps + factor - 1) / factor + 1> lowHistory {};
    std::array<double, taps> highHistory {};
    int lowPos = 0, highPos = 0;
};
