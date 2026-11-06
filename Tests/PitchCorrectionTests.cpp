// Samples outside an edit are compared bit for bit on purpose.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/PitchCorrection.h"
#include "../Source/AudioEdits.h"
#include "TestUtils.h"
#include "PitchCorrectionTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;
    constexpr int second = (int) rate;

    /** A voice-like buzz: 30 harmonics falling 6 dB an octave, shaped by a formant at 800 Hz, at
        the pitch hz (i) gives for each frame, at about -12 dBFS peak; both channels alike. */
    AudioDocument::Snapshot buzz (int frames, const std::function<double (int)>& hz)
    {
        juce::AudioBuffer<float> b (2, frames);
        double phase = 0.0;

        for (int i = 0; i < frames; ++i)
        {
            const auto f0 = hz (i);
            double v = 0.0;

            for (int h = 1; h <= 30 && h * f0 < 0.45 * rate; ++h)
            {
                const auto formant = 1.0 / std::sqrt (1.0 + std::pow ((h * f0 - 800.0) / 300.0, 2.0));
                v += std::sin (h * phase) / h * (0.3 + formant);
            }

            b.setSample (0, i, (float) (0.12 * v));
            b.setSample (1, i, (float) (0.12 * v));
            phase += juce::MathConstants<double>::twoPi * f0 / rate;
        }

        return AudioEdits::fromBuffer (std::move (b));
    }

    double cents (double hz, double reference) { return 1200.0 * std::log2 (hz / reference); }

    /** The median pitch over [from, to) frames, from the detector; 0 if mostly without one. */
    double medianPitch (const AudioSnapshot& s, juce::int64 from, juce::int64 to)
    {
        const auto track = PitchCorrection::detect (s, from, to, rate);
        std::vector<float> voiced;

        for (auto hz : track.hz)
            if (hz > 0.0f)
                voiced.push_back (hz);

        if (voiced.size() * 2 < track.hz.size())
            return 0.0;

        std::nth_element (voiced.begin(), voiced.begin() + (long) voiced.size() / 2, voiced.end());
        return voiced[voiced.size() / 2];
    }

    double rms (const AudioSnapshot& s, juce::int64 from, juce::int64 to)
    {
        juce::AudioBuffer<float> b (1, (int) (to - from));
        s.read (b, 0, from, b.getNumSamples());
        return b.getRMSLevel (0, 0, b.getNumSamples());
    }

    void testDetect()
    {
        juce::StringArray seen;
        auto worst = 0.0;

        for (auto hz : { 98.0, 220.0, 440.5, 830.0 })
        {
            const auto s = buzz (second, [hz] (int) { return hz; });
            const auto found = medianPitch (*s, 0, second);
            worst = juce::jmax (worst, found > 0.0 ? std::abs (cents (found, hz)) : 1200.0);
            seen.add (juce::String (hz, 1) + " as " + juce::String (found, 2));
        }

        // Noise and silence: no pitch.
        juce::AudioBuffer<float> noise (1, second);
        juce::Random random (4);

        for (int i = 0; i < second; ++i)
            noise.setSample (0, i, 0.2f * (2.0f * random.nextFloat() - 1.0f));

        const auto noisy = AudioEdits::fromBuffer (std::move (noise));
        const auto noiseTrack = PitchCorrection::detect (*noisy, 0, second, rate);
        const auto voicedInNoise = std::count_if (noiseTrack.hz.begin(), noiseTrack.hz.end(), [] (float h) { return h > 0.0f; });
        juce::AudioBuffer<float> quiet (1, second);
        quiet.clear();
        const auto silent = PitchCorrection::detect (*AudioEdits::fromBuffer (std::move (quiet)), 0, second, rate);
        const auto voicedInSilence = std::count_if (silent.hz.begin(), silent.hz.end(), [] (float h) { return h > 0.0f; });

        check (worst < 1.0 && voicedInNoise * 20 < (long) noiseTrack.hz.size() && voicedInSilence == 0 && noiseTrack.hz.size() == 200,
               "pitch correction: the pitch of a buzz is found within 1 cent (" + seen.joinIntoString (", ") + " Hz; worst "
                   + juce::String (worst, 2) + " cents); white noise has a pitch in " + juce::String ((int) voicedInNoise) + " of "
                   + juce::String ((int) noiseTrack.hz.size()) + " frames, silence in none");
    }

    void testScales()
    {
        // 228 Hz: 38 cents below A#3 (233.08 Hz), 62 above A3 (220). Chromatic, it goes to A#; in C
        // major, with no A#, to A; at half strength, half way to A#.
        const auto in = buzz (2 * second, [] (int) { return 228.0; });
        const auto run = [&] (PitchCorrection::Settings s)
        {
            const auto out = PitchCorrection::correct (*in, 0, 2 * second, rate, s);
            return out != nullptr ? medianPitch (*out, second / 2, 3 * second / 2) : 0.0;
        };

        PitchCorrection::Settings chromatic;
        PitchCorrection::Settings major;
        major.scale = PitchCorrection::Scale::major;
        PitchCorrection::Settings half;
        half.strengthPercent = 50.0f;
        const auto aSharp = 440.0 * std::pow (2.0, 1.0 / 12.0) / 2.0;
        const auto toChromatic = cents (run (chromatic), aSharp), toMajor = cents (run (major), 220.0);
        const auto halfWay = cents (run (half), 228.0), wanted = 0.5 * cents (aSharp, 228.0);
        const auto out = PitchCorrection::correct (*in, 0, 2 * second, rate, chromatic);
        const auto levelChange = out != nullptr ? juce::Decibels::gainToDecibels (rms (*out, second / 2, 3 * second / 2) / rms (*in, second / 2, 3 * second / 2)) : 99.0f;

        check (std::abs (toChromatic) < 3.0 && std::abs (toMajor) < 3.0 && std::abs (halfWay - wanted) < 3.0 && std::abs (levelChange) < 0.5,
               "pitch correction: a voice at 228 Hz goes to A#3 in the chromatic scale (" + juce::String (toChromatic, 2) + " cents off), to A3 in "
               "C major (" + juce::String (toMajor, 2) + " cents off), half way at half strength (" + juce::String (halfWay, 1) + " cents for "
                   + juce::String (wanted, 1) + "), its level kept (" + juce::String (levelChange, 2) + " dB)");
    }

    void testGlideAndSpeed()
    {
        // A glide from 200 to 260 Hz over 3 s, corrected at once: every moment on a note (all but
        // the moments a note changes).
        const auto glideIn = buzz (3 * second, [] (int i) { return 200.0 * std::pow (260.0 / 200.0, (double) i / (3.0 * rate)); });
        PitchCorrection::Settings now;
        now.speedMs = 0.0f;
        const auto glideOut = PitchCorrection::correct (*glideIn, 0, 3 * second, rate, now);
        auto onNote = 0, voiced = 0;

        if (glideOut != nullptr)
            for (auto hz : PitchCorrection::detect (*glideOut, second / 10, 3 * second - second / 10, rate).hz)
                if (hz > 0.0f)
                {
                    ++voiced;
                    const auto note = 69.0 + 12.0 * std::log2 (hz / 440.0);
                    onNote += std::abs (note - std::round (note)) < 0.05 ? 1 : 0;
                }

        // Slowly (200 ms): a note at 228 Hz is still near where it was 30 ms in, and on A# by 1.5 s.
        const auto in = buzz (2 * second, [] (int) { return 228.0; });
        PitchCorrection::Settings slow;
        slow.speedMs = 200.0f;
        const auto out = PitchCorrection::correct (*in, 0, 2 * second, rate, slow);
        const auto aSharp = 440.0 * std::pow (2.0, 1.0 / 12.0) / 2.0;
        const auto early = out != nullptr ? cents (medianPitch (*out, (int) (0.02 * rate), (int) (0.04 * rate)), 228.0) : 99.0;
        const auto late = out != nullptr ? cents (medianPitch (*out, (int) (1.4 * rate), (int) (1.6 * rate)), aSharp) : 99.0;

        check (voiced > 400 && onNote * 100 >= voiced * 95 && std::abs (early) < 10.0 && std::abs (late) < 3.0,
               "pitch correction: a glide from 200 to 260 Hz corrected at once is within 5 cents of a note in " + juce::String (onNote) + " of "
                   + juce::String (voiced) + " frames; corrected over 200 ms, a note 38 cents flat is " + juce::String (early, 1)
                   + " cents from where it was 30 ms in and " + juce::String (late, 2) + " cents from A# by 1.5 s");
    }

    void testKeptAndRange()
    {
        // In tune: kept in tune. Noise (no pitch): passes as it was, to float rounding.
        const auto tuned = buzz (2 * second, [] (int) { return 220.0; });
        const auto tunedOut = PitchCorrection::correct (*tuned, 0, 2 * second, rate, {});
        const auto stays = tunedOut != nullptr ? cents (medianPitch (*tunedOut, second / 2, 3 * second / 2), 220.0) : 99.0;

        juce::AudioBuffer<float> noise (2, second);
        juce::Random random (9);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < second; ++i)
                noise.setSample (ch, i, 0.2f * (2.0f * random.nextFloat() - 1.0f));

        const auto noisy = AudioEdits::fromBuffer (juce::AudioBuffer<float> (noise));
        const auto noiseOut = PitchCorrection::correct (*noisy, 0, second, rate, {});
        auto largest = noiseOut != nullptr ? 0.0f : 1.0f;

        if (noiseOut != nullptr)
        {
            juce::AudioBuffer<float> b (2, second);
            noiseOut->read (b, 0, 0, second);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < second; ++i)
                    largest = juce::jmax (largest, std::abs (b.getSample (ch, i) - noise.getSample (ch, i)));
        }

        // A range: the audio outside it bit for bit, and the pitch in it moved.
        const auto in = buzz (3 * second, [] (int) { return 228.0; });
        const juce::int64 start = second, end = 2 * second;
        const auto out = PitchCorrection::correct (*in, start, end, rate, {});
        auto outsideSame = out != nullptr;

        if (out != nullptr)
        {
            juce::AudioBuffer<float> a (2, 3 * second), b (2, 3 * second);
            in->read (a, 0, 0, a.getNumSamples());
            out->read (b, 0, 0, b.getNumSamples());

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 3 * second; ++i)
                    if (i < start || i >= end)
                        outsideSame = outsideSame && a.getSample (ch, i) == b.getSample (ch, i);
        }

        const auto aSharp = 440.0 * std::pow (2.0, 1.0 / 12.0) / 2.0;
        const auto inside = out != nullptr ? cents (medianPitch (*out, start + second / 4, end - second / 4), aSharp) : 99.0;
        int calls = 0;
        const auto stopped = PitchCorrection::correct (*in, 0, 3 * second, rate, {}, [&] (float) { return ++calls < 2; });
        PitchCorrection::Settings badKey, badSpeed, badStrength;
        badKey.key = 12;
        badSpeed.speedMs = 501.0f;
        badStrength.strengthPercent = -1.0f;

        check (std::abs (stays) < 1.0 && largest < 1.0e-6f && outsideSame && std::abs (inside) < 3.0 && stopped == nullptr && calls == 2
                   && PitchCorrection::correct (*in, 10, 10, rate, {}) == nullptr && ! PitchCorrection::isValid (badKey)
                   && ! PitchCorrection::isValid (badSpeed) && ! PitchCorrection::isValid (badStrength),
               "pitch correction: a voice in tune stays in tune (" + juce::String (stays, 2) + " cents), noise passes as it was (within "
                   + juce::String (largest, 8) + "), a range moves the pitch in it (" + juce::String (inside, 2) + " cents from A#) and leaves "
                   "the audio outside it bit for bit; an empty range and settings out of range are refused, and it can be stopped");
    }
}

void runPitchCorrectionTests()
{
    testDetect();
    testScales();
    testGlideAndSpeed();
    testKeptAndRange();
}
