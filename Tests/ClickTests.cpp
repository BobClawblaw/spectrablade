// Untouched samples are compared bit for bit on purpose.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/ClickRepair.h"
#include "TestUtils.h"
#include "ClickTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;
    constexpr double twoPi = juce::MathConstants<double>::twoPi;
    constexpr int second = (int) rate;

    double toDb (double gain)                     { return 20.0 * std::log10 (juce::jmax (gain, 1.0e-30)); }

    /** Stereo, 10 s: three tones (about -10 dBFS together) over white noise at -60 dBFS RMS. */
    juce::AudioBuffer<float> clean (int seconds = 10)
    {
        juce::AudioBuffer<float> b (2, seconds * second);
        juce::Random random (99);
        const auto noise = std::pow (10.0, -60.0 / 20.0) * std::sqrt (3.0);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (ch, i, (float) (0.15 * std::sin (twoPi * 220.0 * i / rate + ch)
                                             + 0.1 * std::sin (twoPi * 1320.0 * i / rate)
                                             + 0.05 * std::sin (twoPi * 3150.0 * i / rate + 2.0 * ch)
                                             + noise * (2.0 * random.nextDouble() - 1.0)));

        return b;
    }

    struct Click
    {
        int position, length;
    };

    /** Plants clicks of 1, 5 and 20 samples, every 0.3 s or so, alternately in one or both channels. */
    std::vector<Click> plant (juce::AudioBuffer<float>& b, int count)
    {
        juce::Random random (7);
        std::vector<Click> clicks;

        for (int n = 0; n < count; ++n)
        {
            const auto position = second / 4 + n * (int) (0.3 * rate) + random.nextInt (2000);
            const auto length = n % 3 == 0 ? 1 : n % 3 == 1 ? 5 : 20;

            for (int ch = 0; ch < (n % 2 == 0 ? 2 : 1); ++ch)
                for (int i = 0; i < length; ++i)
                    b.addSample (ch, position + i, (float) ((n % 4 < 2 ? 0.5 : -0.4) * std::exp (-i / 6.0)
                                                           * (length == 20 ? (random.nextFloat() * 2.0f - 1.0f) : 1.0f)));

            clicks.push_back ({ position, length });
        }

        return clicks;
    }

    AudioDocument::Snapshot snapshotOf (const juce::AudioBuffer<float>& b)
    {
        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (juce::AudioBuffer<float> (b)), 0, b.getNumSamples() } };
        return std::make_shared<const AudioSnapshot> (b.getNumChannels(), std::move (segments));
    }

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }
}

