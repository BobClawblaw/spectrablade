#pragma once

#include "AudioEdits.h"

/** Modulation effects: chorus and flanger (a delay swept by a slow oscillator, mixed with the dry
    audio) and a phaser (a chain of all-pass filters swept the same way). Each runs a frame at a
    time in a core that the offline effect and the live built-in effect share, so the two give the
    same samples. The oscillator starts at the top of its cycle (the shortest delay, the lowest
    phaser frequency) on the first frame; the second channel (and every other one after it) runs
    it the spread later in its cycle, which widens a stereo image. */
namespace Modulation
{
    /** A swept delay: chorus with a longer delay and no feedback, flanger with a short one and feedback. */
    struct Delay
    {
        double rateHz = 0.8;          // the sweep's rate, 0.01 to 10 Hz
        double delayMs = 15.0;        // the shortest delay, 0 to 50 ms (never under one frame)
        double depthMs = 5.0;         // how much longer it gets at the top of the sweep, 0 to 50 ms
        double feedback = 0.0;        // the share of the delayed sound fed back in, -0.95 to 0.95
        double mixPercent = 50.0;     // the delayed sound's share of the output, 0 to 100 %
        double spreadDegrees = 90.0;  // the second channel's sweep, this far behind, 0 to 180
    };

    Delay chorusDefaults();
    Delay flangerDefaults();
    bool isValid (const Delay&);

    struct Phaser
    {
        double rateHz = 0.5;          // 0.01 to 10 Hz
        double lowHz = 300.0;         // the sweep's range, 20 Hz to 20 kHz (kept under half the rate)
        double highHz = 3000.0;       // at least lowHz
        int stages = 6;               // all-pass filters, an even number from 2 to 12
        double feedback = 0.4;        // -0.95 to 0.95
        double mixPercent = 50.0;     // 0 to 100 %; 50 gives the deepest notches
        double spreadDegrees = 90.0;  // 0 to 180
    };

    bool isValid (const Phaser&);

    /** The swept delay, run a block at a time. */
    class DelayCore
    {
    public:
        /** Sizes a delay line a channel for the longest delay at this rate and starts afresh. */
        void prepare (double sampleRate, int numChannels);
        void reset();

        /** New settings (valid); the delay line and the sweep carry on. */
        void set (const Delay& d) noexcept      { settings = d; }

        void process (float* const* channels, int numChannels, int numFrames) noexcept;

    private:
        Delay settings;
        double rate = 48000.0, phase = 0.0;
        std::vector<std::vector<float>> lines;
        int write = 0;
    };

    /** The phaser, run a block at a time. */
    class PhaserCore
    {
    public:
        void prepare (double sampleRate, int numChannels);
        void reset();
        void set (const Phaser& p) noexcept     { settings = p; }
        void process (float* const* channels, int numChannels, int numFrames) noexcept;

    private:
        static constexpr int maxStages = 12;
        Phaser settings;
        double rate = 48000.0, phase = 0.0;
        std::vector<std::array<double, maxStages>> state;
        std::vector<double> last;
    };

    /** The effects over [start, end) of every channel, starting from silence in the delay line and
        filters (as the live effect starts); nothing rings past the end. Nullptr for settings that
        aren't valid or an empty range. */
    AudioEdits::Snapshot sweptDelay (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Delay&);
    AudioEdits::Snapshot phaser (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Phaser&);
}
