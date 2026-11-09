#include "Sequences.h"
#include "Delivery.h"
#include "TimeStretch.h"
#include "ExtremeStretch.h"
#include "AudioEdits.h"
#include "CentreChannel.h"
#include "PhaseCorrection.h"
#include "PitchCorrection.h"
#include "Dereverb.h"
#include "Leveller.h"
#include "Dynamics.h"
#include "Equalizer.h"
#include "Loudness.h"
#include "Modulation.h"
#include "Multiband.h"
#include "Exciter.h"
#include "Maximizer.h"
#include "TransientShaper.h"
#include "Dither.h"
#include "Plosive.h"
#include "Breath.h"
#include "OfflineRender.h"
#include "Reverb.h"
#include "SampleRateConverter.h"

namespace Sequences
{
namespace
{
    using Snapshot = AudioDocument::Snapshot;

    double get (const Values& v, const char* id)
    {
        const auto it = v.find (id);
        return it != v.end() ? it->second : 0.0;
    }

    struct Kind
    {
        const char* id;
        const char* name;
        std::vector<const char*> values;   // the settings it needs (an optional one, "method", "linearPhase" or "speed", takes its default when missing)
    };

    // In menu order. The names are the commands' undo names.
    const std::vector<Kind>& table()
    {
        static const std::vector<Kind> list {
            { "gain", "Gain", { "db" } },
            { "normalize", "Normalize", { "db", "apart" } },
            { "matchLoudness", "Match Loudness", { "target", "ceiling", "limit" } },
            { "deliverySpec", "Meet Delivery Spec", { "target", "tolerance", "ceiling" } },
            { "invert", "Invert", {} },
            { "reverse", "Reverse", {} },
            { "removeDc", "Remove DC Offset", {} },
            { "swapChannels", "Swap Channels", {} },
            { "invertLeft", "Invert Left Channel", {} },
            { "invertRight", "Invert Right Channel", {} },
            { "leftToRight", "Copy Left to Right", {} },
            { "rightToLeft", "Copy Right to Left", {} },
            { "mono", "Mix to Mono", {} },
            { "stereo", "Make Stereo", {} },
            { "balance", "Balance", { "pan" } },
            { "stereoWidth", "Stereo Width", { "width" } },
            { "multibandWidth", "Multiband Stereo Width", { "lowCrossover", "highCrossover", "low", "mid", "high" } },
            { "maximizer", "Maximizer", { "gain", "ceiling", "character", "emphasis" } },
            { "dither", "Dither", { "bits", "shaping" } },
            { "deplosive", "De-plosive", { "corner", "threshold", "reduction" } },
            { "breath", "Breath Control", { "threshold", "reduction" } },
            { "transientShaper", "Transient Shaper", { "lowCrossover", "highCrossover", "lowAttack", "lowSustain", "midAttack", "midSustain", "highAttack", "highSustain", "output" } },
            { "exciter", "Exciter", { "lowCrossover", "highCrossover", "lowDrive", "lowCurve", "lowMix", "midDrive", "midCurve", "midMix", "highDrive", "highCurve", "highMix", "output" } },
            { "centre", "Centre Channel", { "keep", "low", "high", "reduction" } },
            { "phase", "Automatic Phase Correction", { "most", "turnOver" } },
            { "echo", "Echo", { "delay", "feedback" } },
            { "tremolo", "Tremolo", { "rate", "depth" } },
            { "eqBand", "EQ Band", { "type", "frequency", "gain", "q" } },
            { "compressor", "Compressor", { "threshold", "ratio", "knee", "attack", "release", "makeup" } },
            { "limiter", "Limiter", { "gain", "ceiling", "release", "lookahead", "peaks" } },
            { "deEsser", "De-esser", { "frequency", "threshold", "reduction", "mode" } },
            { "chorus", "Chorus", { "rate", "delay", "depth", "feedback", "mix", "spread" } },
            { "flanger", "Flanger", { "rate", "delay", "depth", "feedback", "mix", "spread" } },
            { "phaser", "Phaser", { "rate", "low", "high", "stages", "feedback", "mix", "spread" } },
            { "reverb", "Reverb", { "preDelay", "decay", "size", "damping", "width", "mix" } },
            { "dereverb", "Reduce Reverb", { "time", "reduction" } },
            { "levelSpeech", "Level Speech", { "target", "boost", "cut", "silence" } },
            { "stretch", "Stretch", { "percent" } },
            { "extremeStretch", "Extreme Stretch", { "factor", "window" } },
            { "pitch", "Shift Pitch", { "semitones" } },
            { "pitchCorrect", "Pitch Correction", { "key", "scale", "speed", "strength" } },
            { "convertRate", "Convert Sample Rate", { "rate" } },
            { "rack", "Apply Effects Rack", {} },
        };
        return list;
    }

