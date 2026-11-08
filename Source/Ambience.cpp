#include "Ambience.h"
#include "AudioEdits.h"

namespace Ambience
{
namespace
{
    constexpr int fftOrder = 12, size = frameSize, hop = size / 4, bins = size / 2 + 1;

    std::vector<float> rootHann()
    {
        std::vector<float> w ((size_t) size);

        for (int i = 0; i < size; ++i)
            w[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / size));

        return w;
    }
}

std::optional<Print> capture (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate)
{
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (sampleRate <= 0.0 || end - start < size)
        return std::nullopt;

    const auto channels = s.getNumChannels();
    Print print { sampleRate, std::vector<std::vector<float>> ((size_t) channels, std::vector<float> ((size_t) bins, 0.0f)) };
    juce::dsp::FFT fft (fftOrder);
    std::vector<float> data ((size_t) (2 * size));
    const auto window = rootHann();
    juce::AudioBuffer<float> block (channels, size);
    auto frames = 0;

    for (auto pos = start; pos + size <= end; pos += hop, ++frames)
    {
        s.read (block, 0, pos, size);

        for (int ch = 0; ch < channels; ++ch)
        {
            for (int i = 0; i < size; ++i)
                data[(size_t) i] = block.getSample (ch, i) * window[(size_t) i];

            std::fill (data.begin() + size, data.end(), 0.0f);
            fft.performRealOnlyForwardTransform (data.data(), true);

            for (int k = 0; k < bins; ++k)
                print.power[(size_t) ch][(size_t) k] += data[(size_t) (2 * k)] * data[(size_t) (2 * k)] + data[(size_t) (2 * k + 1)] * data[(size_t) (2 * k + 1)];
        }
    }

    for (auto& p : print.power)
        for (auto& v : p)
            v /= (float) frames;

    return print;
}

bool isValid (const Settings& s)
{
    return s.levelDb >= -24.0f && s.levelDb <= 12.0f;
}

AudioDocument::Snapshot apply (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Print& print, const Settings& settings)
{
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (! isValid (settings) || ! juce::exactlyEqual (print.sampleRate, sampleRate) || print.power.empty() || start == end || end - start > std::numeric_limits<int>::max() / 4)
        return nullptr;

    const auto channels = s.getNumChannels();
    const auto frames = (int) (end - start);
    const auto window = rootHann();
    const auto level = std::pow (10.0, settings.levelDb / 20.0);
    const auto padded = frames + 2 * size;
    juce::AudioBuffer<float> in (channels, padded), tone (channels, padded);
    in.clear();
    tone.clear();
    s.read (in, size, start, frames);

    juce::dsp::FFT fft (fftOrder);
    std::vector<float> data ((size_t) (2 * size)), existing ((size_t) (2 * size));
    std::vector<double> ownPower ((size_t) bins), ownSmoothed ((size_t) bins);
    const auto steps = (padded - size) / hop + 1;

    for (int ch = 0; ch < channels; ++ch)
    {
        // A print with another channel count: its channels averaged.
        const auto& p = (int) print.power.size() == channels ? print.power[(size_t) ch] : print.power[0];
        juce::Random random (0x2a7 + ch);

        for (int step = 0; step < steps; ++step)
        {
            const auto pos = step * hop;

            // The range's own power here, smoothed over the bins round each (one frame's single
            // bin swings too much to compare), for filling only the quiet.
            if (settings.fillOnlyTheQuiet)
            {
                for (int i = 0; i < size; ++i)
                    existing[(size_t) i] = in.getSample (ch, pos + i) * window[(size_t) i];

                std::fill (existing.begin() + size, existing.end(), 0.0f);
                fft.performRealOnlyForwardTransform (existing.data(), true);

                for (int k = 0; k < bins; ++k)
                    ownPower[(size_t) k] = (double) existing[(size_t) (2 * k)] * existing[(size_t) (2 * k)] + (double) existing[(size_t) (2 * k + 1)] * existing[(size_t) (2 * k + 1)];

                for (int k = 0; k < bins; ++k)
                {
                    auto sum = 0.0;
                    auto n = 0;

                    for (int j = juce::jmax (0, k - 8); j <= juce::jmin (bins - 1, k + 8); ++j)
                    {
                        sum += ownPower[(size_t) j];
                        ++n;
                    }

                    ownSmoothed[(size_t) k] = sum / n;
                }
            }

            std::fill (data.begin(), data.end(), 0.0f);

            for (int k = 1; k < bins - 1; ++k)
            {
                auto wanted = (double) p[(size_t) k] * level * level;

                if (settings.fillOnlyTheQuiet)
                    wanted = juce::jmax (0.0, wanted - ownSmoothed[(size_t) k]);

                const auto magnitude = std::sqrt (wanted);
                const auto phase = random.nextDouble() * juce::MathConstants<double>::twoPi;
                data[(size_t) (2 * k)] = (float) (magnitude * std::cos (phase));
                data[(size_t) (2 * k + 1)] = (float) (magnitude * std::sin (phase));
            }

            fft.performRealOnlyInverseTransform (data.data());

            // A frame of random phase at the print's magnitudes holds half the tone's power (the
            // analysis window's share); the synthesis windows' squares sum to two at this overlap,
            // and frames of random phase add in power, so the tone comes out at the print's level.
            for (int i = 0; i < size; ++i)
                tone.addSample (ch, pos + i, data[(size_t) i] * window[(size_t) i]);
        }
    }

    return AudioEdits::processRange (s, start, end, [&tone, frames] (juce::AudioBuffer<float>& b)
    {
        // Added, eased in and out over a frame at the edges.
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < frames; ++i)
            {
                const auto edge = juce::jmin (i, frames - 1 - i);
                const auto weight = (float) juce::jlimit (0.0, 1.0, (double) edge / size);
                b.addSample (ch, i, tone.getSample (ch, size + i) * weight);
            }
    });
}
}
