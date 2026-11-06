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

    // Every channel carried on past either end as its mirror image, wide enough for any frame
    // that is looked at: frames there read sound that carries on smoothly rather than silence, so
    // the output keeps its level up to its edges without jumping. Frames are matched by the sum
    // over channels of their products, so channels out of phase with each other don't cancel.
    const auto pad = n + tolerance + (int) std::ceil ((double) synthesisHop / ratio) + 2;
    std::vector<std::vector<float>> padded ((size_t) channels, std::vector<float> ((size_t) (inLength + 2 * pad), 0.0f));
    const auto mirrored = [inLength] (int i)
    {
        for (;;)
        {
            if (i < 0)
                i = -i;
            else if (i >= inLength)
                i = 2 * (inLength - 1) - i;
            else
                return i;

            if (inLength == 1)
                return 0;
        }
    };

    for (int ch = 0; ch < channels; ++ch)
        for (int i = -pad; i < inLength + pad; ++i)
            padded[(size_t) ch][(size_t) (i + pad)] = input.getSample (ch, mirrored (i));

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

            if (o < 0 || o >= outLength || from + pad < 0 || from + pad >= inLength + 2 * pad)
                continue;

            weight[(size_t) o] += window[(size_t) i];

            for (int ch = 0; ch < channels; ++ch)
                out.getWritePointer (ch)[o] += window[(size_t) i] * padded[(size_t) ch][(size_t) (from + pad)];
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

namespace
{
    /** Where processed audio meets the audio either side of it: crossfaded over joinSeconds
        (or a quarter of the range when that is shorter), from the original into the processed
        at the start and back at the end, so the joins don't click. A join at the file's start
        or end needs none. */
    void blendJoins (juce::AudioBuffer<float>& processed, const juce::AudioBuffer<float>& original, bool atStart, bool atEnd, double rate)
    {
        const auto length = processed.getNumSamples(), originalLength = original.getNumSamples();
        const auto frames = juce::jmin ((int) std::llround (joinSeconds * rate), length / 4, originalLength / 4);

        if (frames <= 0)
            return;

        for (int ch = 0; ch < processed.getNumChannels(); ++ch)
        {
            auto* p = processed.getWritePointer (ch);
            const auto* o = original.getReadPointer (ch);

            for (int i = 0; i < frames; ++i)
            {
                const auto t = ((float) i + 0.5f) / (float) frames;

                if (! atStart)
                    p[i] = o[i] + (p[i] - o[i]) * t;

                if (! atEnd)
                {
                    auto& last = p[length - frames + i];
                    last = last + (o[originalLength - frames + i] - last) * t;
                }
            }
        }
    }

    /** [start, end) of the audio, processed with up to contextSeconds of the audio either side
        of it (so what comes out at its edges carries on from what is there), that part cut out
        (`ratio` times its length), put back in its place with its joins blended; null for an
        empty range, a range too long to hold, or when processing stops (an empty result). */
    AudioDocument::Snapshot processRange (const AudioSnapshot& audio, juce::int64 start, juce::int64 end, double ratio, double rate,
                                          const std::function<juce::AudioBuffer<float> (const juce::AudioBuffer<float>&)>& process)
    {
        constexpr double contextSeconds = 0.25;
        start = juce::jlimit ((juce::int64) 0, audio.getNumFrames(), start);
        end = juce::jlimit (start, audio.getNumFrames(), end);
        const auto context = (juce::int64) std::llround (contextSeconds * rate);
        const auto from = juce::jmax ((juce::int64) 0, start - context), to = juce::jmin (audio.getNumFrames(), end + context);

        if (end <= start || to - from > std::numeric_limits<int>::max())
            return nullptr;

        juce::AudioBuffer<float> wide (audio.getNumChannels(), (int) (to - from));
        audio.read (wide, 0, from, wide.getNumSamples());
        const auto worked = process (wide);

        if (worked.getNumSamples() == 0)
            return nullptr;

        const auto length = (int) (end - start);
        const auto first = juce::jlimit (0, worked.getNumSamples(), (int) std::llround ((double) (start - from) * ratio));
        const auto count = juce::jmin (worked.getNumSamples() - first, (int) std::llround ((double) length * ratio));
        juce::AudioBuffer<float> original (audio.getNumChannels(), length), processed (worked.getNumChannels(), count);

        for (int ch = 0; ch < wide.getNumChannels(); ++ch)
        {
            original.copyFrom (ch, 0, wide, ch, (int) (start - from), length);
            processed.copyFrom (ch, 0, worked, ch, first, count);
        }

        blendJoins (processed, original, start == 0, end == audio.getNumFrames(), rate);
        const auto frames = (juce::int64) processed.getNumSamples();
        AudioClip clip { processed.getNumChannels(), rate, { { std::make_shared<const AudioBlock> (std::move (processed)), 0, frames } } };
        return AudioEdits::replaceWithClip (audio, start, end, clip);
    }
}

AudioDocument::Snapshot stretchRange (const AudioSnapshot& audio, juce::int64 start, juce::int64 end, double ratio, double rate,
                                      const std::function<bool (float)>& progress)
{
    ratio = juce::jlimit (shortest, longest, ratio);
    return processRange (audio, start, end, ratio, rate, [&] (const juce::AudioBuffer<float>& in) { return stretch (in, ratio, rate, progress); });
}

AudioDocument::Snapshot shiftRange (const AudioSnapshot& audio, juce::int64 start, juce::int64 end, double semitones, double rate,
                                    const std::function<bool (float)>& progress)
{
    return processRange (audio, start, end, 1.0, rate, [&] (const juce::AudioBuffer<float>& in) { return shiftPitch (in, semitones, rate, progress); });
}
}
