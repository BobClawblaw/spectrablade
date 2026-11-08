#include "Spill.h"
#include "AudioEdits.h"
#include "Multiband.h"
#include "DialogueIsolation.h"

namespace Spill
{
namespace
{
    constexpr double blockSeconds = 0.01;
    constexpr int fftOrder = 12, fftSize = 1 << fftOrder;

    /** Each block's RMS level in dB of a buffer's channels, in blocks of `block` frames. */
    std::vector<float> levelsOf (const juce::AudioBuffer<float>& b, int block)
    {
        const auto blocks = b.getNumSamples() / block;
        std::vector<float> out ((size_t) blocks);

        for (int i = 0; i < blocks; ++i)
        {
            auto sum = 0.0;

            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                for (int j = 0; j < block; ++j)
                {
                    const auto v = (double) b.getSample (ch, i * block + j);
                    sum += v * v;
                }

            out[(size_t) i] = (float) (10.0 * std::log10 (juce::jmax (1.0e-20, sum / (block * b.getNumChannels()))));
        }

        return out;
    }

    /** The value at a share of the sorted window round i, over reach blocks either side. */
    float percentileAround (const std::vector<float>& v, int i, int reach, double share)
    {
        const auto from = juce::jmax (0, i - reach), to = juce::jmin ((int) v.size() - 1, i + reach);
        std::vector<float> window (v.begin() + from, v.begin() + to + 1);
        std::sort (window.begin(), window.end());
        return window[(size_t) juce::jlimit (0, (int) window.size() - 1, (int) std::floor (share * (double) (window.size() - 1)))];
    }

    float maxAround (const std::vector<float>& v, int i, int reach)
    {
        auto m = -200.0f;

        for (int j = juce::jmax (0, i - reach); j <= juce::jmin ((int) v.size() - 1, i + reach); ++j)
            m = juce::jmax (m, v[(size_t) j]);

        return m;
    }

    float minAround (const std::vector<float>& v, int i, int reach)
    {
        auto m = 200.0f;

        for (int j = juce::jmax (0, i - reach); j <= juce::jmin ((int) v.size() - 1, i + reach); ++j)
            m = juce::jmin (m, v[(size_t) j]);

        return m;
    }

