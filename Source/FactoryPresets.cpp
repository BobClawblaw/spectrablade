#include "EffectPresets.h"
#include "GraphicEq.h"

/* The presets that come with SpectraBlade: starting points for the jobs a restoration, mixing or
   mastering suite is used for (cleaning up dialogue, taming a vocal, finishing a master for its
   loudness target), each named for its job. They are settings to start from and adjust by ear, not
   measurements; each is a set of the effect's own values (a choice by its index), as a user's preset is. */

namespace EffectPresets
{
namespace
{
    using Table = std::map<juce::String, std::vector<Factory>>;

    /** A curve given at the ten octave bands (31.5 Hz to 16 kHz), read at any frequency along a
        straight line between them in octaves, flat beyond the ends. */
    double curveAt (const std::array<double, 10>& octaves, double frequency)
    {
        const auto& at = GraphicEq::centres (10);

        if (frequency <= at.front())
            return octaves.front();

        if (frequency >= at.back())
            return octaves.back();

        for (size_t i = 1; i < at.size(); ++i)
        {
            if (frequency <= at[i])
            {
                const auto t = std::log2 (frequency / at[i - 1]) / std::log2 (at[i] / at[i - 1]);
                return octaves[i - 1] + t * (octaves[i] - octaves[i - 1]);
            }
        }

        return 0.0;
    }

