#pragma once

#include "Multiband.h"

/** De-plosive: the thumps a p or a b puts into a microphone (a burst of air, mostly below 150 Hz,
    a few tens of milliseconds long) found and lowered, the rest left alone. The sound is split at
    the corner (the multiband split, 24 dB an octave). The low band's envelope (2 ms up, 40 ms down)
    is compared with its own typical level, the envelope's slow average over the last second: where
    it stands over that by more than the threshold while the band above does not rise with it (a
    note or a word that is simply louder raises both), a plosive is on, and the low band is lowered
    by the reduction for as long as it lasts (at most 250 ms: a thump is short, and what stays up
    longer is the sound itself), eased in and out over 5 ms. The typical levels follow the envelopes
    slowly while a plosive is on (over 5 s rather than 1), so a sound that starts loud settles as
    typical rather than counting as a plosive for good. Nothing happens while the low band is
    steady, however loud. The gain is found 10 ms ahead of the audio it is applied to, so it is down
    when the thump's first cycle arrives; the core puts the audio out that much late, and the offline
    apply takes it off. */
namespace Plosive
{
    struct Settings
    {
        double cornerHz = 150.0;       // 60 to 400: below this is what a plosive fills
        double thresholdDb = 12.0;     // 3 to 30: how far the low band must stand over its typical level
        double reductionDb = 18.0;     // 0 to 40: how far the low band is lowered during one
    };

    bool isValid (const Settings&, double sampleRate);

    class Core
    {
    public:
        void prepare (int numChannels, double sampleRate);
        void reset() noexcept;
        void set (const Settings&);

        /** In place; each frame comes out getLatency() frames late. */
        void process (float* const* channels, int numChannels, int numFrames) noexcept;

        int getLatency() const noexcept      { return ahead; }

        /** The low band's gain at the last frame, in dB (0 or less). */
        double getGainDb() const noexcept    { return 20.0 * std::log10 (juce::jmax (1.0e-9, gain)); }

    private:
        struct Channel
        {
            Multiband::Crossover split;
            std::vector<double> lowDelay, highDelay;   // the bands, ahead frames each
        };

        double rate = 48000.0;
        Settings settings;
        std::vector<Channel> channels;
        std::vector<double> lows, highs;
        double envLow = 0.0, envHigh = 0.0, typicalLow = 1.0e-6, typicalHigh = 1.0e-6, gain = 1.0;
        double fastUp = 0.0, fastDown = 0.0, slow = 0.0, slower = 0.0, ramp = 0.0, floorGain = 1.0, threshold = 1.0;
        int onFor = 0, longest = 1;   // frames the plosive has been on, and the most it may
        int ahead = 1, delayPos = 0;
    };

    /** Lowers the plosives in [start, end), primed on up to a second of the audio before (kept as
        it was). Nullptr for settings that aren't valid or an empty range. */
    AudioEdits::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&);
}
