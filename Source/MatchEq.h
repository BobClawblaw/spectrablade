#pragma once

#include "Equalizer.h"

/** Match EQ: the tone of a reference taken onto a sound. A profile is a sound's long-term spectrum
    at twelfth-octave points from 20 Hz to 20 kHz: 8,192-frame Hann windows, hopped by half, their
    power averaged, each point the mean power of the bins in its twelfth of an octave (the lowest
    points, where a bin is wider than that, take the bins round them). The correction is the
    reference's profile less the sound's, smoothed over a width in octaves, with the mean over 100 Hz
    to 10 kHz taken out (so the match changes the tone, not the loudness), kept within a most, and
    scaled by the amount; it is applied in linear phase (Equalizer::LinearPhase from the curve), so
    the result lands where the audio was. */
namespace MatchEq
{
    constexpr int pointsPerOctave = 12, numPoints = 121;   // 20 Hz to 20,480 Hz
    constexpr double lowestHz = 20.0;

    /** Point i's frequency: 20 Hz times 2 to the i / 12. */
    double frequencyOf (int point);

    struct Profile
    {
        double sampleRate = 0.0;
        std::array<double, numPoints> levelDb {};   // dBFS, the power in each twelfth of an octave
    };

    /** The profile of [start, end): nothing for less than a window (8,192 frames) or silence. */
    std::optional<Profile> measure (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate);

    struct Settings
    {
        double amountPercent = 100.0;     // 0 to 100
        double smoothingOctaves = 1.0 / 3.0;   // 1/12 to 2: the width the correction is averaged over
        double mostDb = 12.0;             // 0 to 24: no point corrected by more
    };

    bool isValid (const Settings&);

    /** The correction for each point, in dB: reference less sound, smoothed, the mean over 100 Hz to
        10 kHz taken out, within mostDb, scaled by the amount. Points the sound or the reference has
        no energy at (under -100 dBFS) are left at 0. */
    std::array<double, numPoints> correctionDb (const Profile& reference, const Profile& sound, const Settings&);

    /** The correction at any frequency: between the points on a log scale, flat outside them. */
    double correctionAt (const std::array<double, numPoints>& correction, double frequency);

    /** Applies the match over [start, end): the sound's profile is measured over the range, the
        correction made against the reference and applied in linear phase. Nullptr for settings
        that aren't valid, a range too short to measure, or a reference at another rate. */
    AudioEdits::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Profile& reference, const Settings&);
}