    /** Graphic EQ presets for 10 or 31 bands from the same curves (31 bands read between the octaves). */
    std::vector<Factory> graphic (int numBands)
    {
        const std::vector<std::pair<const char*, std::array<double, 10>>> curves {
            { "Flat", { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
            { "Low cut (rumble and wind)", { -12, -8, -3, 0, 0, 0, 0, 0, 0, 0 } },
            { "Warmth", { 2, 3, 2, 1, 0, 0, 0, 0, -1, -1 } },
            { "Vocal presence", { -2, -2, -1, -1, 0, 1, 3, 3, 1, 0 } },
            { "Dialogue clarity", { -6, -4, -2, -1, 0, 1, 2, 2, 0, -2 } },
            { "Air", { 0, 0, 0, 0, 0, 0, 0, 1, 3, 4 } },
            { "Smile (bass and treble up)", { 4, 3, 1, 0, -1, -1, 0, 1, 3, 4 } },
            { "Bass boost", { 6, 5, 3, 1, 0, 0, 0, 0, 0, 0 } },
            { "Treble boost", { 0, 0, 0, 0, 0, 0, 1, 3, 5, 6 } },
            { "Telephone", { -12, -12, -8, -2, 2, 4, 3, -4, -12, -12 } },
            { "AM radio", { -12, -8, -2, 1, 3, 3, 1, -3, -10, -12 } },
        };

        const auto& centres = GraphicEq::centres (numBands);
        std::vector<Factory> out;

        for (auto& [name, octaves] : curves)
        {
            Values v;

            for (size_t i = 0; i < centres.size(); ++i)
                v["band" + juce::String ((int) i + 1)] = std::round (curveAt (octaves, centres[i]) * 10.0) / 10.0;

            out.push_back ({ name, std::move (v) });
        }

        return out;
    }

    /** A parametric EQ preset: each of its five bands' on, type, frequency, gain and Q. */
    struct Band { bool on; int type; double frequency, gain, q; };

    Values parametric (const std::array<Band, 5>& bands)
    {
        Values v { { "linearPhase", 0.0 } };

        for (size_t i = 0; i < bands.size(); ++i)
        {
            const auto n = "band" + juce::String ((int) i + 1);
            v[n + "On"] = bands[i].on ? 1.0 : 0.0;
            v[n + "Type"] = bands[i].type;
            v[n + "Channel"] = 0.0;
            v[n + "Frequency"] = bands[i].frequency;
            v[n + "Gain"] = bands[i].gain;
            v[n + "Q"] = bands[i].q;
        }

        return v;
    }

    /** A dynamic EQ preset: each band as on, shape (0 bell, 1 low shelf, 2 high shelf), mode (0 over, 1 under), frequency, Q, gain,
        threshold, ratio, attack and release, on every channel; the bands not named are off at their starting places. */
    Values dynamicEq (const std::vector<std::array<double, 10>>& bands, double output = 0.0)
    {
        Values v { { "output", output } };
        const std::array<const char*, 10> names { "On", "Shape", "Mode", "Frequency", "Q", "Gain", "Threshold", "Ratio", "Attack", "Release" };
        const std::array<std::array<double, 10>, 5> starts {{ { 0, 1, 0, 100, 1, 0, -30, 4, 10, 150 }, { 0, 0, 0, 400, 1, 0, -30, 4, 10, 150 },
                                                              { 0, 0, 0, 1000, 1, 0, -30, 4, 10, 150 }, { 0, 0, 0, 3000, 1, 0, -30, 4, 10, 150 },
                                                              { 0, 2, 0, 8000, 1, 0, -30, 4, 10, 150 } }};

        for (size_t b = 0; b < 5; ++b)
        {
            for (size_t i = 0; i < 10; ++i)
                v["band" + juce::String ((int) b + 1) + names[i]] = b < bands.size() ? bands[b][i] : starts[b][i];

            v["band" + juce::String ((int) b + 1) + "Channel"] = 0.0;
        }

        return v;
    }

    /** An exciter preset: the crossovers, then each band's drive, curve (0 tube, 1 tape, 2 transistor) and mix, then the output. */
    Values exciter (double lowCrossover, double highCrossover, const std::array<std::array<double, 3>, 3>& bands, double output)
    {
        Values v { { "lowCrossover", lowCrossover }, { "highCrossover", highCrossover }, { "output", output } };
        const std::array<const char*, 3> names { "low", "mid", "high" };
        const std::array<const char*, 3> settings { "Drive", "Curve", "Mix" };

        for (size_t b = 0; b < 3; ++b)
            for (size_t s = 0; s < 3; ++s)
                v[names[b] + juce::String (settings[s])] = bands[b][s];

        return v;
    }

    /** A multiband preset: the crossovers, then each band's threshold, ratio, attack, release and makeup, then the output. */
    Values multiband (double lowCrossover, double highCrossover, const std::array<std::array<double, 5>, 3>& bands, double output)
    {
        Values v { { "lowCrossover", lowCrossover }, { "highCrossover", highCrossover }, { "output", output } };
        const std::array<const char*, 3> names { "low", "mid", "high" };
        const std::array<const char*, 5> settings { "Threshold", "Ratio", "Attack", "Release", "Makeup" };

        for (size_t b = 0; b < 3; ++b)
            for (size_t s = 0; s < 5; ++s)
                v[juce::String (names[b]) + settings[s]] = bands[b][s];

        return v;
    }

    enum EqType { bell, lowShelf, highShelf, highPass, lowPass };

    Table makeTable()
    {
        Table t;

        // Level
        t["spectrablade:gain"] = {
            { "Up 3 dB", { { "db", 3.0 } } }, { "Up 6 dB", { { "db", 6.0 } } },
            { "Down 3 dB", { { "db", -3.0 } } }, { "Down 6 dB", { { "db", -6.0 } } }, { "Down 12 dB", { { "db", -12.0 } } } };
        t["spectrablade:normalize"] = {
            { "Master peak (-0.1 dBFS)", { { "db", -0.1 }, { "apart", 0.0 } } },
            { "Headroom for encoding (-1 dBFS)", { { "db", -1.0 }, { "apart", 0.0 } } },
            { "Dialogue stem (-3 dBFS)", { { "db", -3.0 }, { "apart", 0.0 } } },
            { "Rough level (-6 dBFS)", { { "db", -6.0 }, { "apart", 0.0 } } },
            { "Each channel to -1 dBFS", { { "db", -1.0 }, { "apart", 1.0 } } } };
        t["spectrablade:normalize-regions"] = {
            { "Master peak (-0.1 dBFS)", { { "db", -0.1 } } }, { "Headroom for encoding (-1 dBFS)", { { "db", -1.0 } } },
            { "Sound effects (-3 dBFS)", { { "db", -3.0 } } }, { "Rough level (-6 dBFS)", { { "db", -6.0 } } } };
        t["spectrablade:match-loudness"] = {
            { "Broadcast, EBU R128 (-23 LUFS)", { { "target", -23.0 }, { "ceiling", -1.0 }, { "limit", 0.0 } } },
            { "Broadcast, ATSC A/85 (-24 LUFS)", { { "target", -24.0 }, { "ceiling", -2.0 }, { "limit", 0.0 } } },
            { "Podcast (-16 LUFS)", { { "target", -16.0 }, { "ceiling", -1.0 }, { "limit", 1.0 } } },
            { "Streaming music (-14 LUFS)", { { "target", -14.0 }, { "ceiling", -1.0 }, { "limit", 1.0 } } },
            { "Audiobook (-20 LUFS)", { { "target", -20.0 }, { "ceiling", -3.0 }, { "limit", 1.0 } } },
            { "Loud master (-9 LUFS)", { { "target", -9.0 }, { "ceiling", -1.0 }, { "limit", 1.0 } } } };
        t["spectrablade:level-speech"] = {
            { "Podcast", { { "target", -16.0 }, { "boost", 12.0 }, { "cut", 12.0 }, { "silence", -50.0 } } },
            { "Broadcast dialogue", { { "target", -23.0 }, { "boost", 10.0 }, { "cut", 10.0 }, { "silence", -55.0 } } },
            { "Audiobook", { { "target", -20.0 }, { "boost", 8.0 }, { "cut", 8.0 }, { "silence", -55.0 } } },
            { "Gentle (interview)", { { "target", -20.0 }, { "boost", 6.0 }, { "cut", 6.0 }, { "silence", -50.0 } } },
            { "Strong (phone recording)", { { "target", -18.0 }, { "boost", 20.0 }, { "cut", 15.0 }, { "silence", -45.0 } } } };
        t["spectrablade:gain-ramp"] = {
            { "Swell in (-12 to 0 dB)", { { "from", -12.0 }, { "to", 0.0 } } },
            { "Dip out (0 to -12 dB)", { { "from", 0.0 }, { "to", -12.0 } } },
            { "Long fade out (0 to -60 dB)", { { "from", 0.0 }, { "to", -60.0 } } },
            { "Long fade in (-60 to 0 dB)", { { "from", -60.0 }, { "to", 0.0 } } } };
        t["spectrablade:fade-edges"] = {
            { "Click-free (5 ms)", { { "ms", 5.0 } } }, { "Short (20 ms)", { { "ms", 20.0 } } },
            { "Medium (100 ms)", { { "ms", 100.0 } } }, { "Long (1 s)", { { "ms", 1000.0 } } } };

        // Stereo
        t["spectrablade:balance"] = {
            { "Centre", { { "pan", 0.0 } } }, { "A little left", { { "pan", -20.0 } } }, { "A little right", { { "pan", 20.0 } } } };
        t["spectrablade:stereo-width"] = {
            { "Mono", { { "width", 0.0 } } }, { "Narrower", { { "width", 70.0 } } },
            { "Wider", { { "width", 130.0 } } }, { "Extra wide", { { "width", 160.0 } } } };
        t["spectrablade:maximizer"] = {
            { "Gentle (-1 dBTP)", { { "gain", 3.0 }, { "ceiling", -1.0 }, { "character", 0.0 }, { "emphasis", 20.0 } } },
            { "Streaming (-1 dBTP)", { { "gain", 6.0 }, { "ceiling", -1.0 }, { "character", 1.0 }, { "emphasis", 30.0 } } },
            { "Loud (-0.3 dBTP)", { { "gain", 10.0 }, { "ceiling", -0.3 }, { "character", 2.0 }, { "emphasis", 40.0 } } },
            { "Punchy drums", { { "gain", 6.0 }, { "ceiling", -0.5 }, { "character", 1.0 }, { "emphasis", 100.0 } } } };
        t["spectrablade:exciter"] = {
            { "Warm lows", exciter (200, 3000, {{ { 9, 0, 60 }, { 0, 0, 0 }, { 0, 0, 0 } }}, 0) },
            { "Tape glue", exciter (200, 3000, {{ { 6, 1, 50 }, { 6, 1, 50 }, { 6, 1, 50 } }}, 0) },
            { "Air and presence", exciter (250, 4000, {{ { 0, 0, 0 }, { 4, 0, 30 }, { 10, 0, 50 } }}, 0) },
            { "Vocal edge", exciter (200, 2500, {{ { 0, 0, 0 }, { 8, 2, 35 }, { 12, 0, 45 } }}, 0) },
            { "Crunch", exciter (150, 2000, {{ { 12, 2, 70 }, { 18, 2, 80 }, { 12, 2, 60 } }}, -2) } };
        t["spectrablade:multiband-width"] = {
            { "Mono below 120 Hz", { { "lowCrossover", 120.0 }, { "highCrossover", 2000.0 }, { "low", 0.0 }, { "mid", 100.0 }, { "high", 100.0 } } },
            { "Tight low end, wide top", { { "lowCrossover", 120.0 }, { "highCrossover", 2500.0 }, { "low", 0.0 }, { "mid", 100.0 }, { "high", 140.0 } } },
            { "Wider highs", { { "lowCrossover", 150.0 }, { "highCrossover", 3000.0 }, { "low", 100.0 }, { "mid", 100.0 }, { "high", 150.0 } } },
            { "Narrower middle", { { "lowCrossover", 150.0 }, { "highCrossover", 2000.0 }, { "low", 100.0 }, { "mid", 60.0 }, { "high", 100.0 } } } };
        t["spectrablade:pan-ramp"] = {
            { "Left to right", { { "from", -100.0 }, { "to", 100.0 } } }, { "Right to left", { { "from", 100.0 }, { "to", -100.0 } } },
            { "Drift left to right", { { "from", -40.0 }, { "to", 40.0 } } } };
        t["spectrablade:centre-channel"] = {
            { "Remove vocals (karaoke)", { { "keep", 0.0 }, { "low", 100.0 }, { "high", 8000.0 }, { "reduction", 20.0 } } },
            { "Lower the vocal a little", { { "keep", 0.0 }, { "low", 200.0 }, { "high", 6000.0 }, { "reduction", 6.0 } } },
            { "Isolate dialogue", { { "keep", 1.0 }, { "low", 150.0 }, { "high", 8000.0 }, { "reduction", 20.0 } } },
            { "Keep the centre, all frequencies", { { "keep", 1.0 }, { "low", 20.0 }, { "high", 20000.0 }, { "reduction", 30.0 } } } };
        t["spectrablade:phase-correction"] = {
            { "Stereo microphones (5 ms)", { { "most", 5.0 }, { "polarity", 0.0 } } },
            { "Tape head azimuth (1 ms)", { { "most", 1.0 }, { "polarity", 0.0 } } },
            { "Spaced pair (20 ms)", { { "most", 20.0 }, { "polarity", 0.0 } } } };

        // Repair
        t["spectrablade:noise-reduction"] = {
            { "Dialogue, light", { { "reduction", 6.0 }, { "threshold", 3.0 } } },
            { "Dialogue, medium", { { "reduction", 12.0 }, { "threshold", 3.0 } } },
            { "Dialogue, strong", { { "reduction", 20.0 }, { "threshold", 4.0 } } },
            { "Music, gentle", { { "reduction", 4.0 }, { "threshold", 2.0 } } },
            { "Steady hiss or hum", { { "reduction", 18.0 }, { "threshold", 6.0 } } } };
        t["spectrablade:click-repair"] = {
            { "Vinyl, light", { { "threshold", 24.0 }, { "length", 1.0 } } },
            { "Vinyl crackle", { { "threshold", 14.0 }, { "length", 2.0 } } },
            { "Mouth clicks", { { "threshold", 18.0 }, { "length", 1.5 } } },
            { "Large pops", { { "threshold", 18.0 }, { "length", 8.0 } } },
            { "Digital clicks", { { "threshold", 20.0 }, { "length", 0.5 } } } };
        t["spectrablade:reduce-reverb"] = {
            { "Small room", { { "time", 0.4 }, { "reduction", 8.0 } } }, { "Medium room", { { "time", 0.8 }, { "reduction", 12.0 } } },
            { "Large room", { { "time", 1.5 }, { "reduction", 15.0 } } }, { "Hall", { { "time", 2.5 }, { "reduction", 18.0 } } } };
        t["spectrablade:remove-hum"] = {
            { "50 Hz (Europe, Asia), light", { { "mains", 0.0 }, { "harmonics", 4.0 }, { "q", 30.0 } } },
            { "50 Hz, thorough", { { "mains", 0.0 }, { "harmonics", 16.0 }, { "q", 60.0 } } },
            { "60 Hz (Americas), light", { { "mains", 1.0 }, { "harmonics", 4.0 }, { "q", 30.0 } } },
            { "60 Hz, thorough", { { "mains", 1.0 }, { "harmonics", 16.0 }, { "q", 60.0 } } },
            { "Guitar amp buzz (60 Hz)", { { "mains", 1.0 }, { "harmonics", 30.0 }, { "q", 40.0 } } } };
        t["spectrablade:declip"] = {
            { "Light clipping", { { "threshold", 98.0 }, { "keep", 0.0 } } },
            { "Moderate clipping", { { "threshold", 95.0 }, { "keep", 0.0 } } },
            { "Heavy clipping", { { "threshold", 90.0 }, { "keep", 0.0 } } },
            { "Heavy, peaks allowed higher", { { "threshold", 90.0 }, { "keep", 1.0 } } } };
        t["spectrablade:silence"] = {
            { "Studio (-60 dB, 0.2 s)", { { "level", -60.0 }, { "length", 0.2 } } },
            { "Quiet room (-50 dB, 0.3 s)", { { "level", -50.0 }, { "length", 0.3 } } },
            { "Noisy room (-40 dB, 0.5 s)", { { "level", -40.0 }, { "length", 0.5 } } },
            { "Long pauses only (-45 dB, 1 s)", { { "level", -45.0 }, { "length", 1.0 } } } };
        t["spectrablade:trim-silence"] = {
            { "Digital silence (-90 dB)", { { "db", -90.0 } } }, { "Studio (-60 dB)", { { "db", -60.0 } } },
            { "Room tone (-45 dB)", { { "db", -45.0 } } } };
        t["spectrablade:attenuate-frequencies"] = {
            { "Soften (-6 dB)", { { "db", -6.0 } } }, { "Lower (-12 dB)", { { "db", -12.0 } } },
            { "Nearly remove (-30 dB)", { { "db", -30.0 } } }, { "Bring out (+6 dB)", { { "db", 6.0 } } } };

        // Time and pitch
        t["spectrablade:stretch"] = {
            { "10% slower", { { "percent", 110.0 } } }, { "10% faster", { { "percent", 90.0 } } },
            { "Half speed", { { "percent", 200.0 } } }, { "Double speed", { { "percent", 50.0 } } },
            { "PAL to film (25 to 24 fps)", { { "percent", 104.17 } } }, { "Film to PAL (24 to 25 fps)", { { "percent", 96.0 } } } };
        t["spectrablade:shift-pitch"] = {
            { "Octave down", { { "semitones", -12.0 } } }, { "Fifth down", { { "semitones", -7.0 } } },
            { "Semitone down", { { "semitones", -1.0 } } }, { "Semitone up", { { "semitones", 1.0 } } },
            { "Fifth up", { { "semitones", 7.0 } } }, { "Octave up", { { "semitones", 12.0 } } } };
        t["spectrablade:pitch-correction"] = {
            { "Natural", { { "key", 0.0 }, { "scale", 0.0 }, { "speed", 50.0 }, { "strength", 70.0 } } },
            { "Subtle", { { "key", 0.0 }, { "scale", 0.0 }, { "speed", 120.0 }, { "strength", 50.0 } } },
            { "Tight", { { "key", 0.0 }, { "scale", 0.0 }, { "speed", 10.0 }, { "strength", 100.0 } } },
            { "Hard (robotic)", { { "key", 0.0 }, { "scale", 0.0 }, { "speed", 0.0 }, { "strength", 100.0 } } } };
        t["spectrablade:crossfade-loop"] = {
            { "Tight (10 ms)", { { "ms", 10.0 } } }, { "Smooth (50 ms)", { { "ms", 50.0 } } }, { "Long (250 ms)", { { "ms", 250.0 } } } };

        // Equalizers
        t["spectrablade:eq-band"] = {
            { "Rumble cut (80 Hz high-pass)", { { "type", highPass }, { "frequency", 80.0 }, { "gain", 0.0 }, { "q", 0.71 } } },
            { "Mud cut (300 Hz)", { { "type", bell }, { "frequency", 300.0 }, { "gain", -4.0 }, { "q", 1.4 } } },
            { "Boxiness cut (500 Hz)", { { "type", bell }, { "frequency", 500.0 }, { "gain", -3.0 }, { "q", 2.0 } } },
            { "Presence (3 kHz)", { { "type", bell }, { "frequency", 3000.0 }, { "gain", 3.0 }, { "q", 1.0 } } },
            { "Harshness cut (3.5 kHz)", { { "type", bell }, { "frequency", 3500.0 }, { "gain", -3.0 }, { "q", 3.0 } } },
            { "Warmth (low shelf)", { { "type", lowShelf }, { "frequency", 120.0 }, { "gain", 3.0 }, { "q", 0.71 } } },
            { "Air (high shelf)", { { "type", highShelf }, { "frequency", 12000.0 }, { "gain", 3.0 }, { "q", 0.71 } } },
            { "Hiss cut (10 kHz low-pass)", { { "type", lowPass }, { "frequency", 10000.0 }, { "gain", 0.0 }, { "q", 0.71 } } } };
        t["spectrablade:parametric-eq"] = {
            { "Vocal clarity", parametric ({{ { true, highPass, 80, 0, 0.71 }, { true, bell, 250, -3, 1.2 }, { true, bell, 3000, 2.5, 1.0 },
                                             { true, bell, 7000, -1.5, 3.0 }, { true, highShelf, 12000, 2, 0.71 } }}) },
            { "Dialogue cleanup", parametric ({{ { true, highPass, 100, 0, 0.71 }, { true, bell, 200, -2, 1.0 }, { true, bell, 2500, 2, 1.0 },
                                                { false, bell, 5000, 0, 1.0 }, { true, lowPass, 15000, 0, 0.71 } }}) },
            { "Kick drum", parametric ({{ { true, highPass, 30, 0, 0.71 }, { true, lowShelf, 60, 3, 0.71 }, { true, bell, 350, -4, 1.5 },
                                         { true, bell, 3500, 3, 1.5 }, { false, highShelf, 10000, 0, 0.71 } }}) },
            { "Acoustic guitar", parametric ({{ { true, highPass, 70, 0, 0.71 }, { true, bell, 200, -2, 1.0 }, { true, bell, 1000, -1, 1.0 },
                                               { true, bell, 5000, 2, 1.0 }, { true, highShelf, 10000, 2, 0.71 } }}) },
            { "Master, brighter", parametric ({{ { true, highPass, 20, 0, 0.71 }, { true, lowShelf, 100, -1, 0.71 }, { false, bell, 1000, 0, 1.0 },
                                                { false, bell, 4000, 0, 1.0 }, { true, highShelf, 8000, 1.5, 0.71 } }}) },
            { "Master, warmer", parametric ({{ { true, highPass, 20, 0, 0.71 }, { true, lowShelf, 120, 1.5, 0.71 }, { false, bell, 1000, 0, 1.0 },
                                               { true, bell, 3500, -1, 1.0 }, { true, highShelf, 10000, -1, 0.71 } }}) },
            { "Telephone", parametric ({{ { true, highPass, 400, 0, 0.71 }, { true, bell, 1500, 4, 1.0 }, { false, bell, 2500, 0, 1.0 },
                                         { false, bell, 3000, 0, 1.0 }, { true, lowPass, 3400, 0, 0.71 } }}) } };
        t["spectrablade:graphic-eq-10"] = graphic (10);
        t["spectrablade:graphic-eq-31"] = graphic (31);

        // Dynamics
        t["spectrablade:compressor"] = {
            { "Gentle glue", { { "threshold", -18.0 }, { "ratio", 2.0 }, { "knee", 6.0 }, { "attack", 30.0 }, { "release", 200.0 }, { "makeup", 2.0 } } },
            { "Vocal", { { "threshold", -20.0 }, { "ratio", 3.0 }, { "knee", 6.0 }, { "attack", 10.0 }, { "release", 120.0 }, { "makeup", 4.0 } } },
            { "Dialogue levelling", { { "threshold", -24.0 }, { "ratio", 4.0 }, { "knee", 6.0 }, { "attack", 5.0 }, { "release", 150.0 }, { "makeup", 6.0 } } },
            { "Drum punch", { { "threshold", -16.0 }, { "ratio", 4.0 }, { "knee", 3.0 }, { "attack", 30.0 }, { "release", 80.0 }, { "makeup", 3.0 } } },
            { "Bass control", { { "threshold", -20.0 }, { "ratio", 4.0 }, { "knee", 6.0 }, { "attack", 15.0 }, { "release", 150.0 }, { "makeup", 3.0 } } },
            { "Master bus", { { "threshold", -12.0 }, { "ratio", 1.5 }, { "knee", 8.0 }, { "attack", 30.0 }, { "release", 300.0 }, { "makeup", 1.0 } } },
            { "Heavy squash", { { "threshold", -30.0 }, { "ratio", 10.0 }, { "knee", 0.0 }, { "attack", 1.0 }, { "release", 60.0 }, { "makeup", 10.0 } } } };
        t["spectrablade:limiter"] = {
            { "Safety (-1 dBTP)", { { "gain", 0.0 }, { "ceiling", -1.0 }, { "release", 100.0 }, { "lookahead", 5.0 }, { "peaks", 1.0 } } },
            { "Broadcast (-2 dBTP)", { { "gain", 0.0 }, { "ceiling", -2.0 }, { "release", 100.0 }, { "lookahead", 5.0 }, { "peaks", 1.0 } } },
            { "Master, louder (+6 dB)", { { "gain", 6.0 }, { "ceiling", -1.0 }, { "release", 50.0 }, { "lookahead", 5.0 }, { "peaks", 1.0 } } },
            { "Transparent (sample peaks, -0.3 dBFS)", { { "gain", 0.0 }, { "ceiling", -0.3 }, { "release", 200.0 }, { "lookahead", 5.0 }, { "peaks", 0.0 } } } };
        t["spectrablade:de-esser"] = {
            { "Female vocal", { { "frequency", 7000.0 }, { "threshold", -30.0 }, { "reduction", 6.0 }, { "mode", 0.0 }, { "listen", 0.0 } } },
            { "Male vocal", { { "frequency", 5000.0 }, { "threshold", -30.0 }, { "reduction", 6.0 }, { "mode", 0.0 }, { "listen", 0.0 } } },
            { "Gentle", { { "frequency", 6500.0 }, { "threshold", -24.0 }, { "reduction", 3.0 }, { "mode", 0.0 }, { "listen", 0.0 } } },
            { "Strong", { { "frequency", 6000.0 }, { "threshold", -36.0 }, { "reduction", 10.0 }, { "mode", 0.0 }, { "listen", 0.0 } } },
            { "Podcast (whole sound)", { { "frequency", 6000.0 }, { "threshold", -30.0 }, { "reduction", 6.0 }, { "mode", 1.0 }, { "listen", 0.0 } } } };
        t["spectrablade:dynamic-eq"] = {
            { "Tame harsh vocals", dynamicEq ({ { 1, 0, 0, 3200, 1.5, -6, -30, 4, 5, 120 } }) },
            { "Control a boomy low end", dynamicEq ({ { 1, 1, 0, 120, 1, -6, -24, 4, 20, 200 } }) },
            { "Hold down sibilance and mud", dynamicEq ({ { 1, 0, 0, 250, 1.2, -4, -26, 3, 15, 180 }, { 1, 0, 0, 7000, 2, -6, -34, 4, 2, 80 } }) },
            { "Lift quiet presence", dynamicEq ({ { 1, 0, 1, 2500, 1, 4, -36, 3, 20, 250 } }) },
            { "Air that never gets harsh", dynamicEq ({ { 1, 2, 0, 9000, 1, -4, -30, 3, 5, 120 }, { 1, 0, 1, 12000, 0.8, 3, -45, 2, 20, 300 } }) } };
        t["spectrablade:multiband-compressor"] = {
            { "Master glue", multiband (200, 3000, {{ { -20, 2, 30, 200, 1 }, { -18, 1.8, 20, 150, 1 }, { -22, 2, 10, 100, 1 } }}, 0) },
            { "Tame a boomy low end", multiband (150, 3000, {{ { -28, 4, 20, 150, 0 }, { -10, 1, 20, 150, 0 }, { -10, 1, 10, 100, 0 } }}, 0) },
            { "Smooth a harsh top", multiband (200, 4000, {{ { -10, 1, 30, 200, 0 }, { -12, 1.5, 20, 150, 0 }, { -26, 3, 5, 80, 0 } }}, 0) },
            { "Vocal smoother", multiband (250, 4000, {{ { -12, 1.5, 30, 200, 0 }, { -22, 3, 10, 120, 2 }, { -26, 3, 5, 80, 0 } }}, 0) },
            { "Broadcast density", multiband (150, 2500, {{ { -24, 3, 20, 150, 2 }, { -24, 3, 15, 120, 2 }, { -24, 3, 10, 100, 2 } }}, 0) } };

        // Modulation and space
        t["spectrablade:chorus"] = {
            { "Subtle", { { "rate", 0.5 }, { "delay", 15.0 }, { "depth", 2.0 }, { "feedback", 0.0 }, { "mix", 30.0 }, { "spread", 90.0 } } },
            { "Lush", { { "rate", 0.8 }, { "delay", 20.0 }, { "depth", 5.0 }, { "feedback", 0.1 }, { "mix", 50.0 }, { "spread", 120.0 } } },
            { "Wide doubler", { { "rate", 0.3 }, { "delay", 25.0 }, { "depth", 3.0 }, { "feedback", 0.0 }, { "mix", 40.0 }, { "spread", 180.0 } } },
            { "Vibrato", { { "rate", 5.0 }, { "delay", 5.0 }, { "depth", 2.0 }, { "feedback", 0.0 }, { "mix", 100.0 }, { "spread", 0.0 } } } };
        t["spectrablade:flanger"] = {
            { "Jet", { { "rate", 0.2 }, { "delay", 2.0 }, { "depth", 2.0 }, { "feedback", 0.7 }, { "mix", 50.0 }, { "spread", 90.0 } } },
            { "Subtle", { { "rate", 0.3 }, { "delay", 1.0 }, { "depth", 1.0 }, { "feedback", 0.3 }, { "mix", 30.0 }, { "spread", 60.0 } } },
            { "Metallic", { { "rate", 0.5 }, { "delay", 1.0 }, { "depth", 0.5 }, { "feedback", 0.9 }, { "mix", 50.0 }, { "spread", 0.0 } } },
            { "Hollow (negative feedback)", { { "rate", 0.1 }, { "delay", 3.0 }, { "depth", 3.0 }, { "feedback", -0.6 }, { "mix", 50.0 }, { "spread", 90.0 } } } };
        t["spectrablade:phaser"] = {   // stages by index: 0 is 2 stages, 1 is 4, ... 5 is 12
            { "Slow sweep", { { "rate", 0.2 }, { "low", 200.0 }, { "high", 4000.0 }, { "stages", 1.0 }, { "feedback", 0.3 }, { "mix", 50.0 }, { "spread", 90.0 } } },
            { "Classic four-stage", { { "rate", 0.5 }, { "low", 300.0 }, { "high", 3000.0 }, { "stages", 1.0 }, { "feedback", 0.5 }, { "mix", 50.0 }, { "spread", 0.0 } } },
            { "Deep eight-stage", { { "rate", 0.3 }, { "low", 100.0 }, { "high", 8000.0 }, { "stages", 3.0 }, { "feedback", 0.7 }, { "mix", 50.0 }, { "spread", 120.0 } } },
            { "Fast wobble", { { "rate", 3.0 }, { "low", 400.0 }, { "high", 2500.0 }, { "stages", 2.0 }, { "feedback", 0.4 }, { "mix", 50.0 }, { "spread", 0.0 } } } };
        t["spectrablade:tremolo"] = {
            { "Gentle", { { "rate", 4.0 }, { "depth", 30.0 } } }, { "Amp tremolo", { { "rate", 6.0 }, { "depth", 60.0 } } },
            { "Helicopter", { { "rate", 12.0 }, { "depth", 100.0 } } }, { "Slow pulse", { { "rate", 1.0 }, { "depth", 50.0 } } } };
        t["spectrablade:echo"] = {
            { "Slapback", { { "delay", 120.0 }, { "feedback", 15.0 } } },
            { "Eighth note at 120 BPM", { { "delay", 250.0 }, { "feedback", 40.0 } } },
            { "Quarter note at 120 BPM", { { "delay", 500.0 }, { "feedback", 35.0 } } },
            { "Canyon", { { "delay", 900.0 }, { "feedback", 55.0 } } } };
        t["spectrablade:reverb"] = {
            { "Ambience", { { "preDelay", 0.0 }, { "decay", 0.3 }, { "size", 50.0 }, { "damping", 60.0 }, { "width", 100.0 }, { "mix", 15.0 } } },
            { "Small room", { { "preDelay", 5.0 }, { "decay", 0.5 }, { "size", 60.0 }, { "damping", 50.0 }, { "width", 80.0 }, { "mix", 20.0 } } },
            { "Drum room", { { "preDelay", 10.0 }, { "decay", 0.8 }, { "size", 70.0 }, { "damping", 40.0 }, { "width", 100.0 }, { "mix", 25.0 } } },
            { "Bright vocal", { { "preDelay", 20.0 }, { "decay", 1.5 }, { "size", 80.0 }, { "damping", 30.0 }, { "width", 100.0 }, { "mix", 20.0 } } },
            { "Concert hall", { { "preDelay", 30.0 }, { "decay", 3.0 }, { "size", 130.0 }, { "damping", 40.0 }, { "width", 100.0 }, { "mix", 30.0 } } },
            { "Cathedral", { { "preDelay", 50.0 }, { "decay", 8.0 }, { "size", 150.0 }, { "damping", 50.0 }, { "width", 100.0 }, { "mix", 35.0 } } } };
        t["spectrablade:convolution-reverb"] = {
            { "Subtle (15%)", { { "mix", 15.0 } } }, { "Medium (30%)", { { "mix", 30.0 } } },
            { "Wet (60%)", { { "mix", 60.0 } } }, { "All wet", { { "mix", 100.0 } } } };

        // Generators
        t["spectrablade:generate-tone"] = {
            { "Reference, 1 kHz at -20 dBFS peak", { { "wave", 0.0 }, { "frequency", 1000.0 }, { "end", 1000.0 }, { "level", -20.0 }, { "length", 30.0 }, { "fade", 5.0 } } },
            { "Bleep (1 kHz)", { { "wave", 0.0 }, { "frequency", 1000.0 }, { "end", 1000.0 }, { "level", -12.0 }, { "length", 1.0 }, { "fade", 5.0 } } },
            { "Tuning A (440 Hz)", { { "wave", 0.0 }, { "frequency", 440.0 }, { "end", 440.0 }, { "level", -12.0 }, { "length", 5.0 }, { "fade", 10.0 } } },
            { "Sweep, 20 Hz to 20 kHz", { { "wave", 0.0 }, { "frequency", 20.0 }, { "end", 20000.0 }, { "level", -12.0 }, { "length", 10.0 }, { "fade", 10.0 } } } };
        t["spectrablade:generate-noise"] = {
            { "Pink, -20 dBFS RMS", { { "colour", 1.0 }, { "level", -20.0 }, { "length", 30.0 }, { "fade", 10.0 } } },
            { "White, -20 dBFS RMS", { { "colour", 0.0 }, { "level", -20.0 }, { "length", 10.0 }, { "fade", 10.0 } } },
            { "Brown, low rumble", { { "colour", 2.0 }, { "level", -30.0 }, { "length", 10.0 }, { "fade", 50.0 } } } };
        t["spectrablade:dtmf-tones"] = {
            { "Touch-tone dialling", { { "kind", 0.0 }, { "tone", 100.0 }, { "gap", 100.0 }, { "level", -6.0 } } },
            { "Fast dialling", { { "kind", 0.0 }, { "tone", 50.0 }, { "gap", 50.0 }, { "level", -6.0 } } },
            { "MF (R1)", { { "kind", 1.0 }, { "tone", 68.0 }, { "gap", 68.0 }, { "level", -6.0 } } } };

        return t;
    }

    const Table& table()
    {
        static const Table t = makeTable();
        return t;
    }
}

const std::vector<Factory>& factory (const juce::String& effectKey)
{
    static const std::vector<Factory> none;
    const auto found = table().find (effectKey);
    return found != table().end() ? found->second : none;
}

juce::StringArray factoryKeys()
{
    juce::StringArray keys;

    for (auto& [key, presets] : table())
        keys.add (key);

    return keys;
}
}
