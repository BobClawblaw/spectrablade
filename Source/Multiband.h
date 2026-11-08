#pragma once

#include "Dynamics.h"
#include "Equalizer.h"

/** Band-split effects: a three-band compressor, a de-esser and a three-band stereo width. All split the sound with
    Linkwitz-Riley crossovers (two second-order Butterworth filters in a row, 24 dB an octave), whose
    bands add back up to the sound with every frequency at its level, only its phase turned; or, in
    linear phase, with symmetric impulses of the same band levels, whose bands add back up to the
    sound exactly as it was, late by the impulses' delay (Split). */
namespace Multiband
{
    /** One Linkwitz-Riley split of one channel at one frequency, run a sample at a time. The parts
        added back give the sound with its level kept at every frequency but its phase turned, and
        a phase turn moves peaks: on a sound already limited or clipped to a ceiling, the whole can
        peak well over it even with every band left alone (measured: 5.9 to 6.5 dB over on a
        record transfer limited to 0 dBTP, 8.3 dB over on a clipped handheld recording). Split in
        linear phase holds them; with this split, a ceiling is set by a limiter after the effect. */
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

        /** The level (1 for unity) of the part below the frequency at hz, as set: 1 at 0 Hz, a half
            at the frequency, and with the part above's level it adds up to 1 everywhere. */
        double lowLevelAt (double hz) const noexcept;

    private:
        struct Biquad
        {
            double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
            double run (double x) noexcept;
        };

