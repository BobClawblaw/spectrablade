#include <JuceHeader.h>
#include "../Source/Azimuth.h"
#include "../Source/PhaseCorrection.h"
#include "../Source/AudioEdits.h"
#include "TestUtils.h"
#include "AzimuthTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    /** Ten seconds of band-limited noise in both channels, the right late on the left by an offset
        drifting from 0.2 to 1.8 frames over the ten seconds (between samples, by a windowed sinc). */
    juce::AudioBuffer<float> transfer()
    {
        juce::Random r (37);
        const auto n = 10 * 48000;
        std::vector<float> source ((size_t) n + 64);
        auto state = 0.0;

        for (auto& v : source)
        {
            state = 0.8 * state + 0.2 * (r.nextDouble() * 2.0 - 1.0);   // noise with its highs gentled, so the offset reads between samples
            v = (float) (0.3 * state);
        }

        juce::AudioBuffer<float> b (2, n);

        for (int i = 0; i < n; ++i)
        {
            const auto offset = 0.2 + 1.6 * i / (double) n;
            const auto position = i + 32 - offset;
            const auto centre = (int) std::floor (position);
            const auto fraction = position - centre;
            auto sum = 0.0, weights = 0.0;

            for (int k = -7; k <= 8; ++k)
            {
                const auto d = k - fraction;
                const auto sinc = std::abs (d) < 1.0e-9 ? 1.0 : std::sin (juce::MathConstants<double>::pi * d) / (juce::MathConstants<double>::pi * d);
                const auto w = sinc * (0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * d / 8.0));
                sum += w * source[(size_t) juce::jlimit (0, n + 63, centre + k)];
                weights += w;
            }

            b.setSample (0, i, source[(size_t) (i + 32)]);
            b.setSample (1, i, (float) (sum / weights));
        }

        return b;
    }

    double offsetOver (const AudioSnapshot& s, double fromSeconds)
    {
        const auto found = PhaseCorrection::measure (s, (juce::int64) (fromSeconds * rate), (juce::int64) ((fromSeconds + 1.0) * rate), rate, 5.0);
        return found.has_value() ? found->offset : 1000.0;
    }
}

void runAzimuthTests()
{
    std::cout << "Azimuth:" << std::endl;

    // The drift is read (0.2 to 1.8 frames); corrected, the channels line up within 0.05 frames at
    // the start, the middle and the end, where they were up to 1.8 apart; mono audio and a short
    // range give nothing; cancelling.
    {
        const auto in = transfer();
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        Azimuth::Settings s;
        const auto drift = Azimuth::measure (*snap, 0, in.getNumSamples(), rate, s);
        const auto readFirst = drift.offsetFrames.empty() ? 1000.0 : drift.offsetFrames.front(), readLast = drift.offsetFrames.empty() ? 1000.0 : drift.offsetFrames.back();
        const auto out = Azimuth::correct (*snap, 0, in.getNumSamples(), rate, s);
        const auto beforeMid = offsetOver (*snap, 4.5), beforeEnd = offsetOver (*snap, 8.5);
        const auto afterStart = out != nullptr ? offsetOver (*out, 0.5) : 1000.0, afterMid = out != nullptr ? offsetOver (*out, 4.5) : 1000.0, afterEnd = out != nullptr ? offsetOver (*out, 8.5) : 1000.0;

        juce::AudioBuffer<float> mono (1, 4 * 48000);
        mono.clear();
        const auto refused = Azimuth::correct (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (mono)), 0, mono.getNumSamples(), rate, s) == nullptr
                             && Azimuth::correct (*snap, 0, 48000, rate, s) == nullptr;
        const auto cancelled = Azimuth::correct (*snap, 0, in.getNumSamples(), rate, s, [] (float) { return false; }) == nullptr;

        std::cout << "      read: " << readFirst << " frames at the start, " << readLast << " at the end; before: " << beforeMid << " at 4.5 s, " << beforeEnd
                  << " at 8.5 s; after: " << afterStart << ", " << afterMid << ", " << afterEnd << std::endl;
        check (std::abs (readFirst - 0.36) < 0.15 && std::abs (readLast - 1.64) < 0.15 && beforeEnd > 1.4 && std::abs (afterStart) < 0.05 && std::abs (afterMid) < 0.05 && std::abs (afterEnd) < 0.05
                   && refused && cancelled,
               "azimuth: an offset drifting from 0.2 to 1.8 frames over ten seconds is read within 0.15 frames at both ends; corrected, the channels "
               "line up within 0.05 frames at the start, the middle and the end (where they were over 1.4 apart); mono audio and a range under two "
               "seconds give nothing; it can be cancelled");
    }
}
