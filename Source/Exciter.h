#pragma once

#include "Multiband.h"
#include "Oversampling.h"

/** An exciter: the sound split into lows, middle and highs (the multiband split, 24 dB an octave),
    each band driven into a saturation curve of its own and mixed back with the band as it was. The
    curves (each with a slope of 1 at zero, so a quiet sound passes as it is, and bounded):
    Tube, soft and uneven (it rounds the negative half sooner), so it adds even harmonics with the
    odd; Tape, soft and even on both halves (tanh), odd harmonics only; Transistor, harder, a cubic
    that reaches a flat top, odd harmonics with more of the higher ones. The drive sets how far the
    band goes into the curve (the band scaled up by the drive, shaped, and scaled back), so the
    harmonics grow with it while the level stays about where it was. The curve sits 12 dB of headroom
    past full scale (headroomDb): at 0 dB of drive a full-scale band only just reaches the bend
    (0.18 dB down at a full-scale peak, nothing on a quiet one), so the effect at 0 dB leaves a sound
    as it was, a hot master included; the drive brings the bend closer. The shaping runs at four times
    the rate (Oversampler4), so the harmonics above Nyquist are filtered off, not folded back, and
    the band as it was is delayed to match; the whole comes out Oversampler4::latency frames late,
    which the live effect reports and the offline apply takes off. A one-pole high-pass at 5 Hz
    after each band's shaping takes off the offset the uneven curve makes. */
namespace Exciter
{
    enum class Curve { tube, tape, transistor };

    /** How far past full scale the curve's bend sits at 0 dB of drive. */
    constexpr double headroomDb = 12.0;

    const juce::StringArray& curveNames();

    /** The curve's output for an input (any size; from -1 to 1 for a full-scale sample). */
    double shape (Curve, double x) noexcept;

    struct Band
    {
        double driveDb = 0.0;        // 0 to 36 dB into the curve (0 leaves the band as it is, within 0.18 dB at full scale)
        Curve curve = Curve::tube;
        double mixPercent = 100.0;   // 0 (the band as it was) to 100 (the shaped band alone)
    };

    struct Settings
    {
        double lowCrossover = 200.0;
        double highCrossover = 3000.0;
        std::array<Band, 3> bands {};   // low, middle, high
        double outputDb = 0.0;          // -24 to +24, after the bands
    };

    /** Whether the settings can be used at this rate: the crossovers as Multiband::Width's, each
        drive from 0 to 36 dB, each mix from 0 to 100 %, the output within 24 dB. */
    bool isValid (const Settings&, double sampleRate);

    /** The settings with the crossovers moved into what the rate allows (as Multiband::spaced). */
    Settings spaced (Settings, double sampleRate);

    class Core
    {
    public:
        static constexpr int latency = Oversampler4::latency;

        void prepare (int numChannels, double sampleRate);
        void reset() noexcept;
        void set (const Settings&);   // valid at the prepared rate

        /** In place; each frame comes out latency frames late. */
        void process (float* const* channels, int numChannels, int numFrames) noexcept;

    private:
        struct Channel
        {
            Multiband::Crossover first, second, lowAlign;
            std::array<Oversampler4, 3> shapers;
            std::array<std::array<double, Oversampler4::latency>, 3> dry {};   // each band as it was, delayed
            std::array<double, 3> blockIn {}, blockOut {};                     // the high-pass's state a band
        };

        double rate = 48000.0;
        Settings settings;
        std::array<double, 3> drive { 1.0, 1.0, 1.0 }, wet { 1.0, 1.0, 1.0 };
        double output = 1.0, blocker = 0.0;
        std::vector<Channel> channels;
        int dryPos = 0;
    };

    /** Runs the exciter over [start, end): the result lands where the audio was (the latency taken
        off), primed on up to a second of the audio before. Nullptr for settings that aren't valid
        or an empty range. */
    AudioEdits::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&);
}
