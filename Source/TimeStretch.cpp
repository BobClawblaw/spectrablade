#include "TimeStretch.h"
#include "SampleRateConverter.h"

namespace TimeStretch
{
int frameLength (double sampleRate)
{
    const auto wanted = juce::jmax (64.0, sampleRate * 0.04);
    return 1 << juce::roundToInt (std::log2 (wanted));
}

juce::AudioBuffer<float> stretch (const juce::AudioBuffer<float>& input, double ratio, double sampleRate,
                                  const std::function<bool (float)>& progress)
{
    ratio = juce::jlimit (shortest, longest, ratio);
    const auto channels = input.getNumChannels();
    const auto inLength = input.getNumSamples();
    const auto outLength = (int) std::llround ((double) inLength * ratio);
    juce::AudioBuffer<float> out (channels, outLength);
    out.clear();

    if (inLength == 0 || outLength == 0 || channels == 0)
        return out;

    const auto n = frameLength (sampleRate);
    const auto synthesisHop = n / 2;
    const auto tolerance = n / 4;

    // Every channel with silence either side, wide enough for any frame that is looked at, for
    // finding where frames match: by the sum over channels of their products, so channels out of
    // phase with each other don't cancel.
    const auto pad = n + tolerance + (int) std::ceil ((double) synthesisHop / ratio) + 2;
    std::vector<std::vector<float>> padded ((size_t) channels, std::vector<float> ((size_t) (inLength + 2 * pad), 0.0f));

    for (int ch = 0; ch < channels; ++ch)
        std::copy (input.getReadPointer (ch), input.getReadPointer (ch) + inLength, padded[(size_t) ch].begin() + pad);

    const auto similarity = [&] (int a, int b)
    {
        auto sum = 0.0;

        if (juce::jmin (a, b) + pad < 0 || juce::jmax (a, b) + n + pad > inLength + 2 * pad)
            return sum;

        for (auto& x : padded)
            for (int i = 0; i < n; i += 2)
                sum += (double) x[(size_t) (a + pad + i)] * (double) x[(size_t) (b + pad + i)];

        return sum;
    };

    std::vector<float> window ((size_t) n);

    for (int i = 0; i < n; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) n);

    std::vector<float> weight ((size_t) outLength, 0.0f);
    auto previous = 0;   // where the last frame was taken from in the input

    for (auto k = 0;; ++k)
    {
        const auto outStart = k * synthesisHop - synthesisHop;   // the first frame starts half a frame early

        if (outStart >= outLength)
            break;

        if (progress != nullptr && k % 64 == 0 && ! progress ((float) juce::jmax (0, outStart) / (float) outLength))
            return {};

        const auto nominal = (int) std::llround ((double) outStart / ratio);
        auto chosen = nominal;

        if (k > 0)
        {
            // The frame that would naturally follow the last one, and the candidate around the
            // nominal place that matches it best (over every other sample, for speed).
            const auto natural = previous + synthesisHop;
            auto best = -std::numeric_limits<double>::infinity();

            for (auto delta = -tolerance; delta <= tolerance; ++delta)
            {
                const auto candidate = nominal + delta;
                const auto sum = similarity (natural, candidate);

                if (sum > best)
                {
                    best = sum;
                    chosen = candidate;
                }
            }
        }

        for (int i = 0; i < n; ++i)
        {
            const auto o = outStart + i, from = chosen + i;

            if (o < 0 || o >= outLength)
                continue;

            weight[(size_t) o] += window[(size_t) i];

            if (from >= 0 && from < inLength)
                for (int ch = 0; ch < channels; ++ch)
                    out.getWritePointer (ch)[o] += window[(size_t) i] * input.getSample (ch, from);
        }

        previous = chosen;
    }

    for (int o = 0; o < outLength; ++o)
        if (weight[(size_t) o] > 1.0e-3f)
            for (int ch = 0; ch < channels; ++ch)
                out.getWritePointer (ch)[o] /= weight[(size_t) o];

    return out;
}

juce::AudioBuffer<float> shiftPitch (const juce::AudioBuffer<float>& input, double semitones, double sampleRate,
                                     const std::function<bool (float)>& progress)
{
    // Longer by the pitch ratio, then played that much faster: higher by it, and as long as before.
    const auto ratio = std::pow (2.0, juce::jlimit (-widestShift, widestShift, semitones) / 12.0);
    const auto part = [&progress] (float from, float to)
    {
        return [&progress, from, to] (float p) { return progress == nullptr || progress (from + (to - from) * p); };
    };

    const auto stretched = stretch (input, ratio, sampleRate, part (0.0f, 0.7f));
    juce::AudioBuffer<float> out;

    if (stretched.getNumSamples() == 0 && input.getNumSamples() > 0)
        return {};

    if (! SampleRateConverter::convert (stretched, sampleRate * ratio, sampleRate, out, part (0.7f, 1.0f)))
        return {};

    // The converter rounds its length up: exactly the input's length, as silence if short.
    out.setSize (out.getNumChannels(), input.getNumSamples(), true, true);
    return out;
}
}