    const Kind* find (const juce::String& id)
    {
        for (auto& k : table())
            if (id == k.id)
                return &k;

        return nullptr;
    }

    Dynamics::Compressor compressorOf (const Values& v)
    {
        return { get (v, "threshold"), get (v, "ratio"), get (v, "knee"), get (v, "attack"), get (v, "release"), get (v, "makeup") };
    }

    Dynamics::Limiter limiterOf (const Values& v)
    {
        return { get (v, "gain"), get (v, "ceiling"), get (v, "release"), get (v, "lookahead"), get (v, "peaks") > 0.5 };
    }

    // "linearPhase" on the band-split steps is optional: 1 for the split in linear phase, else minimum phase.
    Multiband::Width widthOf (const Values& v)
    {
        return { get (v, "lowCrossover"), get (v, "highCrossover"), { get (v, "low"), get (v, "mid"), get (v, "high") }, get (v, "linearPhase") > 0.5 };
    }

    TransientShaper::Settings transientShaperOf (const Values& v)
    {
        TransientShaper::Settings s;
        s.lowCrossover = get (v, "lowCrossover");
        s.highCrossover = get (v, "highCrossover");
        s.outputDb = get (v, "output");
        s.linearPhase = get (v, "linearPhase") > 0.5;
        const std::array<const char*, 3> ids { "low", "mid", "high" };

        for (size_t b = 0; b < 3; ++b)
        {
            const juce::String id (ids[b]);
            s.bands[b] = { get (v, (id + "Attack").toRawUTF8()), get (v, (id + "Sustain").toRawUTF8()) };
        }

        return s;
    }

    Breath::Settings breathOf (const Values& v)
    {
        return { (float) get (v, "threshold"), (float) get (v, "reduction") };
    }

    Plosive::Settings plosiveOf (const Values& v)
    {
        return { get (v, "corner"), get (v, "threshold"), get (v, "reduction") };
    }

    Dither::Settings ditherOf (const Values& v)
    {
        return { juce::roundToInt (get (v, "bits")) == 1 ? 24 : 16, (Dither::Shaping) juce::jlimit (0, 2, juce::roundToInt (get (v, "shaping"))) };
    }

    Maximizer::Settings maximizerOf (const Values& v)
    {
        return { get (v, "gain"), get (v, "ceiling"), (Maximizer::Character) juce::jlimit (0, 2, juce::roundToInt (get (v, "character"))), get (v, "emphasis") };
    }

    Exciter::Settings exciterOf (const Values& v)
    {
        Exciter::Settings s;
        s.lowCrossover = get (v, "lowCrossover");
        s.highCrossover = get (v, "highCrossover");
        s.outputDb = get (v, "output");
        s.linearPhase = get (v, "linearPhase") > 0.5;
        const std::array<const char*, 3> ids { "low", "mid", "high" };

        for (size_t b = 0; b < 3; ++b)
        {
            const juce::String id (ids[b]);
            s.bands[b] = { get (v, (id + "Drive").toRawUTF8()), (Exciter::Curve) juce::jlimit (0, 2, juce::roundToInt (get (v, (id + "Curve").toRawUTF8()))),
                           get (v, (id + "Mix").toRawUTF8()) };
        }

        return s;
    }

    Multiband::DeEsser deEsserOf (const Values& v)
    {
        return { get (v, "frequency"), get (v, "threshold"), get (v, "reduction"), juce::roundToInt (get (v, "mode")) == 1, get (v, "linearPhase") > 0.5 };
    }

    Modulation::Delay delayOf (const Values& v)
    {
        return { get (v, "rate"), get (v, "delay"), get (v, "depth"), get (v, "feedback"), get (v, "mix"), get (v, "spread") };
    }

    Modulation::Phaser phaserOf (const Values& v)
    {
        return { get (v, "rate"), get (v, "low"), get (v, "high"), 2 * ((int) std::lround (get (v, "stages")) + 1),
                 get (v, "feedback"), get (v, "mix"), get (v, "spread") };
    }

    CentreChannel::Settings centreOf (const Values& v)
    {
        return { get (v, "keep") > 0.5, (float) get (v, "low"), (float) get (v, "high"), (float) get (v, "reduction") };
    }

