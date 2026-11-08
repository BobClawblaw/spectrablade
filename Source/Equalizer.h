#pragma once

#include "AudioEdits.h"
#include "Convolver.h"

/** Equalizer bands: second-order filters made from their analogue prototypes by the bilinear
    transform, with the frequency prewarped so each band's frequency lands where it is asked. */
namespace Equalizer
{
    enum class BandType
    {
        peak,       // a bell: gainDb at the frequency, q sets the width
        lowShelf,   // gainDb below the frequency, half of it at the frequency
        highShelf,  // gainDb above the frequency, half of it at the frequency
        highPass,   // -3 dB at the frequency with q = 0.7071, 12 dB an octave below it
        lowPass     // -3 dB at the frequency with q = 0.7071, 12 dB an octave above it
    };

    /** Where a band works: on every channel, on the mid (what left and right share) or the side
        (what differs between them) of a stereo pair, or on the left or the right alone. On a mono
        sound, mid is the sound and side nothing; on more than two channels, mid and side are made
        from the first two and the rest are left as they are. */
    enum class Channel { all, mid, side, left, right };

    /** The channels' names in menu order: Stereo, Mid, Side, Left, Right. */
    const juce::StringArray& channelNames();

    struct Band
    {
        BandType type = BandType::peak;
        double frequency = 1000.0;
        double gainDb = 0.0;    // unused by the passes
        double q = 0.7071067811865476;
        Channel channel = Channel::all;
    };

    bool operator== (const Band&, const Band&);
    inline bool operator!= (const Band& a, const Band& b)    { return ! (a == b); }

    constexpr double maxGainDb = 48.0, minQ = 0.05, maxQ = 50.0;

    /** Whether the band can be made at this rate: a frequency above 0 and below half the rate, a q
        from minQ to maxQ and a gain within maxGainDb either way. */
    bool isValid (const Band&, double sampleRate);

    /** Whether the band changes nothing: a bell or shelf at 0 dB (a pass always changes something). */
    bool isFlat (const Band&);

    /** One band on one channel, run a block at a time: its state carries from block to block, so
        any split of the audio into blocks gives the same samples as one pass. */
    class Filter
    {
    public:
        /** New coefficients for the band at this rate; the state is kept, so a change mid-stream
            doesn't click more than the change itself. The band must be valid. */
        void setBand (const Band&, double sampleRate);
        void reset() noexcept                       { z1 = z2 = 0.0; }
        void process (float* samples, int numSamples) noexcept;
        double tick (double sample) noexcept;

    private:
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
    };

    /** One band on the channels it works on (Band::channel), a block at a time, its state carried
        from block to block as Filter's is: the offline apply and the live EQ both run their bands
        through stages, so they give the same samples. */
    class Stage
    {
    public:
        /** The band at this rate for this many channels (valid). The state is kept when only the
            coefficients change; a change of channel, or of the channel count, starts afresh. */
        void set (const Band&, double sampleRate, int numChannels);
        void reset() noexcept;
        void process (float* const* channels, int numChannels, int numFrames) noexcept;

    private:
        Band band;
        int channelsSet = 0;
        std::vector<Filter> filters;   // one a channel for all, left and right; the first alone for mid and side
    };

    /** The band's gain at a frequency, in dB; 0 for a band that isn't valid. */
    double responseDb (const Band&, double sampleRate, double frequency);

    /** The bands in linear phase: one symmetric impulse (taps frames long, its centre (taps - 1) / 2
        frames in) whose magnitude is the bands' response together, so every frequency comes out at
        the bands' level with no phase turned, (taps - 1) / 2 frames late plus the convolver's block.
        The impulse is the inverse transform of the response sampled on a fine grid, under a Hann
        window, so a feature narrower than about two bins of the window (some 6 Hz at 48 kHz) is
        drawn more broadly than the bands ask. Bands on the mid, the side, the left or the right are
        composed in order into a matrix of impulses (left from left and from right, right likewise);
        a channel no band touches is only delayed, bit for bit. The live EQ and the offline apply run
        the same engine, so they give the same samples. */
    class LinearPhase
    {
    public:
        static constexpr int taps = 16383;
        static constexpr int latency = (taps - 1) / 2 + Convolver::blockSize;

        /** The bands (valid at this rate, in order) for this many channels; starts from silence. */
        void prepare (const std::vector<Band>&, double sampleRate, int numChannels);

        /** Any curve instead: its gain in dB at a frequency (in Hz), on every channel alike. */
        void prepare (const std::function<double (double)>& gainDbAt, double sampleRate, int numChannels);
        void reset();

        /** In place: each frame comes out latency frames late. */
        void process (float* const* channels, int numChannels, int numFrames) noexcept;

        /** The impulses: for two channels and more, [0] left from left, [1] right from right, [2] left
            from right, [3] right from left, [4] the others' (the bands on every channel); a mono
            sound has [0] alone. Empty for a channel no band touches. */
        const std::vector<std::vector<float>>& getImpulses() const noexcept   { return impulses; }

    private:
        void build (const std::vector<double>& ll, const std::vector<double>& lr, const std::vector<double>& rl, const std::vector<double>& rr,
                    const std::vector<double>& others, bool leftTouched, bool rightTouched, bool crossed, bool othersTouched);

        std::vector<std::vector<float>> impulses;
        std::unique_ptr<Convolver::Core> straight, across;   // the channels from themselves, and from each other
        std::vector<bool> plain;                             // a channel only delayed
        std::vector<std::vector<float>> delays;
        std::vector<float*> pointers;
        int delayPos = 0, channelsPrepared = 0;
        juce::AudioBuffer<float> swapped, junk;
    };

    /** Runs the bands in linear phase over [start, end): the result lands where the audio was (the
        engine's delay taken off), with the impulse fed from up to taps frames of the audio before
        the range (kept as it was) and nothing rings past the range's end. Nullptr as apply. */
    AudioEdits::Snapshot applyLinearPhase (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const std::vector<Band>&);

    /** Runs the bands, in order, over [start, end), each on the channels it works on. The filters first run over up
        to a second of the audio before the range, so the range starts as it would in a longer
        pass; that audio is kept as it was. Nothing rings past the range's end. Nullptr for no
        bands, a band that isn't valid or an empty range. */
    AudioEdits::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const std::vector<Band>&);
}
