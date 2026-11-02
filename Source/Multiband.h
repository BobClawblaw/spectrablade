#pragma once

#include "Dynamics.h"

/** Band-split dynamics: a three-band compressor. It splits the sound with Linkwitz-Riley
    crossovers (two second-order Butterworth filters in a row, 24 dB an octave), whose
    bands add back up to the sound with every frequency at its level, only its phase turned. */
namespace Multiband
{
    /** One Linkwitz-Riley split of one channel at one frequency, run a sample at a time. */
    class Crossover
    {
    public:
        void set (double frequency, double sampleRate);
        void reset() noexcept;

        /** Splits a sample into its part below the frequency and its part above. */
        void split (double in, double& low, double& high) noexcept;

        /** The two parts added back up: the sound with the split's phase turn, its level kept, for a
            band that must line up with ones split here. */
        double allPass (double in) noexcept;

    private:
        struct Biquad
        {
            double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
            double run (double x) noexcept;
        };

        std::array<Biquad, 2> lows, highs;
        std::array<Biquad, 2> allLows, allHighs;
    };

    //==============================================================================
    struct Compressor
    {
        double lowCrossover = 200.0;     // Hz, between the low and the middle band
        double highCrossover = 2000.0;   // Hz, between the middle and the high band
        std::array<Dynamics::Compressor, 3> bands {};   // low, middle, high
        double outputDb = 0.0;
    };

    /** Whether the settings can be used at this rate: crossovers from 20 Hz to 20 kHz, below half
        the rate, the high one at least an octave above the low; each band's compressor valid; an
        output within 24 dB. */
    bool isValid (const Compressor&, double sampleRate);

    /** The settings with the crossovers moved into what the rate allows: the high one at most 0.45 of
        the rate (and 20 kHz), the low one at most an octave under that, the high one at least an
        octave above the low. Both the live effect and the panel's dialog use it, so a slider dragged
        too close plays and applies the same. */
    Compressor spaced (Compressor, double sampleRate);

    /** The compressor a frame at a time across the channels: compress runs through it, and so does
        the live effect, so they give the same samples. Each band's gain follows that band's loudest
        channel, so a stereo image stays put within each band. */
    class CompressorCore
    {
    public:
        void prepare (int numChannels, double sampleRate);
        void reset() noexcept;
        void set (const Compressor&);   // valid at the prepared rate; the gains carry on

        void process (float* const* channels, int numChannels, int numFrames) noexcept;

        /** Each band's gain reduction at the last frame, in dB (0 or less), for a meter. */
        std::array<double, 3> getReductionDb() const noexcept   { return reduction; }

    private:
        double rate = 48000.0;
        Compressor settings;
        std::vector<Crossover> first, second, lowAlign;   // a channel each
        std::array<Dynamics::CompressorCore, 3> cores;
        std::array<double, 3> reduction {};
        std::vector<std::array<double, 3>> parts;
        double output = 1.0;
    };

    /** Compresses [start, end) in three bands. Nullptr for settings that aren't valid or an empty range. */
    AudioEdits::Snapshot compress (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Compressor&);

    /** The settings on one panel: the crossovers and the output along the top, then a column a band
        (its frequencies named over it) with a row each for threshold, ratio, attack, release and
        makeup. Each slider covers the setting's whole range; onChange is called as one moves. */
    class Panel final : public juce::Component
    {
    public:
        Panel();

        Compressor getSettings() const;
        void setSettings (const Compressor&);   // without onChange

        /** Shows each band's gain reduction (0 or less, in dB) under its name, for a live effect. */
        void setReductionDb (const std::array<double, 3>&);

        std::function<void()> onChange;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        juce::Slider* make (juce::NormalisableRange<double>, double interval, const juce::String& suffix);

        juce::OwnedArray<juce::Slider> sliders;   // low and high crossover, output, then each band's five
        juce::OwnedArray<juce::Label> labels;
        juce::Slider* lowCrossover = nullptr;
        juce::Slider* highCrossover = nullptr;
        juce::Slider* output = nullptr;
        std::array<std::array<juce::Slider*, 5>, 3> bands {};
        std::optional<std::array<double, 3>> reductionDb;   // shown once set
    };
}