void runClickTests()
{
    const auto reference = clean();

    // Clean audio: nothing to repair.
    {
        const auto r = ClickRepair::repair (*snapshotOf (reference), 0, reference.getNumSamples(), rate, {});
        check (r.has_value() && r->repaired == 0 && r->snapshot == nullptr,
               "click repair: finds nothing in clean audio (" + juce::String (r.has_value() ? r->repaired : -1) + " found)");
    }

    // Silence: the error floor keeps it from reading as clicks.
    {
        juce::AudioBuffer<float> silence (2, 2 * second);
        silence.clear();
        const auto r = ClickRepair::repair (*snapshotOf (silence), 0, silence.getNumSamples(), rate, {});
        check (r.has_value() && r->repaired == 0, "click repair: finds nothing in silence");
    }

    auto damaged = reference;
    const auto clicks = plant (damaged, 30);
    const auto snapshot = snapshotOf (damaged);

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto r = ClickRepair::repair (*snapshot, 0, damaged.getNumSamples(), rate, {});
    const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
    check (r.has_value() && r->snapshot != nullptr, "click repair: repairs planted clicks");

    if (! r.has_value() || r->snapshot == nullptr)
        return;

    check (r->repaired == (int) clicks.size() && r->skipped == 0,
           "click repair: " + juce::String (r->repaired) + " repaired, " + juce::String (r->skipped) + " skipped (planted "
               + juce::String ((int) clicks.size()) + ")");

    const auto repaired = readAll (*r->snapshot);

    // Every sample that changed is near a planted click.
    {
        bool onlyNearClicks = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < repaired.getNumSamples(); ++i)
                if (repaired.getSample (ch, i) != damaged.getSample (ch, i))
                {
                    const auto near = std::any_of (clicks.begin(), clicks.end(), [i] (const Click& c)
                                                   { return i >= c.position - 40 && i < c.position + c.length + 40; });
                    onlyNearClicks = onlyNearClicks && near;
                }

        check (onlyNearClicks, "click repair: only samples near the clicks change");
    }

    // The repairs match the clean signal: the error is about the noise floor the model cannot predict.
    {
        double worstClick = -300.0, errorEnergy = 0.0, signalEnergy = 0.0;

        for (auto& c : clicks)
        {
            double sum = 0.0;
            const auto from = c.position - 8, to = c.position + c.length + 8;

            for (int ch = 0; ch < 2; ++ch)
                for (int i = from; i < to; ++i)
                    sum += juce::square ((double) repaired.getSample (ch, i) - reference.getSample (ch, i));

            worstClick = juce::jmax (worstClick, toDb (std::sqrt (sum / (2 * (to - from)))));
        }

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < repaired.getNumSamples(); ++i)
            {
                errorEnergy += juce::square ((double) repaired.getSample (ch, i) - reference.getSample (ch, i));
                signalEnergy += juce::square ((double) reference.getSample (ch, i));
            }

        check (worstClick < -50.0, "click repair: worst click is left at " + juce::String (worstClick, 1) + " dBFS RMS (was about -10)");
        check (toDb (std::sqrt (signalEnergy / errorEnergy)) > 60.0,
               "click repair: whole file matches the clean signal to " + juce::String (toDb (std::sqrt (signalEnergy / errorEnergy)), 1) + " dB");
    }

    // A range: clicks outside it stay.
    {
        const auto part = ClickRepair::repair (*snapshot, 3 * second, 6 * second, rate, {});
        const auto inside = std::count_if (clicks.begin(), clicks.end(), [] (const Click& c)
                                           { return c.position >= 3 * second && c.position + c.length + 3 < 6 * second; });
        check (part.has_value() && part->repaired == (int) inside,
               "click repair: a range repairs only its own " + juce::String ((int) inside) + " clicks ("
                   + juce::String (part.has_value() ? part->repaired : -1) + ")");
    }

    // A burst of noise longer than the limit is found but left alone.
    {
        auto burst = reference;
        juce::Random random (3);

        for (int i = 0; i < 288; ++i)   // 6 ms
            burst.addSample (0, 2 * second + i, 0.3f * (2.0f * random.nextFloat() - 1.0f));

        const auto res = ClickRepair::repair (*snapshotOf (burst), 0, burst.getNumSamples(), rate, {});
        check (res.has_value() && res->repaired == 0 && res->skipped == 1 && res->snapshot == nullptr,
               "click repair: a 6 ms burst is longer than the 3 ms limit and is skipped");
    }

    // A steady 10 ms tone burst is predictable, so it is not taken for a click.
    {
        auto pop = reference;

        for (int i = 0; i < 480; ++i)
            pop.addSample (0, 2 * second + i, (float) (0.4 * std::exp (-i / 200.0) * ((i / 3) % 2 == 0 ? 1 : -1)));

        const auto res = ClickRepair::repair (*snapshotOf (pop), 0, pop.getNumSamples(), rate, {});
        check (res.has_value() && res->repaired == 0, "click repair: a 10 ms tone burst is left alone");
    }

    // Cancelling.
    {
        int calls = 0;
        check (! ClickRepair::repair (*snapshot, 0, damaged.getNumSamples(), rate, {}, [&] (float) { return ++calls < 2; }).has_value(),
               "click repair: can be cancelled");
    }

    std::cout << "      (10 s of stereo in " << std::llround (ms) << " ms)" << std::endl;

    // Speed: 5 minutes of stereo with a click every 0.3 s.
    {
        auto longer = clean (300);
        const auto many = plant (longer, 990);
        const auto big = snapshotOf (longer);
        const auto t1 = juce::Time::getMillisecondCounterHiRes();
        const auto res = ClickRepair::repair (*big, 0, big->getNumFrames(), rate, {});
        const auto seconds = (juce::Time::getMillisecondCounterHiRes() - t1) / 1000.0;
        check (res.has_value() && res->repaired == (int) many.size(),
               "click repair: 5 minutes of stereo with " + juce::String ((int) many.size()) + " clicks in "
                   + juce::String (seconds, 2) + " s (" + juce::String (res.has_value() ? res->repaired : -1) + " repaired)");
    }
}