    /** Gains eased: a block's cut can be no deeper than a neighbour's plus the ramp's share of the
        full depth for each block between, looking both ways, so a dip is eased in and out. */
    void ease (std::vector<float>& db, int rampBlocks, double depthDb)
    {
        if (rampBlocks <= 1)
            return;

        auto out = db;
        const auto perBlock = (float) (depthDb / rampBlocks);

        for (int i = 0; i < (int) db.size(); ++i)
            for (int j = juce::jmax (0, i - rampBlocks); j <= juce::jmin ((int) db.size() - 1, i + rampBlocks); ++j)
            {
                const auto deepest = db[(size_t) j] - (float) std::abs (i - j) * perBlock;   // gains are 0 or less
                out[(size_t) i] = juce::jmax (out[(size_t) i], deepest);
            }

        db = out;
    }
}

bool isValid (const Settings& s, double sampleRate)
{
    return sampleRate > 0.0 && s.windReductionDb >= 0.0 && s.windReductionDb <= 40.0 && s.windCornerHz >= 60.0 && s.windCornerHz <= 400.0 && s.windCornerHz < 0.45 * sampleRate
        && s.rustleReductionDb >= 0.0 && s.rustleReductionDb <= 40.0 && s.bleedReductionDb >= 0.0 && s.bleedReductionDb <= 40.0
        && s.bleedThresholdDb >= 6.0 && s.bleedThresholdDb <= 40.0;
}

Analysis analyse (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings)
{
    Analysis a;
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (! isValid (settings, sampleRate) || end - start < (juce::int64) (0.1 * sampleRate) || end - start > std::numeric_limits<int>::max() / 4)
        return a;

    const auto block = juce::jmax (1, (int) std::lround (blockSeconds * sampleRate));
    const auto frames = (int) (end - start);
    const auto channels = s.getNumChannels();
    a.blocks = frames / block;
    juce::AudioBuffer<float> whole (channels, frames), lows (channels, frames), highs (channels, frames);
    s.read (whole, 0, start, frames);

    // The lows below the wind corner and the highs above 1 kHz, each channel through its own split.
    for (int ch = 0; ch < channels; ++ch)
    {
        Multiband::Crossover low, high;
        low.set (settings.windCornerHz, sampleRate);
        high.set (1000.0, sampleRate);

        for (int i = 0; i < frames; ++i)
        {
            double l = 0.0, rest = 0.0, mid = 0.0, h = 0.0;
            low.split (whole.getSample (ch, i), l, rest);
            high.split (whole.getSample (ch, i), mid, h);
            lows.setSample (ch, i, (float) l);
            highs.setSample (ch, i, (float) h);
        }
    }

    const auto level = levelsOf (whole, block), lowLevel = levelsOf (lows, block), highLevel = levelsOf (highs, block);

    // Voicing every 20 ms: a block is voiced when the frame centred over it is.
    std::vector<char> voiced ((size_t) a.blocks, 0);
    const auto threshold = DialogueIsolation::thresholdFor (50.0f);

    for (int b = 0; b < a.blocks; b += 2)
    {
        const auto centre = b * block + block / 2;
        const auto v = DialogueIsolation::pitchOf (whole, centre - fftSize / 2, sampleRate).voicing >= threshold;
        voiced[(size_t) b] = v;

        if (b + 1 < a.blocks)
            voiced[(size_t) b + 1] = v;
    }

    const auto nearVoiced = [&voiced, &a] (int i, int reach)
    {
        for (int j = juce::jmax (0, i - reach); j <= juce::jmin (a.blocks - 1, i + reach); ++j)
            if (voiced[(size_t) j])
                return true;

        return false;
    };

    a.windDb.assign ((size_t) a.blocks, 0.0f);
    a.rustleDb.assign ((size_t) a.blocks, 0.0f);
    a.bleedDb.assign ((size_t) a.blocks, 0.0f);
    const auto fiveSeconds = (int) (5.0 / blockSeconds), threeSeconds = (int) (3.0 / blockSeconds), oneSecond = (int) (1.0 / blockSeconds);

    // Wind: the lows over their typical level by 9 dB in an unvoiced block.
    if (settings.windReductionDb > 0.0)
    {
        for (int i = 0; i < a.blocks; ++i)
            if (! voiced[(size_t) i] && lowLevel[(size_t) i] > percentileAround (lowLevel, i, fiveSeconds, 0.3) + 9.0f)
                a.windDb[(size_t) i] = (float) -settings.windReductionDb;

        ease (a.windDb, 5, settings.windReductionDb);
    }

    // Rustle: bursts of the highs over their floor by 9 dB, unvoiced, 20 to 400 ms, away from voice.
    if (settings.rustleReductionDb > 0.0)
    {
        std::vector<char> candidate ((size_t) a.blocks, 0);

        for (int i = 0; i < a.blocks; ++i)
            candidate[(size_t) i] = ! voiced[(size_t) i] && highLevel[(size_t) i] > minAround (highLevel, i, threeSeconds) + 9.0f && ! nearVoiced (i, 15);

        for (int i = 0; i < a.blocks;)
        {
            if (! candidate[(size_t) i])
            {
                ++i;
                continue;
            }

            auto j = i;

            while (j < a.blocks && candidate[(size_t) j])
                ++j;

            if (j - i >= 2 && j - i <= 40)
                for (int k = i; k < j; ++k)
                    a.rustleDb[(size_t) k] = (float) -settings.rustleReductionDb;

            i = j;
        }

        ease (a.rustleDb, 1, settings.rustleReductionDb);
    }

    // Bleed: between the words, more than the threshold under the speech level, away from voice.
    if (settings.bleedReductionDb > 0.0)
    {
        for (int i = 0; i < a.blocks; ++i)
            if (! voiced[(size_t) i] && ! nearVoiced (i, 10) && level[(size_t) i] < maxAround (level, i, oneSecond) - (float) settings.bleedThresholdDb)
                a.bleedDb[(size_t) i] = (float) -settings.bleedReductionDb;

        ease (a.bleedDb, 3, settings.bleedReductionDb);
    }

    return a;
}

AudioDocument::Snapshot apply (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings,
                               const std::function<bool (float)>& progress)
{
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (! isValid (settings, sampleRate) || end - start < (juce::int64) (0.1 * sampleRate) || end - start > std::numeric_limits<int>::max() / 4)
        return nullptr;

    if (progress && ! progress (0.0f))
        return nullptr;

    const auto a = analyse (s, start, end, sampleRate, settings);

    if (a.blocks == 0 || (progress && ! progress (0.5f)))
        return nullptr;

    const auto block = juce::jmax (1, (int) std::lround (blockSeconds * sampleRate));

    return AudioEdits::processRange (s, start, end, [&a, &settings, sampleRate, block] (juce::AudioBuffer<float>& b)
    {
        // The three bands again, each block's gains on them, the bands added back.
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            Multiband::Crossover low, high;
            low.set (settings.windCornerHz, sampleRate);
            high.set (1000.0, sampleRate);
            auto* x = b.getWritePointer (ch);

            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const auto blockIndex = juce::jmin (a.blocks - 1, i / block);
                const auto wind = std::pow (10.0, a.windDb[(size_t) blockIndex] / 20.0), rustle = std::pow (10.0, a.rustleDb[(size_t) blockIndex] / 20.0),
                           bleed = std::pow (10.0, a.bleedDb[(size_t) blockIndex] / 20.0);
                double l = 0.0, rest = 0.0, mid = 0.0, h = 0.0;
                low.split (x[i], l, rest);
                high.split (rest, mid, h);
                x[i] = (float) ((l * wind + mid + h * rustle) * bleed);
            }
        }
    });
}
}
