// Samples outside an edit, and a correction by nothing, are compared bit for bit on purpose.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/PhaseCorrection.h"
#include "TestUtils.h"
#include "PhaseCorrectionTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;
    constexpr int order = 18;            // 262144 frames, about 5.5 s
    constexpr int length = 1 << order;

    /** White noise up to 16 kHz, of a given seed, at about 0.2 RMS. */
    std::vector<float> noise (int seed)
    {
        std::vector<float> x ((size_t) length * 2, 0.0f);
        juce::Random random (seed);

        for (int i = 0; i < length; ++i)
            x[(size_t) i] = 2.0f * random.nextFloat() - 1.0f;

        juce::dsp::FFT fft (order);
        fft.performRealOnlyForwardTransform (x.data(), true);

        for (int k = (int) (16000.0 / rate * length); k <= length / 2; ++k)
            x[(size_t) (2 * k)] = x[(size_t) (2 * k + 1)] = 0.0f;

        fft.performRealOnlyInverseTransform (x.data());
        x.resize ((size_t) length);
        double sum = 0.0;

        for (auto v : x)
            sum += (double) v * v;

        const auto scale = (float) (0.2 / std::sqrt (sum / length));

        for (auto& v : x)
            v *= scale;

        return x;
    }

    /** The signal `by` frames later (fractions too), going round from the end: exactly, as it is
        band-limited. */
    std::vector<float> delayed (const std::vector<float>& in, double by)
    {
        std::vector<float> x ((size_t) length * 2, 0.0f);
        std::copy (in.begin(), in.end(), x.begin());
        juce::dsp::FFT fft (order);
        fft.performRealOnlyForwardTransform (x.data(), true);

        for (int k = 0; k <= length / 2; ++k)
        {
            const auto w = -juce::MathConstants<double>::twoPi * k * by / length;
            const std::complex<double> v (x[(size_t) (2 * k)], x[(size_t) (2 * k + 1)]);
            const auto turned = v * std::polar (1.0, w);
            x[(size_t) (2 * k)] = (float) turned.real();
            x[(size_t) (2 * k + 1)] = (float) turned.imag();
        }

        fft.performRealOnlyInverseTransform (x.data());
        x.resize ((size_t) length);
        return x;
    }

    AudioDocument::Snapshot snapshotOf (const std::vector<float>& left, const std::vector<float>& right)
    {
        juce::AudioBuffer<float> b (2, (int) left.size());

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            b.setSample (0, i, left[(size_t) i]);
            b.setSample (1, i, right[(size_t) i]);
        }

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, (juce::int64) left.size() } };
        return std::make_shared<const AudioSnapshot> (2, std::move (segments));
    }

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    std::vector<float> plus (const std::vector<float>& a, const std::vector<float>& b, float gain = 1.0f)
    {
        auto out = a;

        for (size_t i = 0; i < out.size(); ++i)
            out[i] += gain * b[i];

        return out;
    }

    std::vector<float> scaled (std::vector<float> x, float gain)
    {
        for (auto& v : x)
            v *= gain;

        return x;
    }

    /** How far apart the channels are over the middle of the file, against the left's power, in dB
        (after turning the right over when `turned`). */
    double apart (const juce::AudioBuffer<float>& b)
    {
        double diff = 0.0, power = 0.0;

        for (int i = (int) rate; i < length - (int) rate; ++i)
        {
            const auto d = (double) b.getSample (0, i) - b.getSample (1, i);
            diff += d * d;
            power += (double) b.getSample (0, i) * b.getSample (0, i);
        }

        return 10.0 * std::log10 (juce::jmax (diff, 1.0e-30) / power);
    }

    void testOffsets()
    {
        const auto source = noise (3);
        juce::StringArray seen;
        auto ok = true;

        // The right channel late, early, by whole frames and by fractions; the same sound in both.
        for (auto by : { 7.3, -12.6, 20.0, 0.25 })
        {
            const auto in = snapshotOf (source, delayed (source, by));
            const auto found = PhaseCorrection::measure (*in, 0, length, rate, 1.0);
            auto error = 99.0, after = 0.0;

            if (found.has_value())
            {
                error = std::abs (found->offset - by);

                if (const auto out = PhaseCorrection::correct (*in, 0, length, rate, found->offset, false))
                    after = apart (readAll (*out));
            }

            ok = ok && error < 0.01 && found->correlation > 0.99 && after < -80.0;
            seen.add (juce::String (by, 2) + " found as " + (found.has_value() ? juce::String (found->offset, 3) : juce::String ("nothing"))
                      + ", then " + juce::String (after, 1) + " dB apart");
        }

        check (ok, "phase correction: the right channel late or early by whole frames or fractions of one is found within 0.01 frames, "
                   "and moved back the channels are the same to within 80 dB (" + seen.joinIntoString ("; ") + ")");

        // Two microphones: the same sound 4.6 frames apart, each with its own noise 10 dB down.
        const auto mic = snapshotOf (plus (source, noise (11), 0.316f), plus (delayed (source, 4.6), noise (12), 0.316f));
        const auto heard = PhaseCorrection::measure (*mic, 0, length, rate, 2.0);
        const auto miss = heard.has_value() ? std::abs (heard->offset - 4.6) : 99.0;
        check (miss < 0.02 && heard->correlation > 0.85 && heard->correlation < 0.95,
               "phase correction: the same sound 4.6 frames apart with each channel's own noise 10 dB down is found within 0.02 frames ("
                   + (heard.has_value() ? juce::String (heard->offset, 3) + ", correlation " + juce::String (heard->correlation, 3) : juce::String ("nothing")) + ")");
    }

    void testUpsideDownAndNothing()
    {
        const auto source = noise (5);

        // Upside down and 24 frames late: found as both; turned over and moved, the same.
        const auto in = snapshotOf (source, scaled (delayed (source, 24.0), -1.0f));
        const auto found = PhaseCorrection::measure (*in, 0, length, rate, 1.0);
        auto after = 0.0;

        if (found.has_value())
            if (const auto out = PhaseCorrection::correct (*in, 0, length, rate, found->offset, true))
                after = apart (readAll (*out));

        check (found.has_value() && std::abs (found->offset - 24.0) < 0.01 && found->correlation < -0.99 && after < -80.0,
               "phase correction: a right channel upside down and 24 frames late is found as both (" + (found.has_value() ? juce::String (found->offset, 3)
                   + ", correlation " + juce::String (found->correlation, 3) : juce::String ("nothing")) + "), and turned over and moved back "
                   "the channels are the same (" + juce::String (after, 1) + " dB apart)");

        // Unrelated channels: no peak worth the name. Silence, an offset past the most looked for
        // (40 frames, looking up to 0.5 ms, 24 frames), mono and settings out of range: nothing.
        const auto unrelated = PhaseCorrection::measure (*snapshotOf (source, noise (6)), 0, length, rate, 1.0);
        const auto silent = PhaseCorrection::measure (*snapshotOf (std::vector<float> ((size_t) length), std::vector<float> ((size_t) length)), 0, length, rate, 1.0);
        const auto far = PhaseCorrection::measure (*snapshotOf (source, delayed (source, 40.0)), 0, length, rate, 0.5);
        const auto near = PhaseCorrection::measure (*snapshotOf (source, delayed (source, 40.0)), 0, length, rate, 1.0);
        juce::AudioBuffer<float> one (1, length);
        one.clear();
        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (one)), 0, (juce::int64) length } };
        const auto mono = std::make_shared<const AudioSnapshot> (1, std::move (segments));
        int calls = 0;
        const auto stopped = PhaseCorrection::measure (*in, 0, length, rate, 1.0, [&] (float) { return ++calls < 2; });

        check ((! unrelated.has_value() || std::abs (unrelated->correlation) < PhaseCorrection::weakest) && ! silent.has_value() && ! far.has_value()
                   && near.has_value() && std::abs (near->offset - 40.0) < 0.01
                   && ! PhaseCorrection::measure (*mono, 0, length, rate, 1.0).has_value() && ! PhaseCorrection::measure (*in, 0, length, rate, 0.05).has_value()
                   && ! PhaseCorrection::measure (*in, 0, length, rate, 51.0).has_value() && ! PhaseCorrection::measure (*in, 0, 500, rate, 1.0).has_value()
                   && ! stopped.has_value() && calls == 2,
               "phase correction: unrelated channels show no offset (correlation " + (unrelated.has_value() ? juce::String (unrelated->correlation, 3) : juce::String ("none"))
                   + "); silence, an offset past the most looked for, a mono file, too short a range and settings out of range give nothing, "
                   "and it can be stopped");
    }

    void testRange()
    {
        const auto source = noise (8);
        const auto in = snapshotOf (source, delayed (source, 9.5));
        const auto before = readAll (*in);
        const juce::int64 start = (juce::int64) rate, end = 3 * (juce::int64) rate;
        const auto out = PhaseCorrection::correct (*in, start, end, rate, 9.5, false);
        const auto none = PhaseCorrection::correct (*in, 0, length, rate, 0.0, false);
        auto outsideSame = out != nullptr, insideCloser = false, noneSame = none != nullptr;
        auto largestStep = 0.0f, usualStep = 0.0f;

        if (out != nullptr)
        {
            const auto after = readAll (*out);
            double was = 0.0, now = 0.0;

            for (int i = 0; i < length; ++i)
            {
                for (int ch = 0; ch < 2; ++ch)
                    if (i < start || i >= end)
                        outsideSame = outsideSame && before.getSample (ch, i) == after.getSample (ch, i);

                if (i >= start + 1000 && i < end - 1000)
                {
                    was += std::pow ((double) before.getSample (0, i) - before.getSample (1, i), 2.0);
                    now += std::pow ((double) after.getSample (0, i) - after.getSample (1, i), 2.0);
                }

                // From one frame to the next: no bigger at the range's edges than anywhere.
                if (i > 0)
                {
                    const auto step = std::abs (after.getSample (1, i) - after.getSample (1, i - 1));
                    const auto atEdge = std::abs (i - start) < 2 || std::abs (i - end) < 2;
                    (atEdge ? largestStep : usualStep) = juce::jmax (atEdge ? largestStep : usualStep, step);
                }
            }

            insideCloser = now < was * 1.0e-6;
        }

        if (none != nullptr)
        {
            const auto same = readAll (*none);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < length; ++i)
                    noneSame = noneSame && same.getSample (ch, i) == before.getSample (ch, i);
        }

        check (outsideSame && insideCloser && largestStep <= usualStep && noneSame
                   && PhaseCorrection::correct (*in, 10, 10, rate, 1.0, false) == nullptr,
               "phase correction: corrected over a range, the audio outside it is left bit for bit, the channels in it are in line, and the "
               "edges ease in with no jump; moved by nothing, the audio is the same bit for bit");
    }
}

void runPhaseCorrectionTests()
{
    testOffsets();
    testUpsideDownAndNothing();
    testRange();
}
