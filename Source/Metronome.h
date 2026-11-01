#pragma once

#include <JuceHeader.h>

/** The metronome's beat grid and click. Beats fall on the session's tempo from frame 0, each
    rounded to the nearest frame, so the grid never drifts. */
namespace Metronome
{
    /** How long a click sounds. */
    constexpr double clickSeconds = 0.03;

    /** The frame beat k starts on. */
    juce::int64 beatFrame (juce::int64 beat, double tempo, double rate);

    /** The beat sounding at a frame: the last one starting at or before it. */
    juce::int64 beatAt (juce::int64 frame, double tempo, double rate);

    /** The frames in a bar (beatsPerBar beats, rounded). */
    juce::int64 barFrames (double tempo, int beatsPerBar, double rate);

    /** A change of tempo, time signature or both at the start of a bar (counted from 0). */
    struct TempoChange
    {
        juce::int64 bar = 1;
        double tempo = 120.0;              // beats a minute
        int beatsPerBar = 4;

        bool operator== (const TempoChange& o) const noexcept
        {
            return bar == o.bar && juce::exactlyEqual (tempo, o.tempo) && beatsPerBar == o.beatsPerBar;
        }
    };

    /** Grid lines on the beat grid: every beat (bars 0), or the start of every bars-th bar. */
    struct Grid
    {
        int bars = 0;
    };

    /** A session's beats: its tempo and time signature from the start, then each change from its
        bar on (changes sorted by bar, each after the first bar). Beats count from 0 at frame 0
        and each is rounded to the nearest frame, so the grid never drifts; with no changes the
        beats are those of beatFrame() above. It refers to the changes rather than copying them, so
        it allocates nothing and may be used on the audio thread. */
    class TempoMap
    {
    public:
        TempoMap (double tempo, int beatsPerBar, const std::vector<TempoChange>& laterChanges, double sampleRate) noexcept
            : startTempo (tempo), startBeats (beatsPerBar), changes (laterChanges), rate (sampleRate) {}

        juce::int64 beatFrame (juce::int64 beat) const;
        juce::int64 beatAt (juce::int64 frame) const;

        /** The bar (from 0) a beat is in and the beat's place in it (from 0). */
        std::pair<juce::int64, int> barAndBeat (juce::int64 beat) const;
        juce::int64 barStartBeat (juce::int64 bar) const;
        juce::int64 barFrame (juce::int64 bar) const        { return beatFrame (barStartBeat (bar)); }
        juce::int64 barAt (juce::int64 frame) const         { return barAndBeat (beatAt (frame)).first; }

        double tempoAt (juce::int64 frame) const;
        int beatsPerBarAt (juce::int64 frame) const;

        /** The shortest beat and the shortest bar anywhere in the session, in frames. */
        double shortestBeat() const;
        double shortestBar() const;

        /** The grid line at or before frame and the one after it. */
        std::pair<juce::int64, juce::int64> linesAround (juce::int64 frame, Grid) const;

        /** Calls back with the frame of every grid line from first to last (frames), in order,
            and its beat. */
        void forEachLine (juce::int64 first, juce::int64 last, Grid, const std::function<void (juce::int64 frame, juce::int64 beat)>&) const;

    private:
        struct Segment
        {
            juce::int64 bar = 0, beat = 0;
            double frame = 0.0, beatLength = 0.0;
            int beatsPerBar = 4;
        };

        /** The segment holding what is looked for: the last one before a segment for which
            `before` is true. */
        template <typename Before>
        Segment find (Before before) const;

        /** The frame a segment's first beat is rounded to. */
        static juce::int64 startOf (const Segment& s)       { return (juce::int64) std::llround (s.frame); }

        double startTempo;
        int startBeats;
        const std::vector<TempoChange>& changes;
        double rate;
    };

    /** The click, `offset` frames after its beat starts (0 once it has ended): a short sine that
        dies away, higher and louder on the first beat of a bar. */
    float sample (juce::int64 offset, bool accent, double rate);
}