    PitchCorrection::Settings tuningOf (const Values& v)
    {
        return { (int) std::lround (get (v, "key")), (PitchCorrection::Scale) juce::jlimit (0, 2, (int) std::lround (get (v, "scale"))),
                 (float) get (v, "speed"), (float) get (v, "strength") };
    }

    // "speed" (the fastest change, dB a second) is optional: a step without it moves as fast as the smoothing lets it.
    Leveller::Settings levellerOf (const Values& v)
    {
        Leveller::Settings s { (float) get (v, "target"), (float) get (v, "boost"), (float) get (v, "cut"), (float) get (v, "silence") };

        if (v.count ("speed") > 0)
            s.speedDbPerSecond = (float) get (v, "speed");

        return s;
    }

    RoomReverb::Settings reverbOf (const Values& v)
    {
        return { get (v, "preDelay"), get (v, "decay"), get (v, "size"), get (v, "damping"), get (v, "width"), get (v, "mix") };
    }

    Equalizer::Band bandOf (const Values& v)
    {
        return { (Equalizer::BandType) juce::jlimit (0, 4, (int) get (v, "type")), get (v, "frequency"), get (v, "gain"), get (v, "q") };
    }

    juce::String number (double x)
    {
        auto s = juce::String (x, 3);

        if (s.containsChar ('.'))
            s = s.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");

        return s == "-0" ? juce::String ("0") : s;
    }
}

const juce::StringArray& kinds()
{
    static const juce::StringArray list = []
    {
        juce::StringArray ids;

        for (auto& k : table())
            ids.add (k.id);

        return ids;
    }();
    return list;
}

juce::String Step::name() const
{
    if (kind == "gain")
        return "Gain " + juce::String ((float) get (values, "db"), 2) + " dB";

    if (kind == "matchLoudness")
        return "Match Loudness " + Loudness::format (get (values, "target"), "LUFS");

    if (kind == "deliverySpec")
        return "Meet " + Delivery::specFor (get (values, "target"), get (values, "tolerance"), get (values, "ceiling")).name;

    if (kind == "convertRate")
        return "Convert to " + juce::String (get (values, "rate") / 1000.0, 1) + " kHz";

    if (kind == "levelSpeech")
        return "Level Speech to " + Loudness::format (get (values, "target"), "LUFS");

    if (kind == "centre")
        return juce::String (get (values, "keep") > 0.5 ? "Extract" : "Remove") + " Centre " + juce::String (get (values, "reduction"), 1) + " dB";

    if (kind == "dereverb")
        return "Reduce Reverb " + juce::String (get (values, "reduction"), 1) + " dB";

    // SpectraBlade's own stretch is named; Signalsmith Stretch, the default, is not.
    const auto method = juce::String (get (values, "method") > 0.5 ? " (SpectraBlade Stretch)" : "");

    if (kind == "stretch")
        return "Stretch to " + juce::String (get (values, "percent"), 1) + "%" + method;

    if (kind == "extremeStretch")
        return "Extreme Stretch " + juce::String (get (values, "factor"), 1) + "x";

    if (kind == "pitch")
    {
        const auto semitones = get (values, "semitones");
        return "Shift Pitch " + juce::String (semitones > 0.0 ? "+" : "") + juce::String (semitones, 2) + " Semitones" + method;
    }

    const auto* k = find (kind);
    return k != nullptr ? juce::String (k->name) : kind;
}

juce::String Step::settingsText() const
{
    if (kind == "rack")
    {
        const auto slots = rack != nullptr ? rack->getNumChildElements() : 0;
        return juce::String (slots) + (slots == 1 ? " effect" : " effects");
    }

    juce::StringArray parts;

    if (const auto* k = find (kind))
        for (auto* id : k->values)
            parts.add (juce::String (id) + " " + number (get (values, id)));

    return parts.joinIntoString (", ");
}

juce::String problem (const Step& step, double rate)
{
    const auto* k = find (step.kind);

    if (k == nullptr)
        return "\"" + step.kind + "\" isn't a step this version knows.";

    for (auto* id : k->values)
        if (step.values.count (id) == 0 || ! std::isfinite (step.values.at (id)))
            return "it has no " + juce::String (id) + " setting.";

    const auto& v = step.values;
    const auto bad = [] { return juce::String ("a setting is out of range."); };

    if (step.kind == "rack" && step.rack == nullptr)
        return "it has no rack.";

    if (step.kind == "convertRate" && (get (v, "rate") < 8000.0 || get (v, "rate") > 384000.0))
        return bad();

    if (step.kind == "extremeStretch" && ! ExtremeStretch::isValid ({ get (v, "factor"), get (v, "window") }))
        return bad();

    if ((step.kind == "stretch" && (get (v, "percent") < TimeStretch::shortest * 100.0 || get (v, "percent") > TimeStretch::longest * 100.0))
        || (step.kind == "pitch" && std::abs (get (v, "semitones")) > TimeStretch::widestShift)
        || ((step.kind == "stretch" || step.kind == "pitch") && ! juce::exactlyEqual (get (v, "method"), 0.0) && ! juce::exactlyEqual (get (v, "method"), 1.0)))
        return bad();

    if ((step.kind == "deEsser" || step.kind == "multibandWidth" || step.kind == "exciter" || step.kind == "transientShaper")
        && ! juce::exactlyEqual (get (v, "linearPhase"), 0.0) && ! juce::exactlyEqual (get (v, "linearPhase"), 1.0))
        return bad();

    if (step.kind == "deliverySpec" && ! Delivery::isValid (get (v, "target"), get (v, "tolerance"), get (v, "ceiling")))
        return bad();

    if (step.kind == "matchLoudness" && (get (v, "target") < -70.0 || get (v, "target") > 0.0 || get (v, "ceiling") < -30.0 || get (v, "ceiling") > 0.0))
        return bad();

    if ((step.kind == "balance" && std::abs (get (v, "pan")) > 100.0) || (step.kind == "stereoWidth" && (get (v, "width") < 0.0 || get (v, "width") > 200.0))
        || (step.kind == "echo" && (get (v, "delay") < 1.0 || get (v, "delay") > 5000.0 || get (v, "feedback") < 0.0 || get (v, "feedback") > 95.0))
        || (step.kind == "tremolo" && (get (v, "rate") < 0.1 || get (v, "rate") > 50.0 || get (v, "depth") < 0.0 || get (v, "depth") > 100.0))
        || (step.kind == "eqBand" && ! Equalizer::isValid (bandOf (v), rate))
        || (step.kind == "compressor" && ! Dynamics::isValid (compressorOf (v)))
        || (step.kind == "limiter" && ! Dynamics::isValid (limiterOf (v)))
        || (step.kind == "deEsser" && ! Multiband::isValid (deEsserOf (v), rate))
        || (step.kind == "multibandWidth" && ! Multiband::isValid (widthOf (v), rate))
        || (step.kind == "exciter" && ! Exciter::isValid (exciterOf (v), rate))
        || (step.kind == "maximizer" && ! Maximizer::isValid (maximizerOf (v)))
        || (step.kind == "dither" && (get (v, "bits") < 0.0 || get (v, "bits") > 1.0))
        || (step.kind == "deplosive" && ! Plosive::isValid (plosiveOf (v), rate))
        || (step.kind == "breath" && ! Breath::isValid (breathOf (v)))
        || (step.kind == "transientShaper" && ! TransientShaper::isValid (transientShaperOf (v), rate))
        || ((step.kind == "chorus" || step.kind == "flanger") && ! Modulation::isValid (delayOf (v)))
        || (step.kind == "phaser" && ! Modulation::isValid (phaserOf (v)))
        || (step.kind == "reverb" && ! RoomReverb::isValid (reverbOf (v)))
        || (step.kind == "levelSpeech" && ! Leveller::isValid (levellerOf (v)))
        || (step.kind == "centre" && ! CentreChannel::isValid (centreOf (v)))
        || (step.kind == "pitchCorrect" && (get (v, "scale") < 0.0 || get (v, "scale") > 2.0 || ! PitchCorrection::isValid (tuningOf (v))))
        || (step.kind == "phase" && (get (v, "most") < PhaseCorrection::shortestMs || get (v, "most") > PhaseCorrection::longestMs))
        || (step.kind == "dereverb" && ! Dereverb::isValid ({ (float) get (v, "time"), (float) get (v, "reduction") })))
        return bad();

    return {};
}

Result apply (const Step& step, const Snapshot& audio, juce::int64 start, juce::int64 end, double rate,
              const Markers& markers, juce::AudioPluginFormatManager* plugins, const std::function<bool (float)>& progress)
{
    Result out;
    out.sampleRate = rate;
    out.markers = markers;
    start = juce::jlimit ((juce::int64) 0, audio->getNumFrames(), start);
    end = juce::jlimit (start, audio->getNumFrames(), end);
    out.start = start;
    out.end = end;

    if (out.error = problem (step, rate); out.error.isNotEmpty())
        return out;

    const auto& s = *audio;
    const auto& v = step.values;
    const auto& k = step.kind;

    if (k == "gain")                out.snapshot = AudioEdits::gain (s, start, end, (float) get (v, "db"));
    else if (k == "invert")         out.snapshot = AudioEdits::invert (s, start, end);
    else if (k == "reverse")        out.snapshot = AudioEdits::reverse (s, start, end);
    else if (k == "removeDc")       out.snapshot = AudioEdits::removeDcOffset (s, start, end);
    else if (k == "swapChannels")   out.snapshot = AudioEdits::swapChannels (s, start, end);
    else if (k == "invertLeft")     out.snapshot = AudioEdits::invertChannel (s, start, end, 0);
    else if (k == "invertRight")    out.snapshot = AudioEdits::invertChannel (s, start, end, 1);
    else if (k == "leftToRight")    out.snapshot = AudioEdits::copyChannel (s, start, end, 0, 1);
    else if (k == "rightToLeft")    out.snapshot = AudioEdits::copyChannel (s, start, end, 1, 0);
    else if (k == "eqBand")         out.snapshot = Equalizer::apply (s, start, end, rate, { bandOf (v) });
    else if (k == "compressor")     out.snapshot = Dynamics::compress (s, start, end, rate, compressorOf (v));
    else if (k == "limiter")        out.snapshot = Dynamics::limit (s, start, end, rate, limiterOf (v));
    else if (k == "deEsser")        out.snapshot = Multiband::deEss (s, start, end, rate, deEsserOf (v));
    else if (k == "multibandWidth") out.snapshot = Multiband::widen (s, start, end, rate, widthOf (v));
    else if (k == "exciter")        out.snapshot = Exciter::apply (s, start, end, rate, exciterOf (v));
    else if (k == "maximizer")      out.snapshot = Maximizer::apply (s, start, end, rate, maximizerOf (v));
    else if (k == "dither")         out.snapshot = Dither::apply (s, start, end, ditherOf (v));
    else if (k == "deplosive")      out.snapshot = Plosive::apply (s, start, end, rate, plosiveOf (v));
    else if (k == "breath")         out.snapshot = Breath::apply (s, start, end, rate, breathOf (v));
    else if (k == "transientShaper") out.snapshot = TransientShaper::apply (s, start, end, rate, transientShaperOf (v));
    else if (k == "chorus" || k == "flanger") out.snapshot = Modulation::sweptDelay (s, start, end, rate, delayOf (v));
    else if (k == "phaser")         out.snapshot = Modulation::phaser (s, start, end, rate, phaserOf (v));
    else if (k == "reverb")         out.snapshot = RoomReverb::apply (s, start, end, rate, reverbOf (v));
    else if (k == "levelSpeech")
    {
        if (end > start)
        {
            out.snapshot = Leveller::level (s, start, end, rate, levellerOf (v), progress);

            if (out.snapshot == nullptr)
            {
                out.error = "Stopped.";
                return out;
            }
        }
    }
    else if (k == "centre")
    {
        if (s.getNumChannels() != 2)
        {
            out.notes.add ("not stereo, so its centre wasn't changed");
        }
        else if (end > start && get (v, "reduction") > 0.0)
        {
            out.snapshot = CentreChannel::process (s, start, end, rate, centreOf (v), progress);

            if (out.snapshot == nullptr)
            {
                out.error = "Stopped.";
                return out;
            }
        }
    }
    else if (k == "pitchCorrect")
    {
        if (end > start && get (v, "strength") > 0.0)
        {
            out.snapshot = PitchCorrection::correct (s, start, end, rate, tuningOf (v), progress);

            if (out.snapshot == nullptr)
            {
                out.error = "Stopped.";
                return out;
            }
        }
    }
    else if (k == "phase")
    {
        const auto most = get (v, "most");
        const auto ms = [rate] (double frames) { return juce::String (std::abs (frames) * 1000.0 / rate, 3) + " ms (" + juce::String (std::abs (frames), 2) + " frames)"; };

        if (s.getNumChannels() != 2)
        {
            out.notes.add ("not stereo, so there were no channels to line up");
        }
        else if (end > start)
        {
            auto stopped = false;
            const auto found = PhaseCorrection::measure (s, start, end, rate, most, [&] (float p)
            {
                if (progress == nullptr || progress (p))
                    return true;

                stopped = true;
                return false;
            });

            if (stopped)
            {
                out.error = "Stopped.";
                return out;
            }

            if (! found.has_value() || std::abs (found->correlation) < PhaseCorrection::weakest)
            {
                out.notes.add ("no offset between the channels was found within " + juce::String (most, 1) + " ms (they may be unrelated, too short, "
                               "silent or further apart), so they were left as they were");
            }
            else
            {
                const auto upsideDown = found->correlation < 0.0;
                const auto turnOver = upsideDown && get (v, "turnOver") > 0.5;
                const auto moved = std::abs (found->offset) >= 0.01;

                out.notes.add (moved ? "the right channel was " + ms (found->offset) + (found->offset > 0.0 ? " late" : " early") + " on the left"
                                     : juce::String ("the channels were already in line"));

                if (upsideDown)
                    out.notes.add (turnOver ? juce::String ("the right channel was upside down, and was turned over")
                                            : juce::String ("the right channel looks upside down; it was left so"));

                out.notes.add ("correlation " + juce::String (std::abs (found->correlation), 2));

                if (moved || turnOver)
                    out.snapshot = PhaseCorrection::correct (s, start, end, rate, moved ? found->offset : 0.0, turnOver);
            }
        }
    }
    else if (k == "dereverb")
    {
        if (end > start && get (v, "reduction") > 0.0)
        {
            const auto set = (float) get (v, "time");
            auto used = set;
            out.snapshot = Dereverb::reduce (s, start, end, rate, { set, (float) get (v, "reduction") }, progress, &used);

            if (out.snapshot == nullptr)
            {
                out.error = "Stopped.";
                return out;
            }

            if (used < set - 0.005f)
                out.notes.add ("the recording's own sounds die away in about " + juce::String (used, 2) + " s, so the reverb time was taken as that, not "
                               + juce::String (set, 2) + " s (a longer time would cut into the sounds themselves)");
        }
    }
    else if (k == "normalize")
    {
        const auto db = juce::jmin ((float) get (v, "db"), 0.0f);
        out.snapshot = get (v, "apart") > 0.5 ? AudioEdits::normalizeChannels (s, start, end, db) : AudioEdits::normalize (s, start, end, db);

        if (out.snapshot == nullptr)
            out.notes.add ("silent, so not normalized");
    }
    else if (k == "balance")
    {
        if (const auto pan = juce::roundToInt (get (v, "pan")); pan != 0 && end > start)
            out.snapshot = AudioEdits::balance (s, start, end, (float) pan / 100.0f);
    }
    else if (k == "stereoWidth")
    {
        if (const auto width = juce::roundToInt (get (v, "width")); width != 100 && end > start)
            out.snapshot = AudioEdits::stereoWidth (s, start, end, (float) width / 100.0f);
    }
    else if (k == "echo")
    {
        const auto feedback = juce::roundToInt (get (v, "feedback"));
        const auto delay = (juce::int64) std::llround (juce::roundToInt (get (v, "delay")) / 1000.0 * rate);

        if (feedback > 0 && end > start)
            out.snapshot = AudioEdits::echo (s, start, end, delay, (float) feedback / 100.0f);
    }
    else if (k == "tremolo")
    {
        if (const auto depth = juce::roundToInt (get (v, "depth")); depth > 0 && end > start)
            out.snapshot = AudioEdits::tremolo (s, start, end, rate, get (v, "rate"), (float) depth / 100.0f);
    }
    else if (k == "mono" || k == "stereo")
    {
        const auto channels = k == "mono" ? 1 : 2;

        if (s.getNumFrames() > 0 && s.getNumChannels() != channels)
            out.snapshot = AudioEdits::convertChannels (s, channels);
    }
    else if (k == "deliverySpec" && end > start)
    {
        // As Meet Delivery Spec does it, and what the result measures against the spec, as notes.
        const auto spec = Delivery::specFor (get (v, "target"), get (v, "tolerance"), get (v, "ceiling"));
        auto d = Delivery::deliver (s, start, end, rate, spec, progress);

        if (d.error.isNotEmpty())
        {
            out.notes.add ("nothing above the -70 LUFS gate, so its loudness wasn't changed");
        }
        else if (d.snapshot == nullptr)
        {
            out.error = "Stopped.";
            return out;
        }
        else
        {
            out.snapshot = std::move (d.snapshot);

            if (d.limited)
                out.notes.add ("limited, with " + juce::String (d.gainDb, 1) + " dB of gain into the true-peak limiter");

            out.notes.add (Delivery::summary (spec, d.after));
        }
    }
    else if (k == "matchLoudness")
    {
        const auto target = get (v, "target"), ceiling = get (v, "ceiling");
        const auto measured = Loudness::measure (s, start, end, rate);

        if (! measured.has_value() || ! std::isfinite (measured->integrated))
        {
            out.notes.add ("nothing above the -70 LUFS gate, so its loudness wasn't changed");
        }
        else
        {
            const auto wanted = target - measured->integrated;
            const auto gain = juce::jmin (wanted, ceiling - measured->truePeak);

            if (get (v, "limit") > 0.5 && gain < wanted)
            {
                auto matched = Dynamics::matchLoudness (s, start, end, rate, measured->integrated, target, ceiling);
                out.snapshot = std::move (matched.snapshot);
                out.notes.add ("limited, with " + juce::String (matched.gainDb, 1) + " dB of gain into the limiter");

                if (std::abs (matched.integrated - target) > 0.05)
                    out.notes.add ("reached " + Loudness::format (matched.integrated, "LUFS"));
            }
            else
            {
                out.snapshot = AudioEdits::gain (s, start, end, (float) gain);

                if (gain < wanted - 0.05)
                    out.notes.add ("the true-peak ceiling allowed " + juce::String (gain, 1) + " dB, so it is at "
                                   + Loudness::format (measured->integrated + gain, "LUFS"));
            }
        }
    }
    else if (k == "extremeStretch")
    {
        if (end > start)
        {
            out.snapshot = ExtremeStretch::stretchRange (s, start, end, { get (v, "factor"), get (v, "window") }, rate, progress);

            if (out.snapshot == nullptr)
            {
                out.error = "Stopped.";
                return out;
            }

            const auto newEnd = end + out.snapshot->getNumFrames() - s.getNumFrames();
            out.markers = MarkerEdits::afterStretch (markers, start, end, newEnd - start);
            out.end = newEnd;
        }
    }
    else if (k == "stretch" || k == "pitch")
    {
        // A whole number of hundredths, as the dialog takes them; nothing to do at 100% or 0.
        const auto amount = std::round (get (v, k == "stretch" ? "percent" : "semitones") * 100.0) / 100.0;

        if (end > start && ! juce::exactlyEqual (amount, k == "stretch" ? 100.0 : 0.0))
        {
            // An optional setting: 1 for SpectraBlade's own stretch, else Signalsmith Stretch.
            const auto method = get (v, "method") > 0.5 ? TimeStretch::Method::spectrablade : TimeStretch::Method::signalsmith;
            out.snapshot = k == "stretch" ? TimeStretch::stretchRange (s, start, end, amount / 100.0, rate, progress, method)
                                          : TimeStretch::shiftRange (s, start, end, amount, rate, progress, method);

            if (out.snapshot == nullptr)
            {
                out.error = "Stopped.";
                return out;
            }

            const auto newEnd = end + out.snapshot->getNumFrames() - s.getNumFrames();
            out.markers = MarkerEdits::afterStretch (markers, start, end, newEnd - start);
            out.end = newEnd;
        }
    }
    else if (k == "convertRate")
    {
        const auto newRate = get (v, "rate");

        if (! juce::exactlyEqual (newRate, rate))
        {
            out.snapshot = SampleRateConverter::convert (s, rate, newRate, progress);

            if (out.snapshot == nullptr)
            {
                out.error = "Stopped.";
                return out;
            }

            const auto scale = [&] (juce::int64 frame) { return (juce::int64) std::llround ((double) frame * newRate / rate); };
            out.markers = SampleRateConverter::convert (markers, rate, newRate);
            out.sampleRate = newRate;
            out.start = scale (start);
            out.end = scale (end);
        }
    }
    else if (k == "rack")
    {
        if (plugins == nullptr)
        {
            out.error = "Apply Effects Rack can't run here.";
            return out;
        }

        if (step.rack->getNumChildElements() == 0 || end <= start)
            return out;

        juce::AudioBuffer<float> buffer (s.getNumChannels(), (int) (end - start));
        s.read (buffer, 0, start, buffer.getNumSamples());

        if (const auto error = OfflineRender::processThroughRack (*step.rack, *plugins, rate, buffer, progress); error.isNotEmpty())
        {
            out.error = error;
            return out;
        }

        AudioClip rendered { buffer.getNumChannels(), rate, AudioEdits::fromBuffer (std::move (buffer))->getSegments() };
        out.snapshot = AudioEdits::replaceWithClip (s, start, end, rendered);
    }

    return out;
}

Result run (const Sequence& sequence, const Snapshot& audio, juce::int64 start, juce::int64 end, double rate,
            const Markers& markers, juce::AudioPluginFormatManager* plugins, const std::function<bool (float)>& progress)
{
    Result out;
    out.snapshot = audio;
    out.sampleRate = rate;
    out.markers = markers;
    out.start = start;
    out.end = end;
    const auto count = (float) juce::jmax ((size_t) 1, sequence.steps.size());

    for (size_t i = 0; i < sequence.steps.size(); ++i)
    {
        const auto& step = sequence.steps[i];
        const auto stepProgress = [&progress, i, count] (float p) { return progress == nullptr || progress (((float) i + p) / count); };

        // Stopped or refused, nothing is changed.
        const auto fail = [&] (const juce::String& error)
        {
            Result failed;
            failed.snapshot = audio;
            failed.sampleRate = rate;
            failed.markers = markers;
            failed.start = start;
            failed.end = end;
            failed.error = error;
            return failed;
        };

        if (! stepProgress (0.0f))
            return fail ("Stopped.");

        auto r = apply (step, out.snapshot, out.start, out.end, out.sampleRate, out.markers, plugins, stepProgress);

        if (r.error.isNotEmpty())
            return fail (r.error == "Stopped." ? r.error : step.name() + ": " + r.error);

        for (auto& n : r.notes)
            out.notes.add (step.name() + ": " + n);

        if (r.snapshot != nullptr)
            out.snapshot = std::move (r.snapshot);

        out.sampleRate = r.sampleRate;
        out.markers = std::move (r.markers);
        out.start = r.start;
        out.end = r.end;
    }

    return out;
}

std::unique_ptr<juce::XmlElement> toXml (const Sequence& sequence)
{
    auto xml = std::make_unique<juce::XmlElement> ("SPECTRABLADE_SEQUENCE");
    xml->setAttribute ("version", 1);
    xml->setAttribute ("name", sequence.name);

    for (auto& step : sequence.steps)
    {
        auto* e = xml->createNewChildElement ("STEP");
        e->setAttribute ("kind", step.kind);

        for (auto& [id, value] : step.values)
        {
            auto* value_ = e->createNewChildElement ("VALUE");
            value_->setAttribute ("id", id);
            value_->setAttribute ("value", value);
        }

        if (step.rack != nullptr)
            e->addChildElement (new juce::XmlElement (*step.rack));
    }

    return xml;
}

std::optional<Sequence> fromXml (const juce::XmlElement& xml)
{
    if (! xml.hasTagName ("SPECTRABLADE_SEQUENCE"))
        return std::nullopt;

    Sequence sequence;
    sequence.name = xml.getStringAttribute ("name");

    for (auto* e : xml.getChildWithTagNameIterator ("STEP"))
    {
        Step step;
        step.kind = e->getStringAttribute ("kind");

        if (find (step.kind) == nullptr)
            return std::nullopt;

        for (auto* value : e->getChildWithTagNameIterator ("VALUE"))
            step.values[value->getStringAttribute ("id")] = value->getDoubleAttribute ("value");

        if (auto* rack = e->getChildByName ("SPECTRABLADE_RACK"))
            step.rack = std::make_shared<const juce::XmlElement> (*rack);

        sequence.steps.push_back (std::move (step));
    }

    return sequence;
}

Store::Store (juce::File f) : folder (std::move (f)) {}

juce::File Store::fileFor (const juce::String& name) const
{
    return folder.getChildFile (juce::File::createLegalFileName (name) + ".sbsequence");
}

juce::StringArray Store::names() const
{
    juce::StringArray list;

    for (auto& f : folder.findChildFiles (juce::File::findFiles, false, "*.sbsequence"))
        if (auto xml = juce::XmlDocument::parse (f))
            if (auto s = fromXml (*xml); s.has_value() && s->name.isNotEmpty())
                list.addIfNotAlreadyThere (s->name);

    list.sortNatural();
    return list;
}

std::optional<Sequence> Store::load (const juce::String& name) const
{
    if (auto xml = juce::XmlDocument::parse (fileFor (name)))
        return fromXml (*xml);

    return std::nullopt;
}

bool Store::contains (const juce::String& name) const    { return fileFor (name).existsAsFile(); }

bool Store::save (const Sequence& sequence)
{
    if (sequence.name.trim().isEmpty() || ! folder.createDirectory())
        return false;

    return toXml (sequence)->writeTo (fileFor (sequence.name));
}

bool Store::remove (const juce::String& name)    { return fileFor (name).deleteFile(); }

Store& userStore()
{
    static Store store (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                            .getChildFile ("SpectraBlade").getChildFile ("Sequences"));
    return store;
}

void Recorder::start()
{
    recording = true;
    recorded.clear();
    notRecorded.clear();
}

void Recorder::add (Step step)
{
    if (recording)
        recorded.push_back (std::move (step));
}

void Recorder::skip (const juce::String& edit)
{
    if (recording)
        notRecorded.add (edit);
}

Sequence Recorder::stop()
{
    recording = false;
    return { {}, recorded };
}
}
