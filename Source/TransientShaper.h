#pragma once

#include "Multiband.h"

/** A transient shaper: the attacks (the first moments of a hit) and the sustain (what rings on
    after) turned up or down on their own, in each of three bands. The sound is split into lows,
    middle and highs (the multiband split, 24 dB an octave, so the bands add back flat; in minimum
    or linear phase: Multiband::Split). In each
    band three envelopes follow the loudest channel: a fast one (0.5 ms up, 20 ms down), a slow one
    (15 ms up, 50 ms down) and a hold of the recent peak (1 ms up, 150 ms down); the low band's come
    down slower (80, 150 and 300 ms), so the lowest notes' own cycles don't read as hits. Where the
    fast one stands over the slow by more than 1.5 dB, the sound is attacking, and the band's attack
    gain applies in proportion (fully at 12 dB over); where the hold stands over the fast by more than
    1.5 dB, the sound has fallen from its recent peak and is sustaining, and the sustain gain applies
    likewise, more the further it has fallen. Nothing acts under -60 dBFS. The gain follows the
    envelopes sample by sample, the same on every channel, so a stereo image stays put. No latency
    in minimum phase; the split's in linear phase (Core::getLatency), which the live effect reports
    and the offline apply takes off. */
namespace TransientShaper
{
    struct Band
    {
        double attackDb = 0.0;    // -12 to +12 dB on the attacks
        double sustainDb = 0.0;   // -12 to +12 dB on the sustain
    };

    struct Settings
    {
        double lowCrossover = 200.0;
        double highCrossover = 3000.0;
        std::array<Band, 3> bands {};   // low, middle, high
        double outputDb = 0.0;          // -24 to +24
        bool linearPhase = false;       // the split in linear phase (Multiband::Split), its latency added
    };

    /** Whether the settings can be used at this rate: the crossovers as Multiband::Width's, each
        gain within 12 dB, the output within 24 dB. */
    bool isValid (const Settings&, double sampleRate);

    /** The settings with the crossovers moved into what the rate allows (as Multiband::spaced). */
    Settings spaced (Settings, double sampleRate);

    class Core
    {
    public:
        void prepare (int numChannels, double sampleRate);
        void reset() noexcept;
        void set (const Settings&);

        /** In place; each frame comes out getLatency() frames late. */
        void process (float* const* channels, int numChannels, int numFrames) noexcept;

        /** Each band's gain at the last frame, in dB, for a meter. */
        const std::array<double, 3>& getGainDb() const noexcept   { return gainDb; }

        /** Multiband::Split::linearLatency in linear phase, else 0. */
        int getLatency() const noexcept                           { return split.getLatency(); }

    private:
        double rate = 48000.0;
        Settings settings;
        double fastUp = 0.0, slowUp = 0.0, holdUp = 0.0, output = 1.0;
        std::array<double, 3> fastDown {}, slowDown {}, holdDown {}, envFast {}, envSlow {}, envHold {}, gainDb {};
        Multiband::Split split;
    };

    /** Shapes [start, end) (as Multiband::runPrimed: the result lands where the audio was, the
        latency taken off, primed on the audio before). Nullptr for settings that aren't valid or an
        empty range. */
    AudioEdits::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&);
}
