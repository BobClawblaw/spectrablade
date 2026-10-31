#include "SpectralEdit.h"
#include "AudioEdits.h"

namespace SpectralEdit
{
namespace
{
    constexpr int fftOrder = 12;
    constexpr int size = 1 << fftOrder;
    constexpr int hop = size / 4;
    constexpr double edgeBins = 3.0;   // frequency taper outside the band

    double smoothStep (double x)
    {
        x = juce::jlimit (0.0, 1.0, x);
        return x * x * (3.0 - 2.0 * x);
    }
}

AudioDocument::Snapshot changeBand (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end,
                                    juce::Range<double> band, double sampleRate, float gainDb)
{
    const auto length = snapshot.getNumFrames();
    start = juce::jlimit ((juce::int64) 0, length, start);
    end = juce::jlimit (start, length, end);

    if (end <= start || band.isEmpty())
        return nullptr;

    // Work on the selection plus two frames of context each side, so every replaced sample has
    // full window overlap; outside the selection the gain is one and the audio comes back as it was.
    const auto workStart = start - 2 * size;
    auto workLength = (int) (end - start) + 4 * size;
    workLength += (hop - (workLength - size) % hop) % hop;

    juce::AudioBuffer<float> work (snapshot.getNumChannels(), workLength);
    snapshot.read (work, 0, workStart, workLength);
    juce::AudioBuffer<float> out (snapshot.getNumChannels(), workLength);
    out.clear();

    juce::dsp::FFT fft (fftOrder);
    std::vector<float> window ((size_t) size), data ((size_t) size * 2);

    for (int i = 0; i < size; ++i)
        window[(size_t) i] = (float) std::sqrt (0.5 * (1.0 - std::cos (juce::MathConstants<double>::twoPi * i / size)));

    // The windows multiply to a periodic Hann; at 75% overlap those sum to 2. The FFT pair's own
    // scale is measured rather than assumed.
    data.assign (data.size(), 0.0f);
    data[0] = 1.0f;
    fft.performRealOnlyForwardTransform (data.data(), true);
    fft.performRealOnlyInverseTransform (data.data());
    const auto roundTrip = data[0];
    const auto scale = 1.0f / (2.0f * roundTrip);

    // Gain per bin: the target inside the band, tapering to one over a few bins outside it.
    const auto gain = juce::Decibels::decibelsToGain (gainDb, -300.0f);
    const auto binHz = sampleRate / size;
    std::vector<double> bandWeight ((size_t) size / 2 + 1);

    for (int b = 0; b <= size / 2; ++b)
    {
        const auto bin = b * binHz;
        const auto below = (band.getStart() - bin) / binHz, above = (bin - band.getEnd()) / binHz;
        bandWeight[(size_t) b] = 1.0 - smoothStep (juce::jmax (below, above) / edgeBins);
    }

    for (int ch = 0; ch < work.getNumChannels(); ++ch)
    {
        const auto* in = work.getReadPointer (ch);
        auto* dest = out.getWritePointer (ch);

        for (int p = 0; p + size <= workLength; p += hop)
        {
            // Time weight from where the frame's centre falls: full inside the selection,
            // easing out over one hop either side.
            const auto centre = (double) (workStart + p + size / 2);
            const auto timeWeight = smoothStep (juce::jmin (centre - ((double) start - hop), ((double) end + hop) - centre) / (2.0 * hop));

            for (int i = 0; i < size; ++i)
                data[(size_t) i] = in[p + i] * window[(size_t) i];

            std::fill (data.begin() + size, data.end(), 0.0f);

            if (timeWeight > 0.0)
            {
                fft.performRealOnlyForwardTransform (data.data(), true);

                for (int b = 0; b <= size / 2; ++b)
                {
                    const auto g = (float) (1.0 + ((double) gain - 1.0) * timeWeight * bandWeight[(size_t) b]);
                    data[(size_t) (2 * b)] *= g;
                    data[(size_t) (2 * b + 1)] *= g;
                }

                fft.performRealOnlyInverseTransform (data.data());
            }
            else
            {
                // Nothing changes in this frame: skip the transform pair, keeping its gain.
                for (int i = 0; i < size; ++i)
                    data[(size_t) i] *= roundTrip;
            }

            for (int i = 0; i < size; ++i)
                dest[p + i] += data[(size_t) i] * window[(size_t) i] * scale;
        }
    }

    // Replace the selection plus one frame each side (inside the file) with the result.
    const auto replaceStart = juce::jmax ((juce::int64) 0, start - size);
    const auto replaceEnd = juce::jmin (length, end + size);
    juce::AudioBuffer<float> result (out.getNumChannels(), (int) (replaceEnd - replaceStart));

    for (int ch = 0; ch < out.getNumChannels(); ++ch)
        result.copyFrom (ch, 0, out, ch, (int) (replaceStart - workStart), result.getNumSamples());

    AudioClip clip { snapshot.getNumChannels(), sampleRate, AudioEdits::fromBuffer (std::move (result))->getSegments() };
    return AudioEdits::replaceWithClip (snapshot, replaceStart, replaceEnd, clip);
}
}
