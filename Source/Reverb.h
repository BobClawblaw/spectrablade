#pragma once

#include "AudioEdits.h"

/** An algorithmic reverb: the sound, after a pre-delay, is smeared by four all-pass filters and fed
    into eight delay lines whose outputs are mixed back into their inputs by a lossless matrix (a
    feedback delay network). Each line loses just enough on every pass for the sound to fall 60 dB
    in the decay time, and a low-pass in each line takes the highs away faster as damping rises.
    Two different mixes of the lines make the left and right sides. One core, run a frame at a time,
    serves the offline effect and the live built-in effect, so the two give the same samples. */
namespace RoomReverb
{
    struct Settings
    {
        double preDelayMs = 20.0;      // 0 to 200 ms before the reverb begins
        double decaySeconds = 2.0;     // the time to fall 60 dB, 0.1 to 20 s
        double sizePercent = 100.0;    // the delay lines' lengths, 50 to 150 % (about 30 to 75 ms at 100 %)
        double dampingPercent = 30.0;  // 0 (as bright as it goes in) to 100 (dark)
        double widthPercent = 100.0;   // the reverb's stereo width, 0 (mono) to 100 %
        double mixPercent = 30.0;      // the reverb's share of the output, 0 to 100 %
    };

    bool isValid (const Settings&);

    /** How long the reverb rings on after the sound stops: the pre-delay plus the decay time. */
    double tailSeconds (const Settings&);

    class Core
    {
    public:
        static constexpr int numLines = 8;

        /** Sizes the lines for the largest size and pre-delay at this rate and starts afresh. */
        void prepare (double sampleRate);
        void reset();

        /** New settings (valid). A new size moves the lines' read points, so it may click. */
        void set (const Settings&);

        /** Adds the reverb in place: each channel becomes its dry share plus the reverb's (the
            channels' average goes in; even channels get the left side, odd ones the right, a
            single channel their middle). */
        void process (float* const* channels, int numChannels, int numFrames) noexcept;

    private:
        Settings settings;
        double rate = 48000.0;
        std::vector<float> preDelay;
        int preWrite = 0, preFrames = 0;

        struct Diffuser
        {
            std::vector<float> line;
            int pos = 0;
        };
        std::array<Diffuser, 4> diffusers;

        std::array<std::vector<float>, numLines> lines;
        std::array<int, numLines> lengths {}, writes {};
        std::array<double, numLines> gains {}, lows {};
        double damping = 0.0;
    };

    /** Adds the reverb to [start, end) (its dry share and the reverb), and lets it ring on into the
        audio after the range, added on top of it, for the tail's length or to the end of the file.
        Starts from silence, as the live effect does. Nullptr for settings that aren't valid or an
        empty range. */
    AudioEdits::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&);
}
