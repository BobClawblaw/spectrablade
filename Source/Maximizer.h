#pragma once

#include "Dynamics.h"
#include "Oversampling.h"

/** A maximizer: a true-peak limiter for the last step of a master, beyond the plain Limiter. The
    gain drives the sound into the ceiling, and no sample comes out above it; the peaks between
    samples are held under it too, as near as a four-times interpolation draws them (Oversampler4's
    peak). Character sets how the limiter moves: Transparent looks 10 ms ahead and recovers slowly
    (releases of 400 and 60 ms), so little pumps or distorts; Loud looks 2 ms ahead and recovers
    fast (80 and 10 ms), louder and rougher; Balanced sits between (5 ms; 200 and 30 ms). The two
    releases run side by side, a slow one for sustained loudness and a fast one for short peaks,
    and the gain is their blend (the character leaning toward one or the other). Transient Emphasis
    lifts the attacks before the limiter (by up to 6 dB, where a 0.5 ms envelope stands over a 10 ms
    one), so hits keep their edge through it. The whole comes out late by the peak detector's 20
    frames plus the look-ahead; the live effect reports it and the offline apply takes it off. */
namespace Maximizer
{
    enum class Character { transparent, balanced, loud };

    const juce::StringArray& characterNames();

    struct Settings
    {
        double gainDb = 0.0;             // 0 to 24 dB into the ceiling
        double ceilingDb = -1.0;         // -24 to 0 dBTP
        Character character = Character::balanced;
        double emphasisPercent = 0.0;    // 0 to 100: how much the attacks are lifted
    };

    bool isValid (const Settings&);

    /** The look-ahead in ms and the slow and fast releases in ms for a character. */
    struct Timing { double lookaheadMs, slowReleaseMs, fastReleaseMs, blend; };
    Timing timingOf (Character);

    /** The frames the core delays the audio by at this rate: the peak detector's plus the look-ahead. */
    int latencyFrames (Character, double sampleRate);

    class Core
    {
    public:
        void prepare (int numChannels, double sampleRate);
        void reset();
        void set (const Settings&);   // valid at the prepared rate; a change of character resets

        /** In place; each frame comes out latencyFrames late. */
        void process (float* const* channels, int numChannels, int numFrames) noexcept;

        /** The gain at the last frame, in dB (0 or less), for a meter. */
        double getReductionDb() const noexcept    { return reductionDb; }

    private:
        double rate = 48000.0;
        Settings settings;
        int ahead = 1, channelsPrepared = 0;
        double input = 1.0, top = 1.0, blend = 0.5, reductionDb = 0.0;
        double fastUp = 0.0, fastDown = 0.0, slowUp = 0.0, slowDown = 0.0, envFast = 0.0, envSlow = 0.0;
        std::vector<Oversampler4> detectors;                 // a channel each
        Dynamics::LimiterCore slow, fast;
        juce::AudioBuffer<float> delay;                      // the audio, the detector's delay plus the look-ahead
        std::vector<double> emphasisGains;                   // the attack lift for each delayed frame, applied with its frame
        int delayPos = 0;
    };

    /** Runs the maximizer over [start, end): the result lands where the audio was, primed on up to
        a second of the audio before. Nullptr for settings that aren't valid or an empty range. */
    AudioEdits::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&);
}
