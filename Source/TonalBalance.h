#pragma once

#include "Analysis.h"
#include "MatchEq.h"

/** The Tonal Balance panel: the long-term spectrum of what goes to the outputs against a target
    range for a style of material. The spectrum is MatchEq's profile (twelfth-octave points, 20 Hz
    to 20 kHz), averaged over the last few seconds of output and drawn relative to its own mean over
    100 Hz to 10 kHz, so only the shape counts, not the level. The target is a band (a lower and an
    upper bound at each point) for the chosen style, shaded behind the curve; where the curve leaves
    the band it is drawn in the warning colour, and the panel says how much of the range lies within.

    The styles are SpectraBlade's own reference shapes, meant as starting points rather than
    measurements of any catalogue: each is a slope against pink noise (which falls 3 dB an octave
    on this scale) with a tolerance, and roll-offs at the ends:
    Full mix: pink, 3 dB either way, rolling off below 40 Hz and above 12 kHz.
    Bass-heavy: 3 dB more below 100 Hz, 1 dB less above 5 kHz, 3 dB either way.
    Bright: 2 dB less below 100 Hz, 2 dB more above 4 kHz, 3 dB either way.
    Speech: 6 dB less below 100 Hz and above 8 kHz, 4 dB either way.
    Orchestral: 1.5 dB an octave steeper than pink, 4 dB either way. */
namespace TonalBalance
{
    enum class Style { fullMix, bassHeavy, bright, speech, orchestral };

    const juce::StringArray& styleNames();

    /** The target's lower and upper bounds at each twelfth-octave point, in dB relative to the mean
        over 100 Hz to 10 kHz (the centre curve has that mean at zero). */
    struct Target
    {
        std::array<double, MatchEq::numPoints> low {}, high {};
    };

    Target targetFor (Style);

    /** A profile relative to its own mean over 100 Hz to 10 kHz (points under -100 dBFS left where
        they are; nothing changes if none count). */
    std::array<double, MatchEq::numPoints> relative (const std::array<double, MatchEq::numPoints>& levelDb);

    /** How much of a relative profile lies within the target, from 0 to 1, over the points from
        30 Hz to 16 kHz. */
    double shareWithin (const std::array<double, MatchEq::numPoints>& relativeDb, const Target&);

    class Panel final : public juce::Component,
                        private juce::Timer
    {
    public:
        /** Fills the buffer (2 channels, Analysis::fftSize frames) with the newest output; returns
            the sample rate, 0 if none. */
        std::function<double (juce::AudioBuffer<float>&)> readOutput;

        Panel();
        ~Panel() override;

        /** Takes in a window's spectrum (dB by bin, as Analysis::spectrumDb), as each tick does:
            its power is averaged into the profile, the newest window weighing a tenth (about three
            seconds of memory at the tick rate). */
        void take (const std::vector<float>& spectrumDb, double sampleRate);

        /** The averaged profile, relative to its mean, and the share within the target. */
        const std::array<double, MatchEq::numPoints>& getRelative() const noexcept   { return shown; }
        double getShareWithin() const noexcept                                     { return share; }

        void setStyle (Style);
        Style getStyle() const noexcept                                            { return style; }

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void timerCallback() override;
        void refresh();
        juce::Rectangle<float> plotArea() const;

        juce::ComboBox styles;
        Style style = Style::fullMix;
        Target target;
        std::array<double, MatchEq::numPoints> power {}, shown {};
        bool anything = false;
        double share = 0.0;
        juce::AudioBuffer<float> recent { 2, Analysis::fftSize };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
    };
}
