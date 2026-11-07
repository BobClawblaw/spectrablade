#include <JuceHeader.h>
#include "../Source/Loudness.h"
#include "../Source/AudioEdits.h"
#include "../Source/Delivery.h"
#include "TestUtils.h"
#include "LoudnessTests.h"

namespace
{
    using TestUtils::check;

    constexpr double twoPi = juce::MathConstants<double>::twoPi;

    struct Part
    {
        double seconds, dbfs;
    };

    /** A 1 kHz sine in every channel, its peak level stepping through the parts (EBU's signals). */
    juce::AudioBuffer<float> tone (std::initializer_list<Part> parts, double rate, int channels = 2, double hz = 1000.0, double phase = 0.0)
    {
        int total = 0;

        for (auto& p : parts)
            total += (int) std::llround (p.seconds * rate);

        juce::AudioBuffer<float> b (channels, total);
        int i = 0;

        for (auto& p : parts)
        {
            const auto gain = std::pow (10.0, p.dbfs / 20.0);

            for (int end = i + (int) std::llround (p.seconds * rate); i < end; ++i)
                for (int ch = 0; ch < channels; ++ch)
                    b.setSample (ch, i, (float) (gain * std::sin (twoPi * hz * i / rate + phase)));
        }

        return b;
    }

    /** 20 ms raised-cosine fades at both ends, so a hard start does not ring past the tone's peak. */
    juce::AudioBuffer<float> faded (juce::AudioBuffer<float> b, double rate)
    {
        const auto n = (int) (0.02 * rate);

        for (int i = 0; i < n; ++i)
        {
            const auto g = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * i / n));

            for (int ch = 0; ch < b.getNumChannels(); ++ch)
            {
                b.setSample (ch, i, b.getSample (ch, i) * g);
                b.setSample (ch, b.getNumSamples() - 1 - i, b.getSample (ch, b.getNumSamples() - 1 - i) * g);
            }
        }

        return b;
    }

    bool near (double value, double expected, double tolerance)  { return std::abs (value - expected) <= tolerance; }

    void integrated (const juce::String& name, const juce::AudioBuffer<float>& b, double rate, double expected)
    {
        const auto r = Loudness::measure (b, rate);
        check (near (r.integrated, expected, 0.1), name + " reads " + Loudness::format (r.integrated, "LUFS")
                                                       + " (EBU: " + juce::String (expected, 1) + " +-0.1)");
    }

    void range (const juce::String& name, const juce::AudioBuffer<float>& b, double rate, double expected)
    {
        const auto r = Loudness::measure (b, rate);
        check (near (r.range, expected, 1.0), name + " has range " + juce::String (r.range, 2) + " LU (EBU: "
                                                  + juce::String (expected, 0) + " +-1)");
    }
}

namespace
{
    /** Noise with a gentle low tilt, ten seconds of it, at this peak level; with clicks to full
        scale every second when `peaky`. */
    juce::AudioBuffer<float> programme (double rate, double gain, bool peaky)
    {
        juce::Random r (17);
        juce::AudioBuffer<float> b (2, (int) (10.0 * rate));
        float last[2] {};

        for (int i = 0; i < b.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch)
            {
                last[ch] = 0.7f * last[ch] + 0.3f * (r.nextFloat() * 2.0f - 1.0f);
                b.setSample (ch, i, (float) gain * last[ch]);
            }

        if (peaky)
            for (int i = (int) rate / 2; i < b.getNumSamples(); i += (int) rate)
                for (int k = 0; k < 24; ++k)
                    for (int ch = 0; ch < 2; ++ch)
                        b.setSample (ch, i + k, (k % 2 == 0 ? 0.98f : -0.98f));

        return b;
    }

