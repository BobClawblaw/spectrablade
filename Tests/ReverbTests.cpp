#include <JuceHeader.h>
#include "../Source/Reverb.h"
#include "TestUtils.h"
#include "ReverbTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    /** The reverb of an impulse at frame 0 in seconds of stereo silence, the whole file processed. */
    juce::AudioBuffer<float> impulseResponse (const RoomReverb::Settings& s, double seconds)
    {
        juce::AudioBuffer<float> b (2, (int) (seconds * rate));
        b.clear();
        b.setSample (0, 0, 1.0f);
        b.setSample (1, 0, 1.0f);
        const auto snap = AudioEdits::fromBuffer (std::move (b));
        return readAll (*RoomReverb::apply (*snap, 0, snap->getNumFrames(), rate, s));
    }

    /** The level in dB of both channels over 50 ms from `at` seconds. */
    double levelDb (const juce::AudioBuffer<float>& b, double at)
    {
        const auto from = (int) (at * rate), n = (int) (0.05 * rate);
        double sum = 0.0;

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = from; i < from + n; ++i)
                sum += (double) b.getSample (ch, i) * b.getSample (ch, i);

        return 10.0 * std::log10 (sum / (n * b.getNumChannels()) + 1.0e-30);
    }
}

void runReverbTests()
{
    // The decay: -60 dB over the decay time, measured as the fall from 0.5 s to 1.5 s.
    {
        RoomReverb::Settings s;
        s.preDelayMs = 0.0;
        s.dampingPercent = 0.0;
        s.mixPercent = 100.0;
        auto measured = std::array<double, 3> {};
        const auto decays = std::array<double, 3> { 1.0, 2.0, 4.0 };

        for (size_t k = 0; k < decays.size(); ++k)
        {
            s.decaySeconds = decays[k];
            const auto ir = impulseResponse (s, 2.0);
            measured[k] = 60.0 * 1.0 / (levelDb (ir, 0.5) - levelDb (ir, 1.5));   // seconds to fall 60 dB
        }

        std::cout << "      reverb decay: set 1, 2, 4 s, measured " << measured[0] << ", " << measured[1] << ", " << measured[2] << " s" << std::endl;
        auto close = true;

        for (size_t k = 0; k < decays.size(); ++k)
            close = close && std::abs (measured[k] / decays[k] - 1.0) < 0.1;

        check (close, "reverb: an impulse's reverb falls 60 dB in the decay time (1, 2 and 4 s, each within 10 %)");
    }

    // The pre-delay: nothing before it plus the shortest line, then reverb.
    {
        RoomReverb::Settings s;
        s.preDelayMs = 100.0;
        s.mixPercent = 100.0;
        const auto ir = impulseResponse (s, 0.5);
        int first = -1;

        for (int i = 0; i < ir.getNumSamples() && first < 0; ++i)
            if (ir.getSample (0, i) != 0.0f || ir.getSample (1, i) != 0.0f)
                first = i;

        std::cout << "      reverb pre-delay 100 ms: first sound at frame " << first << " (" << first / rate * 1000.0 << " ms)" << std::endl;
        check (first >= 4800 + (int) std::lround (0.0297 * rate) && first < 4800 + (int) (0.04 * rate),
               "reverb: with a 100 ms pre-delay, silence until the pre-delay plus the shortest line (29.7 ms), and reverb within 40 ms of it");
    }

    // Damping darkens the tail: the share of its energy in sample-to-sample change falls.
    {
        const auto brightness = [] (double damping)
        {
            RoomReverb::Settings s;
            s.preDelayMs = 0.0;
            s.mixPercent = 100.0;
            s.dampingPercent = damping;
            const auto ir = impulseResponse (s, 1.0);
            double change = 0.0, energy = 0.0;

            for (int i = (int) (0.3 * rate); i < (int) (0.6 * rate); ++i)
            {
                const auto x = (double) ir.getSample (0, i), d = x - ir.getSample (0, i - 1);
                change += d * d;
                energy += x * x;
            }

            return change / energy;
        };

        const auto bright = brightness (0.0), dark = brightness (80.0);
        std::cout << "      reverb damping: high share " << bright << " at 0 %, " << dark << " at 80 %" << std::endl;
        check (dark < 0.5 * bright, "reverb: 80 % damping leaves the tail with under half the highs of no damping");
    }

    // Mix and width: no mix is the input bit for bit; no width makes the reverb the same on both sides.
    juce::Random r (17);
    juce::AudioBuffer<float> noise (2, 48000);

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < noise.getNumSamples(); ++i)
            noise.setSample (ch, i, r.nextFloat() - 0.5f);

    const auto noiseSnap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (noise));
    {
        RoomReverb::Settings dry;
        dry.mixPercent = 0.0;
        const auto same = readAll (*RoomReverb::apply (*noiseSnap, 0, 48000, rate, dry));
        RoomReverb::Settings mono;
        mono.mixPercent = 100.0;
        mono.widthPercent = 0.0;
        const auto narrow = readAll (*RoomReverb::apply (*noiseSnap, 0, 48000, rate, mono));
        mono.widthPercent = 100.0;
        const auto wide = readAll (*RoomReverb::apply (*noiseSnap, 0, 48000, rate, mono));
        auto unchanged = true, together = true, apart = false;

        for (int i = 0; i < 48000; ++i)
        {
            unchanged = unchanged && juce::exactlyEqual (same.getSample (0, i), noise.getSample (0, i)) && juce::exactlyEqual (same.getSample (1, i), noise.getSample (1, i));
            together = together && juce::exactlyEqual (narrow.getSample (0, i), narrow.getSample (1, i));
            apart = apart || ! juce::exactlyEqual (wide.getSample (0, i), wide.getSample (1, i));
        }

        // Twenty seconds of decay on noise stays bounded.
        RoomReverb::Settings longest;
        longest.decaySeconds = 20.0;
        longest.mixPercent = 100.0;
        longest.dampingPercent = 0.0;
        const auto held = readAll (*RoomReverb::apply (*noiseSnap, 0, 48000, rate, longest));
        std::cout << "      reverb 20 s decay on noise (peak 0.5): peak " << held.getMagnitude (0, 48000) << std::endl;
        check (unchanged && together && apart && std::isfinite (held.getMagnitude (0, 48000)) && held.getMagnitude (0, 48000) < 10.0f,
               "reverb: no mix gives the input bit for bit; no width the same reverb on both sides, full width different sides; a 20 s decay on noise stays bounded");
    }

    // The tail: a range ending mid-file rings on into what follows, added on top, for the pre-delay
    // plus the decay, and no further.
    {
        RoomReverb::Settings s;
        s.decaySeconds = 0.3;
        s.preDelayMs = 10.0;
        const auto out = readAll (*RoomReverb::apply (*noiseSnap, 0, 12000, rate, s));
        const auto tailEnd = 12000 + (int) std::ceil (0.31 * rate);
        auto before = false, after = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 12000; i < 48000; ++i)
            {
                if (i < tailEnd)
                    before = before || ! juce::exactlyEqual (out.getSample (ch, i), noise.getSample (ch, i));
                else
                    after = after && juce::exactlyEqual (out.getSample (ch, i), noise.getSample (ch, i));
            }

        RoomReverb::Settings bad;
        bad.decaySeconds = 0.05;
        check (before && after && RoomReverb::apply (*noiseSnap, 0, 48000, rate, bad) == nullptr && RoomReverb::apply (*noiseSnap, 100, 100, rate, s) == nullptr,
               "reverb: the tail is added after the range for the pre-delay plus the decay, the audio after that bit for bit; a decay under 0.1 s or an empty range refused");
    }
}
