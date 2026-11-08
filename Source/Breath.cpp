#include "Breath.h"
#include "AudioEdits.h"

namespace Breath
{
namespace
{
    constexpr double blockSeconds = 0.01, speechReach = 1.0, floorReach = 3.0, shortestSeconds = 0.08, longestSeconds = 0.8, rampSeconds = 0.02;
    constexpr double crossingsPerSecond = 1200.0, overFloorDb = 6.0, surroundReach = 0.5;
    constexpr double silentDb = -90.0;   // blocks under this are digital silence, not a floor
}

bool isValid (const Settings& s)
{
    return s.thresholdDb >= 6.0f && s.thresholdDb <= 40.0f && s.reductionDb >= 0.0f && s.reductionDb <= 40.0f;
}

std::vector<Found> find (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings)
{
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (! isValid (settings) || sampleRate <= 0.0 || end - start < (juce::int64) (0.1 * sampleRate))
        return {};

    const auto block = juce::jmax (1, (int) std::lround (blockSeconds * sampleRate));
    const auto blocks = (int) ((end - start) / block);
    const auto channels = s.getNumChannels();
    std::vector<double> levelDb ((size_t) blocks), crossings ((size_t) blocks);
    juce::AudioBuffer<float> b (channels, block);
    auto lastMix = 0.0f;

    for (int i = 0; i < blocks; ++i)
    {
        s.read (b, 0, start + (juce::int64) i * block, block);
        auto sum = 0.0;
        auto crossed = 0;

        for (int j = 0; j < block; ++j)
        {
            auto mix = 0.0f;

            for (int ch = 0; ch < channels; ++ch)
            {
                const auto v = b.getSample (ch, j);
                sum += (double) v * v;
                mix += v;
            }

            mix /= (float) channels;

            if ((mix > 0.0f && lastMix < 0.0f) || (mix < 0.0f && lastMix > 0.0f))
                ++crossed;

            if (mix != 0.0f)
                lastMix = mix;
        }

        levelDb[(size_t) i] = 10.0 * std::log10 (juce::jmax (1.0e-20, sum / (block * channels)));
        crossings[(size_t) i] = crossed / blockSeconds;
    }

    // Each block's speech level and floor from the blocks round it.
    const auto speechBlocks = (int) (speechReach / blockSeconds), floorBlocks = (int) (floorReach / blockSeconds);
    std::vector<char> candidate ((size_t) blocks, 0), quiet ((size_t) blocks, 0);
    std::vector<double> floorDb ((size_t) blocks, -200.0), around;

    for (int i = 0; i < blocks; ++i)
    {
        auto speech = -200.0;

        for (int j = juce::jmax (0, i - speechBlocks); j <= juce::jmin (blocks - 1, i + speechBlocks); ++j)
            speech = juce::jmax (speech, levelDb[(size_t) j]);

        around.clear();

        for (int j = juce::jmax (0, i - floorBlocks); j <= juce::jmin (blocks - 1, i + floorBlocks); ++j)
            if (levelDb[(size_t) j] > silentDb)
                around.push_back (levelDb[(size_t) j]);

        if (! around.empty())
        {
            const auto twentieth = around.begin() + (long) (around.size() / 20);
            std::nth_element (around.begin(), twentieth, around.end());
            floorDb[(size_t) i] = *twentieth;
        }

        const auto floor = floorDb[(size_t) i];
        quiet[(size_t) i] = levelDb[(size_t) i] < speech - settings.thresholdDb;

        candidate[(size_t) i] = quiet[(size_t) i] && levelDb[(size_t) i] > floor + overFloorDb && crossings[(size_t) i] > crossingsPerSecond;
    }

    // Runs of candidates of the right length that stand over their surroundings: the non-speech
    // blocks within half a second either side that aren't candidates themselves (the floor when
    // there are none). A noisy floor's swells stand over it by less than a breath does.
    std::vector<Found> found;
    const auto shortest = (int) std::lround (shortestSeconds / blockSeconds), longest = (int) std::lround (longestSeconds / blockSeconds);
    const auto surroundBlocks = (int) (surroundReach / blockSeconds);
    const auto median = [] (std::vector<double>& v) { const auto mid = v.begin() + (long) (v.size() / 2); std::nth_element (v.begin(), mid, v.end()); return *mid; };
    std::vector<double> surroundings, run;

    for (int i = 0; i < blocks;)
    {
        if (! candidate[(size_t) i])
        {
            ++i;
            continue;
        }

        auto j = i;

        while (j < blocks && candidate[(size_t) j])
            ++j;

        if (j - i >= shortest && j - i <= longest)
        {
            surroundings.clear();

            for (int k = juce::jmax (0, i - surroundBlocks); k < i; ++k)
                if (quiet[(size_t) k] && ! candidate[(size_t) k])
                    surroundings.push_back (levelDb[(size_t) k]);

            for (int k = j; k < juce::jmin (blocks, j + surroundBlocks); ++k)
                if (quiet[(size_t) k] && ! candidate[(size_t) k])
                    surroundings.push_back (levelDb[(size_t) k]);

            run.assign (levelDb.begin() + i, levelDb.begin() + j);
            const auto over = median (run) - (surroundings.empty() ? floorDb[(size_t) i] : median (surroundings));

            if (over >= overFloorDb)
                found.push_back ({ start + (juce::int64) i * block, start + (juce::int64) j * block, (float) over });
        }

        i = j;
    }

    return found;
}

AudioDocument::Snapshot apply (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings)
{
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (! isValid (settings) || sampleRate <= 0.0 || end - start < (juce::int64) (0.1 * sampleRate) || end - start > std::numeric_limits<int>::max() / 2)
        return nullptr;

    const auto breaths = find (s, start, end, sampleRate, settings);

    const auto ramp = juce::jmax (1, (int) std::lround (rampSeconds * sampleRate));
    const auto reductionDb = settings.reductionDb;

    return AudioEdits::processRange (s, start, end, [&breaths, start, reductionDb, ramp] (juce::AudioBuffer<float>& b)
    {
        for (const auto& breath : breaths)
        {
            // By the reduction, but no further than to its surroundings.
            const auto floorGain = std::pow (10.0, -juce::jmin ((double) reductionDb, (double) breath.overDb) / 20.0);
            const auto from = (int) (breath.start - start), to = (int) (breath.end - start);

            for (int i = juce::jmax (0, from - ramp); i < juce::jmin (b.getNumSamples(), to + ramp); ++i)
            {
                // Eased in over the ramp before the breath and out over the ramp after it.
                const auto in = juce::jlimit (0.0, 1.0, (i - (from - ramp)) / (double) ramp), out = juce::jlimit (0.0, 1.0, ((to + ramp) - i) / (double) ramp);
                const auto depth = juce::jmin (in, out);
                const auto gain = 1.0 + (floorGain - 1.0) * depth;

                for (int ch = 0; ch < b.getNumChannels(); ++ch)
                    b.setSample (ch, i, (float) (b.getSample (ch, i) * gain));
            }
        }
    });
}
}
