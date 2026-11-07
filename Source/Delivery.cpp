#include "Delivery.h"
#include "Dynamics.h"

namespace Delivery
{
const std::vector<Spec>& specs()
{
    static const std::vector<Spec> all {
        { "EBU R128", -23.0, 0.5, -1.0 },
        { "ATSC A/85", -24.0, 2.0, -2.0 },
        { "Online -16 LUFS", -16.0, 1.0, -1.0 },
        { "Online -14 LUFS", -14.0, 1.0, -1.0 },
    };
    return all;
}

Spec specFor (double targetLufs, double toleranceLu, double maxTruePeak)
{
    for (auto& spec : specs())
        if (juce::exactlyEqual (spec.targetLufs, targetLufs) && juce::exactlyEqual (spec.toleranceLu, toleranceLu)
            && juce::exactlyEqual (spec.maxTruePeak, maxTruePeak))
            return spec;

    return { "Delivery spec " + Loudness::format (targetLufs, "LUFS"), targetLufs, toleranceLu, maxTruePeak };
}

bool isValid (double targetLufs, double toleranceLu, double maxTruePeak)
{
    return targetLufs >= -70.0 && targetLufs <= 0.0 && toleranceLu >= 0.1 && toleranceLu <= 10.0 && maxTruePeak >= -30.0 && maxTruePeak <= 0.0;
}

std::vector<Line> check (const Spec& spec, const Loudness::Result& r)
{
    std::vector<Line> lines;
    const auto integratedPasses = std::isfinite (r.integrated) && std::abs (r.integrated - spec.targetLufs) <= spec.toleranceLu + 1.0e-9;
    lines.push_back ({ "Integrated loudness", Loudness::format (r.integrated, "LUFS"),
                       Loudness::format (spec.targetLufs, "LUFS") + " +/- " + juce::String (spec.toleranceLu, 1) + " LU", integratedPasses });
    lines.push_back ({ "True peak", Loudness::format (r.truePeak, "dBTP"), "at most " + Loudness::format (spec.maxTruePeak, "dBTP"),
                       ! (r.truePeak > spec.maxTruePeak + 1.0e-9) });
    lines.push_back ({ "Loudness range", juce::String (r.range, 1) + " LU", {}, true });
    lines.push_back ({ "Loudest momentary", Loudness::format (r.maxMomentary, "LUFS"), {}, true });
    lines.push_back ({ "Loudest short-term", Loudness::format (r.maxShortTerm, "LUFS"), {}, true });
    lines.push_back ({ "Sample peak", Loudness::format (r.samplePeak, "dBFS"), {}, true });
    return lines;
}

juce::String summary (const Spec& spec, const Loudness::Result& after)
{
    return juce::String (passes (spec, after) ? "meets it at " : "does not meet it, at ") + Loudness::format (after.integrated, "LUFS")
           + ", true peak " + Loudness::format (after.truePeak, "dBTP");
}

bool passes (const Spec& spec, const Loudness::Result& r)
{
    const auto lines = check (spec, r);
    return std::all_of (lines.begin(), lines.end(), [] (auto& l) { return l.pass; });
}

Delivered deliver (const AudioSnapshot& audio, juce::int64 start, juce::int64 end, double sampleRate, const Spec& spec,
                   const std::function<bool (float)>& progress)
{
    Delivered d;
    const auto half = [&progress] (float from)
    {
        return [&progress, from] (float p) { return progress == nullptr || progress (from + 0.5f * p); };
    };

    const auto before = Loudness::measure (audio, start, end, sampleRate, half (0.0f));

    if (! before.has_value())
        return d;

    d.before = *before;

    if (! std::isfinite (before->integrated))
    {
        d.error = "There is nothing above the -70 LUFS gate to measure.";
        return d;
    }

    // Gain alone where the true peak stays under the ceiling; otherwise into a limiter just under it.
    const auto wanted = spec.targetLufs - before->integrated;

    if (before->truePeak + wanted <= spec.maxTruePeak)
    {
        d.snapshot = AudioEdits::gain (audio, start, end, (float) wanted);
        d.gainDb = wanted;
    }
    else
    {
        auto matched = Dynamics::matchLoudness (audio, start, end, sampleRate, before->integrated, spec.targetLufs, spec.maxTruePeak - 0.1);
        d.snapshot = std::move (matched.snapshot);
        d.gainDb = matched.gainDb;
        d.limited = true;
    }

    if (d.snapshot == nullptr)
    {
        d.error = "The loudness could not be changed.";
        return d;
    }

    const auto after = Loudness::measure (*d.snapshot, start, end, sampleRate, half (0.5f));

    if (! after.has_value())
    {
        d.snapshot = nullptr;
        return d;
    }

    d.after = *after;
    return d;
}

Stems planStems (const Spec& spec, const juce::AudioBuffer<float>& mix, double sampleRate, int stems, const juce::String& title)
{
    Stems plan;
    Delivered d;
    d.before = Loudness::measure (mix, sampleRate);

    if (! std::isfinite (d.before.integrated))
    {
        plan.error = "The mix has nothing above the -70 LUFS gate to measure.";
        return plan;
    }

    d.gainDb = plan.gainDb = spec.targetLufs - d.before.integrated;
    juce::AudioBuffer<float> gained (mix);
    gained.applyGain ((float) juce::Decibels::decibelsToGain (plan.gainDb, -1000.0));
    d.after = Loudness::measure (gained, sampleRate);

    plan.report = report (spec, d, title).replace ("Gain: ", juce::String (stems) + " stems, each with the same gain: ", false);

    if (d.after.truePeak > spec.maxTruePeak)
        plan.report << "\nThe mix's true peak passes the ceiling: it would need limiting, which the stems can't carry and still add up to the mix.";

    return plan;
}

juce::String report (const Spec& spec, const Delivered& d, const juce::String& title)
{
    juce::StringArray out;
    out.add (title);
    out.add ("Delivery spec: " + spec.name + " (" + Loudness::format (spec.targetLufs, "LUFS") + " +/- " + juce::String (spec.toleranceLu, 1)
             + " LU, true peak at most " + Loudness::format (spec.maxTruePeak, "dBTP") + ")");
    out.add (juce::String (d.gainDb >= 0.0 ? "Gain: +" : "Gain: ") + juce::String (d.gainDb, 2) + " dB"
             + (d.limited ? ", into a true-peak limiter at " + Loudness::format (spec.maxTruePeak - 0.1, "dBTP") : juce::String()));
    out.add ({});

    const auto now = check (spec, d.after), was = check (spec, d.before);

    for (size_t i = 0; i < now.size(); ++i)
    {
        const auto& l = now[i];
        out.add ((l.required.isEmpty() ? juce::String ("      ") : juce::String (l.pass ? "PASS  " : "FAIL  ")) + l.what + ": " + l.measured
                 + (l.required.isNotEmpty() ? " (" + l.required + ")" : juce::String()) + "; before " + was[i].measured);
    }

    out.add ({});
    out.add (passes (spec, d.after) ? "Meets " + spec.name + "." : "Does not meet " + spec.name + ".");
    return out.joinIntoString ("\n");
}
}