    void testDelivery()
    {
        constexpr double rate = 48000.0;
        const auto& specs = Delivery::specs();
        const auto& r128 = specs[0];
        const auto& online14 = specs[3];

        // The specs and the checks at their edges.
        Loudness::Result edge;
        edge.integrated = -22.5;
        edge.truePeak = -1.0;
        auto over = edge;
        over.integrated = -22.4;
        auto peaked = edge;
        peaked.truePeak = -0.9;
        check (specs.size() == 4 && r128.name == "EBU R128" && juce::exactlyEqual (r128.targetLufs, -23.0) && juce::exactlyEqual (r128.toleranceLu, 0.5)
                   && juce::exactlyEqual (r128.maxTruePeak, -1.0) && juce::exactlyEqual (specs[1].targetLufs, -24.0) && juce::exactlyEqual (specs[1].maxTruePeak, -2.0)
                   && Delivery::passes (r128, edge) && ! Delivery::passes (r128, over) && ! Delivery::passes (r128, peaked)
                   && ! Delivery::check (r128, peaked)[1].pass && Delivery::check (r128, peaked)[0].pass,
               "delivery: EBU R128 is -23 LUFS +/- 0.5 LU at most -1 dBTP and ATSC A/85 -24 at most -2 dBTP; -22.5 LUFS at -1.0 dBTP passes, "
               "-22.4 LUFS or -0.9 dBTP fails on that line alone");

        // A quiet programme: gain alone meets R128.
        const auto quiet = AudioEdits::fromBuffer (programme (rate, 0.05, false));
        const auto d = Delivery::deliver (*quiet, 0, quiet->getNumFrames(), rate, r128);
        const auto text = Delivery::report (r128, d, "quiet.wav");
        check (d.snapshot != nullptr && ! d.limited && ! Delivery::passes (r128, d.before) && Delivery::passes (r128, d.after)
                   && std::abs (d.after.integrated + 23.0) < 0.01 && text.contains ("PASS  Integrated loudness") && text.contains ("PASS  True peak")
                   && text.contains ("Meets EBU R128.") && text.contains ("before " + Loudness::format (d.before.integrated, "LUFS")),
               "delivery: a quiet programme at " + Loudness::format (d.before.integrated, "LUFS") + " meets EBU R128 with "
                   + juce::String (d.gainDb, 2) + " dB of gain alone: " + Loudness::format (d.after.integrated, "LUFS") + ", true peak "
                   + Loudness::format (d.after.truePeak, "dBTP") + ", and the report says so line by line");

        // A peaky one: to -14 LUFS the gain would pass -1 dBTP, so it is limited, and still meets it.
        const auto loud = AudioEdits::fromBuffer (programme (rate, 0.12, true));
        const auto l = Delivery::deliver (*loud, 0, loud->getNumFrames(), rate, online14);
        check (l.snapshot != nullptr && l.limited && l.before.truePeak + (online14.targetLufs - l.before.integrated) > online14.maxTruePeak
                   && Delivery::passes (online14, l.after) && Delivery::report (online14, l, "loud.wav").contains ("true-peak limiter"),
               "delivery: a programme with clicks to full scale, at " + Loudness::format (l.before.integrated, "LUFS") + " and "
                   + Loudness::format (l.before.truePeak, "dBTP") + ", meets -14 LUFS through the true-peak limiter: "
                   + Loudness::format (l.after.integrated, "LUFS") + ", true peak " + Loudness::format (l.after.truePeak, "dBTP"));

        // Only the range given changes; silence is refused.
        const auto from = (juce::int64) (2.0 * rate), to = (juce::int64) (7.0 * rate);
        const auto part = Delivery::deliver (*quiet, from, to, rate, r128);
        juce::AudioBuffer<float> was (2, (int) quiet->getNumFrames()), now (2, (int) quiet->getNumFrames());
        quiet->read (was, 0, 0, was.getNumSamples());
        part.snapshot->read (now, 0, 0, now.getNumSamples());
        auto outsideSame = true;

        for (int ch = 0; ch < 2; ++ch)
            outsideSame = outsideSame && std::memcmp (was.getReadPointer (ch), now.getReadPointer (ch), sizeof (float) * (size_t) from) == 0
                          && std::memcmp (was.getReadPointer (ch, (int) to), now.getReadPointer (ch, (int) to), sizeof (float) * (size_t) (was.getNumSamples() - to)) == 0;

        juce::AudioBuffer<float> zeros (2, (int) rate);
        zeros.clear();
        const auto silent = AudioEdits::fromBuffer (std::move (zeros));
        const auto none = Delivery::deliver (*silent, 0, silent->getNumFrames(), rate, r128);
        check (outsideSame && Delivery::passes (r128, part.after) && none.snapshot == nullptr && none.error.contains ("-70 LUFS"),
               "delivery: on a selection only it changes, the rest bit for bit as it was; silence is refused, with nothing above the gate");
    }
}

