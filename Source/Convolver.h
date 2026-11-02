#pragma once

#include "AudioEdits.h"

/** Convolution reverb: the sound convolved with an impulse response (a recording of a space's
    answer to a click), by uniformly partitioned convolution with the FFT. The core works in blocks
    of blockSize frames, so it puts out each frame blockSize frames after it comes in; the live
    effect reports that as latency and the offline effect takes it off, and both run the same core,
    so they give the same samples. */
namespace Convolver
{
    constexpr int blockSize = 512;
    constexpr double maxSeconds = 10.0;

    /** An impulse response as it was read: one or two channels at its own rate. */
    struct Impulse
    {
        juce::AudioBuffer<float> samples;
        double sampleRate = 0.0;
        juce::String name;
    };

    /** Why the impulse can't be used (empty if it can): one or two channels, at a rate, up to
        maxSeconds long, and not silent. */
    juce::String problemWith (const Impulse&);

    /** Reads an impulse response from an audio file (error set, and an empty impulse, if it can't). */
    Impulse read (const juce::File&, juce::AudioFormatManager&, juce::String& error);

    /** The impulse at this rate (resampled when it differs) and scaled so its loudest channel's
        energy (the sum of its squared samples) is 1, so the reverb of noise comes out about as
        loud as the noise. */
    juce::AudioBuffer<float> atRate (const Impulse&, double sampleRate);

    class Core
    {
    public:
        Core();
        ~Core();

        /** Splits the impulse (already at the rate, as atRate gives it) into blocks and sizes
            everything for this many channels; starts from silence. Channel c uses the impulse's
            channel c % its channel count. An impulse with no frames gives no reverb. */
        void prepare (const juce::AudioBuffer<float>& impulse, int numChannels);
        void reset();

        /** The reverb's share of the output, 0 to 100 %. */
        void setMix (double percent) noexcept      { mix = juce::jlimit (0.0, 100.0, percent) / 100.0; }

        /** In place: each frame comes out blockSize frames late, as its dry share plus the reverb's. */
        void process (float* const* channels, int numChannels, int numFrames) noexcept;

    private:
        void convolveBlock (int channel) noexcept;

        juce::dsp::FFT fft;
        double mix = 0.3;
        int partitions = 0, pos = 0, fdlPos = 0;
        std::vector<std::vector<std::vector<float>>> spectra;   // [impulse channel][partition], 2N floats each
        struct Channel
        {
            std::vector<float> input, previous, output, dry;     // blockSize each
            std::vector<std::vector<float>> history;             // the last `partitions` input spectra
            int impulseChannel = 0;
        };
        std::vector<Channel> channels;
        std::vector<float> work, sum;
    };

    /** Adds the impulse's reverb to [start, end) (its dry share and the reverb), ringing on into
        the audio after the range, added on top of it, for the impulse's length (or to the end of
        the file). Nullptr for an impulse that can't be used, a mix outside 0 to 100 % or an empty range. */
    AudioEdits::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Impulse&, double mixPercent);
}
