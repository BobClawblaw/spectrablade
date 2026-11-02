#include "Convolver.h"
#include "SampleRateConverter.h"

namespace Convolver
{
namespace
{
    constexpr int fftOrder = 10;               // 1,024 points: a block and the one before it
    constexpr int fftSize = 1 << fftOrder;
    static_assert (fftSize == 2 * blockSize);

    // Bins kept: 0 to fftSize / 2, each a (real, imaginary) pair.
    constexpr int bins = fftSize / 2 + 1;
}

juce::String problemWith (const Impulse& i)
{
    const auto channels = i.samples.getNumChannels(), frames = i.samples.getNumSamples();

    if (channels < 1 || channels > 2)
        return "An impulse response must be mono or stereo.";

    if (i.sampleRate <= 0.0 || frames < 1)
        return "The impulse response is empty.";

    if (frames > maxSeconds * i.sampleRate)
        return "An impulse response can be at most " + juce::String (maxSeconds, 0) + " seconds long.";

    for (int ch = 0; ch < channels; ++ch)
        if (i.samples.getMagnitude (ch, 0, frames) > 0.0f)
            return {};

    return "The impulse response is silent.";
}

Impulse read (const juce::File& file, juce::AudioFormatManager& formats, juce::String& error)
{
    Impulse i;
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
    {
        error = file.getFileName() + " isn't an audio file SpectraBlade can read.";
        return {};
    }

    if (reader->numChannels < 1 || reader->numChannels > 2 || reader->lengthInSamples > (juce::int64) (maxSeconds * reader->sampleRate))
    {
        error = "An impulse response must be mono or stereo and at most " + juce::String (maxSeconds, 0) + " seconds long.";
        return {};
    }

    i.samples.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&i.samples, 0, (int) reader->lengthInSamples, 0, true, true);
    i.sampleRate = reader->sampleRate;
    i.name = file.getFileNameWithoutExtension();
    error = problemWith (i);
    return error.isEmpty() ? std::move (i) : Impulse {};
}

juce::AudioBuffer<float> atRate (const Impulse& i, double sampleRate)
{
    juce::AudioBuffer<float> out;

    if (! problemWith (i).isEmpty() || sampleRate <= 0.0)
        return out;

    if (juce::exactlyEqual (i.sampleRate, sampleRate))
        out.makeCopyOf (i.samples);
    else
        SampleRateConverter::convert (i.samples, i.sampleRate, sampleRate, out);

    double energy = 0.0;

    for (int ch = 0; ch < out.getNumChannels(); ++ch)
    {
        double e = 0.0;

        for (int n = 0; n < out.getNumSamples(); ++n)
            e += (double) out.getSample (ch, n) * out.getSample (ch, n);

        energy = juce::jmax (energy, e);
    }

    if (energy > 0.0)
        out.applyGain ((float) (1.0 / std::sqrt (energy)));

    return out;
}

//==============================================================================
Core::Core() : fft (fftOrder) {}
Core::~Core() = default;

void Core::prepare (const juce::AudioBuffer<float>& impulse, int numChannels)
{
    const auto frames = impulse.getNumSamples();
    partitions = frames > 0 ? (frames + blockSize - 1) / blockSize : 0;
    work.assign ((size_t) (2 * fftSize), 0.0f);
    sum.assign ((size_t) (2 * fftSize), 0.0f);

    // Each block of the impulse, zero-padded to the FFT's size, as a spectrum.
    spectra.assign ((size_t) juce::jmax (1, impulse.getNumChannels()), {});

    for (int ch = 0; ch < impulse.getNumChannels(); ++ch)
    {
        auto& parts = spectra[(size_t) ch];
        parts.assign ((size_t) partitions, std::vector<float> ((size_t) (2 * bins)));

        for (int p = 0; p < partitions; ++p)
        {
            std::fill (work.begin(), work.end(), 0.0f);
            const auto from = p * blockSize, count = juce::jmin (blockSize, frames - from);
            std::copy_n (impulse.getReadPointer (ch, from), count, work.begin());
            fft.performRealOnlyForwardTransform (work.data(), true);
            std::copy_n (work.begin(), 2 * bins, parts[(size_t) p].begin());
        }
    }

    channels.assign ((size_t) juce::jmax (1, numChannels), {});

    for (size_t c = 0; c < channels.size(); ++c)
    {
        auto& ch = channels[c];
        ch.input.assign ((size_t) blockSize, 0.0f);
        ch.previous.assign ((size_t) blockSize, 0.0f);
        ch.output.assign ((size_t) blockSize, 0.0f);
        ch.dry.assign ((size_t) blockSize, 0.0f);
        ch.history.assign ((size_t) partitions, std::vector<float> ((size_t) (2 * bins), 0.0f));
        ch.impulseChannel = (int) c % juce::jmax (1, impulse.getNumChannels());
    }

    reset();
}