void runLoudnessTests()
{
    testDelivery();
    // EBU Tech 3341, cases 1-5, at 48 kHz and again at 44.1 kHz (the K-weighting is derived per rate).
    for (auto rate : { 48000.0, 44100.0 })
    {
        const auto at = " at " + juce::String (rate / 1000.0, 1) + " kHz";
        integrated ("loudness: 3341-1, stereo -23 dBFS" + at, tone ({ { 20, -23 } }, rate), rate, -23.0);
        integrated ("loudness: 3341-2, stereo -33 dBFS" + at, tone ({ { 20, -33 } }, rate), rate, -33.0);
        integrated ("loudness: 3341-3, -36/-23/-36 (relative gate)" + at, tone ({ { 10, -36 }, { 60, -23 }, { 10, -36 } }, rate), rate, -23.0);
        integrated ("loudness: 3341-4, -72/-36/-23/-36/-72 (absolute gate)" + at,
                    tone ({ { 10, -72 }, { 10, -36 }, { 60, -23 }, { 10, -36 }, { 10, -72 } }, rate), rate, -23.0);
        integrated ("loudness: 3341-5, -26/-20/-26" + at, tone ({ { 20, -26 }, { 20.1, -20 }, { 20, -26 } }, rate), rate, -23.0);
    }

    // BS.1770: a 0 dBFS 1 kHz sine in one channel reads -3.01 LKFS.
    {
        auto b = tone ({ { 10, 0 } }, 48000.0);
        b.clear (1, 0, b.getNumSamples());
        integrated ("loudness: 0 dBFS sine in one channel", b, 48000.0, -3.01);
    }

    // 5.1: the LFE is left out, the surrounds count 1.41 times (+1.5 dB).
    {
        auto lfeOnly = tone ({ { 10, -20 } }, 48000.0, 6);

        for (int ch : { 0, 1, 2, 4, 5 })
            lfeOnly.clear (ch, 0, lfeOnly.getNumSamples());

        check (! std::isfinite (Loudness::measure (lfeOnly, 48000.0).integrated), "loudness: 5.1 LFE alone does not count");

        auto surround = tone ({ { 10, -20 } }, 48000.0, 6);

        for (int ch : { 0, 1, 2, 3, 5 })
            surround.clear (ch, 0, surround.getNumSamples());

        integrated ("loudness: 5.1 left surround alone is weighted +1.5 dB", surround, 48000.0, -23.0 + 10.0 * std::log10 (1.41));
    }

    {
        juce::AudioBuffer<float> silence (2, 48000 * 5);
        silence.clear();
        check (! std::isfinite (Loudness::measure (silence, 48000.0).integrated), "loudness: silence reads -inf");
    }

    // Momentary and short-term maxima of a steady tone equal its loudness.
    {
        const auto r = Loudness::measure (tone ({ { 10, -23 } }, 48000.0), 48000.0);
        check (near (r.maxMomentary, -23.0, 0.1) && near (r.maxShortTerm, -23.0, 0.1),
               "loudness: momentary " + Loudness::format (r.maxMomentary, "LUFS") + ", short-term "
                   + Loudness::format (r.maxShortTerm, "LUFS") + " for a steady -23 tone");
    }

    // EBU Tech 3342, cases 1-4.
    range ("loudness range: 3342-1, -20/-30", tone ({ { 20, -20 }, { 20, -30 } }, 48000.0), 48000.0, 10.0);
    range ("loudness range: 3342-2, -20/-15", tone ({ { 20, -20 }, { 20, -15 } }, 48000.0), 48000.0, 5.0);
    range ("loudness range: 3342-3, -40/-20", tone ({ { 20, -40 }, { 20, -20 } }, 48000.0), 48000.0, 20.0);
    range ("loudness range: 3342-4, -50/-35/-20/-35/-50",
           tone ({ { 20, -50 }, { 20, -35 }, { 20, -20 }, { 20, -35 }, { 20, -50 } }, 48000.0), 48000.0, 15.0);

    // True peak: a quarter-rate sine sampled 45 degrees off its crests peaks 3 dB above its samples.
    {
        const auto r = Loudness::measure (faded (tone ({ { 2, -6 } }, 48000.0, 2, 12000.0, juce::MathConstants<double>::pi / 4.0), 48000.0), 48000.0);
        check (near (r.samplePeak, -9.03, 0.05), "true peak: sample peak " + Loudness::format (r.samplePeak, "dBFS") + " (expected -9.03)");
        check (near (r.truePeak, -6.02, 0.05), "true peak: " + juce::String (r.truePeak, 2) + " dBTP between the samples (expected -6.02 +-0.05)");
    }

    // A whole-sample-peak signal: true peak is never below the sample peak.
    {
        const auto r = Loudness::measure (faded (tone ({ { 2, -1 } }, 48000.0), 48000.0), 48000.0);
        check (r.truePeak >= r.samplePeak && near (r.truePeak, -1.0, 0.02),
               "true peak: " + juce::String (r.truePeak, 2) + " dBTP for a -1 dBFS 1 kHz sine");
    }

    // A range of a snapshot is measured as if it were a file of its own.
    {
        const auto b = tone ({ { 10, -36 }, { 10, -23 }, { 10, -36 } }, 48000.0);
        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (juce::AudioBuffer<float> (b)), 0, b.getNumSamples() } };
        const AudioSnapshot snapshot (2, std::move (segments));
        const auto r = Loudness::measure (snapshot, 480000, 960000, 48000.0);
        check (r.has_value() && near (r->integrated, -23.0, 0.1), "loudness: the middle 10 s of a file reads its own level");

        int calls = 0;
        const auto cancelled = Loudness::measure (snapshot, 0, b.getNumSamples(), 48000.0, [&] (float) { return ++calls < 2; });
        check (! cancelled.has_value(), "loudness: measuring can be cancelled");
    }
}