        std::array<Biquad, 2> lows, highs;
        std::array<Biquad, 2> allLows, allHighs;
        double rateSet = 48000.0;
    };

    /** The band split of every channel, either way. Minimum phase: the Crossovers (the low band
        turned as the high split turns the others, so the three add up flat), no delay. Linear
        phase: the lows from a symmetric low-pass impulse at the low crossover and the highs from a
        high-pass one at the high crossover (each with the Crossovers' own level curve, by the
        linear-phase EQ's engine), the middle what is left of the sound delayed to match, so the
        three add back to the sound exactly as it was (to the float's rounding), its peaks held,
        linearLatency frames late, which the live effects report and the offline ones take off.
        With no high crossover the split is two-way (the de-esser's): below and above the one
        frequency, the third band silent. Up to chunk frames at a time: run, then read the bands. */
    class Split
    {
    public:
        static constexpr int linearLatency = Equalizer::LinearPhase::latency;
        static constexpr int chunk = 512;

        void prepare (int numChannels, double sampleRate);
        void reset() noexcept;

        /** The crossovers (valid at the prepared rate; no high one for a two-way split) and the
            way; the impulses are only remade when these change. */
        void set (double lowCrossover, std::optional<double> highCrossover, bool linearPhase);

        int getLatency() const noexcept                      { return linear ? linearLatency : 0; }
        int getNumChannels() const noexcept                  { return (int) parts.size(); }

        /** Splits numFrames (up to chunk) of the first channels into the bands. */
        void run (const float* const* in, int numChannels, int numFrames) noexcept;

        /** Channel ch's band b (0 the lows, 1 the middle, 2 the highs) as run left it, numFrames long. */
        const double* band (int ch, int b) const noexcept    { return parts[(size_t) ch][(size_t) b].data(); }

        /** Channel ch as it came in, getLatency() frames late, bit for bit (for a channel that
            passes while the others are worked on, so it stays in step with them). */
        const float* same (int ch) const noexcept            { return sames[(size_t) ch]; }

    private:
        struct Channel
        {
            Crossover first, second, lowAlign;
            std::vector<float> delay;   // the sound, linearLatency frames of it, for the middle
        };

        double rate = 48000.0, low = 0.0;
        std::optional<double> high;
        bool linear = false, built = false;
        std::vector<Channel> channels;
        std::vector<std::array<std::vector<double>, 3>> parts;   // [channel][band][frame]
        std::vector<const float*> sames;
        Equalizer::LinearPhase lows, highs;                       // the impulses, in linear phase
        juce::AudioBuffer<float> lowWork, highWork, sameWork;
        int delayPos = 0;
    };

    /** Runs a core (prepared at the rate and set) over [start, end) after priming it on the audio
        before, which is kept as it was: up to a second of it, or the linear-phase impulses' length
        when the core has latency, so the range starts as it would in a longer pass. The core's
        latency is taken off, so the result lands where the audio was; nothing rings past the
        range's end. Nullptr for an empty range. */
    template <typename Core>
    AudioEdits::Snapshot runPrimed (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, Core& core)
    {
        start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
        end = juce::jlimit (start, s.getNumFrames(), end);

        if (start == end || end - start > std::numeric_limits<int>::max() / 4)
            return nullptr;

        const auto latency = core.getLatency();
        const auto reach = latency > 0 ? juce::jmax ((juce::int64) std::ceil (sampleRate), (juce::int64) Equalizer::LinearPhase::taps)
                                       : (juce::int64) std::ceil (sampleRate);
        const auto prime = (int) juce::jmin (start, reach);

        return AudioEdits::processRange (s, start - prime, end, [&core, prime, latency] (juce::AudioBuffer<float>& b)
        {
            const auto channels = b.getNumChannels(), frames = b.getNumSamples();
            juce::AudioBuffer<float> work (channels, frames + latency);
            work.clear();

            for (int ch = 0; ch < channels; ++ch)
                work.copyFrom (ch, 0, b, ch, 0, frames);

            core.process (work.getArrayOfWritePointers(), channels, work.getNumSamples());

            for (int ch = 0; ch < channels; ++ch)
                b.copyFrom (ch, prime, work, ch, latency + prime, frames - prime);
        });
    }

    //==============================================================================
    struct Compressor
    {
        double lowCrossover = 200.0;     // Hz, between the low and the middle band
        double highCrossover = 2000.0;   // Hz, between the middle and the high band
        std::array<Dynamics::Compressor, 3> bands {};   // low, middle, high
        double outputDb = 0.0;
        bool linearPhase = false;        // the split in linear phase (Split), Split::linearLatency frames late
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

        /** In place; each frame comes out getLatency() frames late. */
        void process (float* const* channels, int numChannels, int numFrames) noexcept;

        /** Each band's gain reduction at the last frame, in dB (0 or less), for a meter. */
        std::array<double, 3> getReductionDb() const noexcept   { return reduction; }

        /** Split::linearLatency in linear phase, else 0. */
        int getLatency() const noexcept                         { return split.getLatency(); }

    private:
        double rate = 48000.0;
        Compressor settings;
        Split split;
        std::array<Dynamics::CompressorCore, 3> cores;
        std::array<double, 3> reduction {};
        double output = 1.0;
    };

    /** Compresses [start, end) in three bands (as runPrimed). Nullptr for settings that aren't valid or an empty range. */
    AudioEdits::Snapshot compress (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Compressor&);

    /** The settings on one panel: the crossovers, the output and the linear-phase switch along the top, then a column a band
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
        juce::ToggleButton linearPhase { "Linear phase" };
        std::array<std::array<juce::Slider*, 5>, 3> bands {};
        std::optional<std::array<double, 3>> reductionDb;   // shown once set
    };

    //==============================================================================
    struct DeEsser
    {
        double frequency = 6000.0;       // Hz: the sibilance band starts here
        double thresholdDb = -30.0;      // the band's level where it starts being turned down
        double maxReductionDb = 12.0;    // the most it is turned down
        bool wholeSound = false;         // turn the whole sound down, not just the band above the frequency
        bool linearPhase = false;        // the split in linear phase (Split), Split::linearLatency frames late
    };

    /** Whether the settings can be used at this rate: a frequency from 1 to 16 kHz below half the
        rate, a threshold from -60 to 0 dB, a reduction from 0 to 24 dB. */
    bool isValid (const DeEsser&, double sampleRate);

    /** The de-esser a frame at a time: the loudest channel's part above the frequency is the level;
        above the threshold, the band (or the whole sound) is turned down by the excess, up to the
        reduction, moving down in 1 ms and back up in 60 ms. */
    class DeEsserCore
    {
    public:
        static constexpr double attackMs = 1.0, releaseMs = 60.0;

        void prepare (int numChannels, double sampleRate);
        void reset() noexcept;
        void set (const DeEsser&);

        /** Puts out only what it takes away instead of the result (the two add up to the sound with
            nothing taken), to hear what the frequency and threshold catch. Off to start. */
        void setListening (bool) noexcept;

        /** In place; each frame comes out getLatency() frames late. */
        void process (float* const* channels, int numChannels, int numFrames) noexcept;

        double getReductionDb() const noexcept               { return gainDb; }
        int getLatency() const noexcept                      { return split.getLatency(); }

    private:
        double rate = 48000.0;
        DeEsser settings;
        Split split;
        double attack = 0.0, release = 0.0, gainDb = 0.0;
        bool listening = false;
    };

    /** De-esses [start, end) (as runPrimed). Nullptr for settings that aren't valid or an empty range. */
    AudioEdits::Snapshot deEss (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const DeEsser&);

    //==============================================================================
    /** The stereo width of each of three bands: 0 % makes the band mono (its side taken out), 100 %
        leaves it, up to 200 % doubles its side. The low band at 0 % is a mono low end below the low
        crossover. The mid (what the channels share) is never touched, so a mono sound, or more
        channels than two past the first pair, comes through unchanged. */
    struct Width
    {
        double lowCrossover = 150.0;     // Hz, between the low and the middle band
        double highCrossover = 2000.0;   // Hz, between the middle and the high band
        std::array<double, 3> percent { 100.0, 100.0, 100.0 };   // low, middle, high
        bool linearPhase = false;        // the split in linear phase (Split), Split::linearLatency frames late
    };

    /** Whether the settings can be used at this rate: crossovers from 20 Hz to 20 kHz, below 0.45
        of the rate, the high one at least an octave above the low; each width from 0 to 200 %. */
    bool isValid (const Width&, double sampleRate);

    /** The settings with the crossovers moved into what the rate allows, as spaced (Compressor) does. */
    Width spaced (Width, double sampleRate);

    /** The width a frame at a time: the first two channels split into the bands (the compressor's
        split, so a band at 100 % adds back flat), each band's side scaled, the bands added back up.
        Other channels pass. */
    class WidthCore
    {
    public:
        void prepare (int numChannels, double sampleRate);
        void reset() noexcept;
        void set (const Width&);   // valid at the prepared rate

        /** In place; each frame comes out getLatency() frames late. */
        void process (float* const* channels, int numChannels, int numFrames) noexcept;

        int getLatency() const noexcept   { return split.getLatency(); }

    private:
        double rate = 48000.0;
        Width settings;
        Split split;
        std::array<double, 3> factors { 1.0, 1.0, 1.0 };
        int channelsPrepared = 0;
    };

    /** Sets the bands' widths over [start, end) (as runPrimed). Nullptr for settings that aren't valid or an empty range. */
    AudioEdits::Snapshot widen (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Width&);
}