void Core::reset()
{
    for (auto& ch : channels)
    {
        for (auto* v : { &ch.input, &ch.previous, &ch.output, &ch.dry })
            std::fill (v->begin(), v->end(), 0.0f);

        for (auto& h : ch.history)
            std::fill (h.begin(), h.end(), 0.0f);
    }

    pos = 0;
    fdlPos = 0;
}

void Core::process (float* const* data, int numChannels, int numFrames) noexcept
{
    numChannels = juce::jmin (numChannels, (int) channels.size());
    const auto wet = mix, dry = 1.0 - mix;

    for (int i = 0; i < numFrames; ++i)
    {
        for (int c = 0; c < numChannels; ++c)
        {
            auto& ch = channels[(size_t) c];
            const auto x = data[c][i];
            const auto late = (double) ch.dry[(size_t) pos];
            ch.dry[(size_t) pos] = x;
            ch.input[(size_t) pos] = x;
            data[c][i] = (float) (dry * late + wet * (double) ch.output[(size_t) pos]);
        }

        if (++pos == blockSize)
        {
            for (int c = 0; c < numChannels; ++c)
                convolveBlock (c);

            pos = 0;
            fdlPos = partitions > 0 ? (fdlPos + 1) % partitions : 0;
        }
    }
}

void Core::convolveBlock (int c) noexcept
{
    auto& ch = channels[(size_t) c];

    if (partitions == 0)
    {
        std::fill (ch.output.begin(), ch.output.end(), 0.0f);
        ch.previous.swap (ch.input);
        return;
    }

    // Overlap-save: the spectrum of the last two blocks of input, kept in the history.
    std::fill (work.begin(), work.end(), 0.0f);
    std::copy (ch.previous.begin(), ch.previous.end(), work.begin());
    std::copy (ch.input.begin(), ch.input.end(), work.begin() + blockSize);
    fft.performRealOnlyForwardTransform (work.data(), true);
    std::copy_n (work.begin(), 2 * bins, ch.history[(size_t) fdlPos].begin());

    // The sum over the impulse's blocks of each one times the input spectrum that many blocks ago.
    std::fill (sum.begin(), sum.end(), 0.0f);
    const auto& parts = spectra[(size_t) ch.impulseChannel];

    for (int p = 0; p < partitions; ++p)
    {
        const auto* x = ch.history[(size_t) ((fdlPos - p + partitions) % partitions)].data();
        const auto* h = parts[(size_t) p].data();
        auto* s = sum.data();

        for (int k = 0; k < 2 * bins; k += 2)
        {
            s[k]     += x[k] * h[k]     - x[k + 1] * h[k + 1];
            s[k + 1] += x[k] * h[k + 1] + x[k + 1] * h[k];
        }
    }

    // Back to time; its second half is this block's output (the first half wraps around).
    fft.performRealOnlyInverseTransform (sum.data());
    std::copy_n (sum.begin() + blockSize, blockSize, ch.output.begin());
    ch.previous.swap (ch.input);
}

//==============================================================================
AudioEdits::Snapshot apply (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Impulse& impulse, double mixPercent)
{
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (problemWith (impulse).isNotEmpty() || sampleRate <= 0.0 || start == end || ! (mixPercent >= 0.0 && mixPercent <= 100.0))
        return nullptr;

    const auto shaped = atRate (impulse, sampleRate);
    const auto tailEnd = juce::jmin (s.getNumFrames(), end + shaped.getNumSamples());

    if (tailEnd - start + blockSize > std::numeric_limits<int>::max())
        return nullptr;

    const auto inside = (int) (end - start);

    return AudioEdits::processRange (s, start, tailEnd, [&shaped, mixPercent, inside] (juce::AudioBuffer<float>& b)
    {
        // The range, then silence for the tail and the core's block of delay.
        const auto total = b.getNumSamples();
        juce::AudioBuffer<float> w (b.getNumChannels(), total + blockSize);
        w.clear();

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            w.copyFrom (ch, 0, b, ch, 0, inside);

        Core core;
        core.prepare (shaped, b.getNumChannels());
        core.setMix (mixPercent);
        core.process (w.getArrayOfWritePointers(), w.getNumChannels(), w.getNumSamples());

        // A block late: the range replaced, the tail added on top of what follows.
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            b.copyFrom (ch, 0, w, ch, blockSize, inside);

            if (total > inside)
                b.addFrom (ch, inside, w, ch, blockSize + inside, total - inside);
        }
    });
}
}
