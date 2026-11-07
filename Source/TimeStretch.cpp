#include "TimeStretch.h"
#include <signalsmith-stretch.h>

namespace TimeStretch
{
namespace
{
    using Stretcher = signalsmith::stretch::SignalsmithStretch<float>;

    /** Every channel of the input with `pad` samples either side, carried on past each end as its
        image turned through the end sample (2 x[0] - x[i] before, likewise after): the waveform's
        slope carries on through the end, so the stretcher starts and ends inside sound that
        continues smoothly and the output keeps its level to its edges (a plain mirror turns the
        slope back, and the stretcher smears that kink into the audio kept). */
    juce::AudioBuffer<float> reflected (const juce::AudioBuffer<float>& input, int pad)
    {
        const auto length = input.getNumSamples();
        juce::AudioBuffer<float> padded (input.getNumChannels(), length + 2 * pad);

        for (int ch = 0; ch < input.getNumChannels(); ++ch)
        {
            const auto* in = input.getReadPointer (ch);
            auto* out = padded.getWritePointer (ch);
            std::copy (in, in + length, out + pad);

            for (int i = 1; i <= pad; ++i)
            {
                out[pad - i] = 2.0f * in[0] - in[juce::jmin (i, length - 1)];
                out[pad + length - 1 + i] = 2.0f * in[length - 1] - in[juce::jmax (length - 1 - i, 0)];
            }
        }

        return padded;
    }

    /** The input (ratio within shortest to longest) to round (length * ratio) samples, through a
        stretcher already set up for its pitch. As the library's exact(), but in pieces so progress
        hears how far it is and can stop it (an empty buffer then). */
    juce::AudioBuffer<float> run (Stretcher& stretcher, const juce::AudioBuffer<float>& input, double ratio,
                                  const std::function<bool (float)>& progress)
    {
        const auto channels = input.getNumChannels();
        const auto length = input.getNumSamples();
        const auto outLength = (int) std::llround ((double) length * ratio);
        juce::AudioBuffer<float> out (channels, outLength);
        out.clear();

        if (length == 0 || outLength == 0 || channels == 0)
            return out;

        // The library needs some input before its first output: a little more than its seek
        // length, reflected either side, cut away again afterwards.
        const auto playbackRate = 1.0f / (float) ratio;
        const auto pad = stretcher.outputSeekLength (playbackRate) + stretcher.blockSamples();
        const auto padded = reflected (input, pad);
        const auto inTotal = padded.getNumSamples();
        const auto outTotal = (int) std::llround ((double) inTotal * ratio);
        const auto rate = (float) inTotal / (float) outTotal;
        const auto seek = stretcher.outputSeekLength (rate);
        juce::AudioBuffer<float> whole (channels, outTotal);
        whole.clear();

        const auto inputsFrom = [&padded] (int offset)
        {
            std::vector<const float*> p;

            for (int ch = 0; ch < padded.getNumChannels(); ++ch)
                p.push_back (padded.getReadPointer (ch) + offset);

            return p;
        };

        const auto outputsFrom = [&whole] (int offset)
        {
            std::vector<float*> p;

            for (int ch = 0; ch < whole.getNumChannels(); ++ch)
                p.push_back (whole.getWritePointer (ch) + offset);

            return p;
        };

        stretcher.outputSeek (inputsFrom (0), seek);
        const auto outputEnd = outTotal - (int) ((float) seek / rate);
        const auto inputCount = inTotal - seek;

        // 8192 samples of output at a time (a sixth of a second at 48 kHz), so stopping is soon
        // heard; the input to each piece in proportion.
        const auto pieces = juce::jmax (1, outputEnd / 8192);

        for (int k = 0; k < pieces; ++k)
        {
            if (progress != nullptr && ! progress ((float) k / (float) pieces))
                return {};

            const auto inFrom = (int) ((juce::int64) inputCount * k / pieces), inTo = (int) ((juce::int64) inputCount * (k + 1) / pieces);
            const auto outFrom = (int) ((juce::int64) outputEnd * k / pieces), outTo = (int) ((juce::int64) outputEnd * (k + 1) / pieces);
            stretcher.process (inputsFrom (seek + inFrom), inTo - inFrom, outputsFrom (outFrom), outTo - outFrom);
        }

        stretcher.flush (outputsFrom (outputEnd), outTotal - outputEnd, rate);

        // The part that came from the input itself.
        const auto first = juce::jlimit (0, outTotal - outLength, (int) std::llround ((double) pad * ratio));

        for (int ch = 0; ch < channels; ++ch)
            out.copyFrom (ch, 0, whole, ch, first, outLength);

        return out;
    }

    void setUp (Stretcher& stretcher, int channels, double sampleRate)
    {
        stretcher.presetDefault (channels, (float) sampleRate);
    }
}

juce::AudioBuffer<float> stretch (const juce::AudioBuffer<float>& input, double ratio, double sampleRate,
                                  const std::function<bool (float)>& progress)
{
    ratio = juce::jlimit (shortest, longest, ratio);
    Stretcher stretcher;
    setUp (stretcher, juce::jmax (1, input.getNumChannels()), sampleRate);
    return run (stretcher, input, ratio, progress);
}

juce::AudioBuffer<float> shiftPitch (const juce::AudioBuffer<float>& input, double semitones, double sampleRate,
                                     const std::function<bool (float)>& progress)
{
    Stretcher stretcher;
    setUp (stretcher, juce::jmax (1, input.getNumChannels()), sampleRate);
    stretcher.setTransposeSemitones ((float) juce::jlimit (-widestShift, widestShift, semitones));
    return run (stretcher, input, 1.0, progress);
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
