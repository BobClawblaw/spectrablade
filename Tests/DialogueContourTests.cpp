#include <JuceHeader.h>
#include "../Source/DialogueContour.h"
#include "../Source/AudioEdits.h"
#include "TestUtils.h"
#include "DialogueContourTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    /** A phrase: a voice-like buzz at 200 Hz (eight harmonics) for 1.2 s, silence to 1.5 s. */
    juce::AudioBuffer<float> phrase()
    {
        juce::AudioBuffer<float> b (1, (int) (1.5 * 48000));
        b.clear();

        for (int i = 0; i < (int) (1.2 * 48000); ++i)
        {
            const auto t = i / rate;
            auto v = 0.0;

            for (int h = 1; h <= 8; ++h)
                v += 0.15 / h * std::sin (juce::MathConstants<double>::twoPi * 200.0 * h * t);

            b.setSample (0, i, (float) (v * juce::jmin (1.0, i / 480.0) * juce::jmin (1.0, ((int) (1.2 * 48000) - i) / 480.0)));
        }

        return b;
    }

    /** The pitch at a time, in cents against 200 Hz, from the tracker. */
    double centsAt (const AudioSnapshot& s, double seconds)
    {
        const auto track = PitchCorrection::detect (s, 0, s.getNumFrames(), rate);
        const auto f = juce::jlimit (0, (int) track.hz.size() - 1, (int) ((seconds * rate - (double) track.first) / track.hop));
        return track.hz[(size_t) f] > 0.0f ? 1200.0 * std::log2 (track.hz[(size_t) f] / 200.0) : -10000.0;
    }
}

void runDialogueContourTests()
{
    std::cout << "Dialogue contour:" << std::endl;

    // A fall of 4 semitones over the last 0.4 s: the phrase is unchanged before the ending, half way
    // down in its middle, and 4 semitones down at its end; a rise goes the other way; the shifts
    // are 0 before the ending; refused settings; cancelling.
    {
        const auto in = phrase();
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        DialogueContour::Settings fall;
        fall.semitones = -4.0f;
        fall.endingSeconds = 0.4f;
        const auto fallen = DialogueContour::apply (*snap, 0, in.getNumSamples(), rate, fall);
        DialogueContour::Settings rise = fall;
        rise.semitones = 3.0f;
        const auto risen = DialogueContour::apply (*snap, 0, in.getNumSamples(), rate, rise);

        const auto before = centsAt (*fallen, 0.5), middle = centsAt (*fallen, 1.0), endFall = centsAt (*fallen, 1.17), endRise = centsAt (*risen, 1.17);
        const auto track = PitchCorrection::analyse (*snap, 0, in.getNumSamples(), rate);
        const auto shifts = DialogueContour::shiftsFor (track, 0, in.getNumSamples(), rate, fall);
        auto zeroBefore = true;

        for (size_t f = 0; f < shifts.size(); ++f)
            if ((double) (track.first + (juce::int64) f * track.hop) / rate < 0.75)
                zeroBefore = zeroBefore && juce::exactlyEqual (shifts[f], 0.0);

        DialogueContour::Settings bad;
        bad.semitones = 20.0f;
        const auto refused = DialogueContour::apply (*snap, 0, in.getNumSamples(), rate, bad) == nullptr && DialogueContour::apply (*snap, 100, 100, rate, fall) == nullptr;
        const auto cancelled = DialogueContour::apply (*snap, 0, in.getNumSamples(), rate, fall, [] (float) { return false; }) == nullptr;

        std::cout << "      a fall of 4 semitones over 0.4 s: at 0.5 s " << before << " cents, at 1.0 s " << middle << ", at 1.17 s " << endFall << "; a rise of 3: at 1.17 s " << endRise << std::endl;
        check (std::abs (before) < 5.0 && middle < -150.0 && middle > -250.0 && endFall < -330.0 && endFall > -430.0 && endRise > 230.0 && endRise < 330.0 && zeroBefore && refused && cancelled,
               "dialogue contour: a fall of 4 semitones over the last 0.4 s leaves the phrase within 5 cents before the ending, has it 150 to 250 cents "
               "down in the ending's middle and 330 to 430 down near its end; a rise of 3 puts the end 230 to 330 up; the shifts are 0 before the "
               "ending; a move past 12 semitones and an empty range are refused; it can be cancelled");
    }
}
