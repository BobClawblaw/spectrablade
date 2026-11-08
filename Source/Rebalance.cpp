#include "Rebalance.h"
#include "AudioEdits.h"
#include "DialogueIsolation.h"

namespace Rebalance
{
namespace
{
    constexpr int fftOrder = 12, size = 1 << fftOrder, hop = size / 4, bins = size / 2 + 1, medianReach = 8;

    std::vector<float> rootHann()
    {
        std::vector<float> w ((size_t) size);

        for (int i = 0; i < size; ++i)
            w[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / size));

        return w;
    }

    float medianOf (std::vector<float>& v)
    {
        if (v.empty())
            return 0.0f;

        std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
        return v[v.size() / 2];
    }
}

bool isValid (const Settings& s)
{
    for (auto db : { s.voiceDb, s.bassDb, s.drumsDb, s.restDb })
        if (db < -40.0 || db > 12.0)
            return false;

    return s.bassCornerHz >= 60.0 && s.bassCornerHz <= 300.0;
}

AudioDocument::Snapshot apply (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const Settings& settings,
                               const std::function<bool (float)>& progress)
{
    if (! isValid (settings) || sampleRate <= 0.0)
        return nullptr;

    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (end - start < size || end - start > std::numeric_limits<int>::max() / 8)
        return nullptr;

    const auto channels = s.getNumChannels();
    const auto frames = (int) (end - start);
    const auto window = rootHann();
    const auto binHz = sampleRate / size;
    const auto padded = frames + 2 * size;
    juce::AudioBuffer<float> in (channels, padded), out (channels, padded);
    in.clear();
    out.clear();
    s.read (in, size, start, frames);
    const auto steps = (padded - size) / hop + 1;

    // A ring of the frames round the one being shaped (the medians across time need them), each
    // holding every channel's spectrum and magnitude and the mix's magnitude.
    constexpr int ring = 2 * medianReach + 1;
    struct Frame
    {
        std::vector<std::vector<float>> spectrum, magnitude;   // a channel each
        std::vector<float> mixMagnitude;
    };
    std::vector<Frame> frameRing ((size_t) ring);

    for (auto& f : frameRing)
    {
        f.spectrum.assign ((size_t) channels, std::vector<float> ((size_t) (2 * bins)));
        f.magnitude.assign ((size_t) channels, std::vector<float> ((size_t) bins));
        f.mixMagnitude.assign ((size_t) bins, 0.0f);
    }

    juce::dsp::FFT fft (fftOrder);
    std::vector<float> data ((size_t) (2 * size));
    const auto gains = std::array<double, 4> { std::pow (10.0, settings.voiceDb / 20.0), std::pow (10.0, settings.bassDb / 20.0),
                                               std::pow (10.0, settings.drumsDb / 20.0), std::pow (10.0, settings.restDb / 20.0) };
    const auto threshold = DialogueIsolation::thresholdFor (50.0f);
    const auto eightyHz = (int) std::ceil (80.0 / binHz), fourK = juce::jmin (bins - 1, (int) (4000.0 / binHz)), eightK = juce::jmin (bins - 1, (int) (8000.0 / binHz));
    std::vector<float> acrossTime, acrossFrequency, power ((size_t) bins), voiceMask ((size_t) bins), bassMask ((size_t) bins);

    for (int k = 0; k < bins; ++k)
    {
        // The bass: all below the corner, fading out over half an octave above it.
        const auto hz = k * binHz;
        bassMask[(size_t) k] = (float) (hz <= settings.bassCornerHz ? 1.0 : juce::jlimit (0.0, 1.0, 1.0 - std::log2 (hz / settings.bassCornerHz) / 0.5));
    }

    const auto analyse = [&] (int step)
    {
        auto& f = frameRing[(size_t) (step % ring)];
        const auto pos = step * hop;
        std::vector<float> mixRe ((size_t) bins, 0.0f), mixIm ((size_t) bins, 0.0f);

        for (int ch = 0; ch < channels; ++ch)
        {
            std::fill (data.begin(), data.end(), 0.0f);

            for (int i = 0; i < size; ++i)
                data[(size_t) i] = in.getSample (ch, pos + i) * window[(size_t) i];

            fft.performRealOnlyForwardTransform (data.data(), true);
            std::copy_n (data.begin(), 2 * bins, f.spectrum[(size_t) ch].begin());

            for (int k = 0; k < bins; ++k)
            {
                const auto re = data[(size_t) (2 * k)], im = data[(size_t) (2 * k + 1)];
                f.magnitude[(size_t) ch][(size_t) k] = std::sqrt (re * re + im * im);
                mixRe[(size_t) k] += re / (float) channels;
                mixIm[(size_t) k] += im / (float) channels;
            }
        }

        for (int k = 0; k < bins; ++k)
            f.mixMagnitude[(size_t) k] = std::sqrt (mixRe[(size_t) k] * mixRe[(size_t) k] + mixIm[(size_t) k] * mixIm[(size_t) k]);
    };

    const auto shape = [&] (int centre, int newest)
    {
        const auto& f = frameRing[(size_t) (centre % ring)];

        for (int k = 0; k < bins; ++k)
            power[(size_t) k] = f.mixMagnitude[(size_t) k] * f.mixMagnitude[(size_t) k];

        // The voice: the harmonics of the pitch, and the highs, in a voiced frame of the mix.
        const auto pitch = DialogueIsolation::pitchOf (power, sampleRate);
        std::fill (voiceMask.begin(), voiceMask.end(), 0.0f);

        if (pitch.voicing >= threshold)
        {
            for (int h = 1; h * pitch.frequency <= 4000.0; ++h)
            {
                const auto middle = h * pitch.frequency;
                const auto from = juce::jmax (eightyHz, (int) std::ceil ((middle - pitch.frequency / 4.0) / binHz)), to = juce::jmin (fourK, (int) ((middle + pitch.frequency / 4.0) / binHz));

                for (int k = from; k <= to; ++k)
                    voiceMask[(size_t) k] = 1.0f;
            }

            for (int k = fourK + 1; k <= eightK; ++k)
                voiceMask[(size_t) k] = 1.0f;
        }

        const auto oldest = juce::jmax (0, centre - medianReach), latest = juce::jmin (newest, centre + medianReach);

        for (int ch = 0; ch < channels; ++ch)
        {
            const auto& spectrum = f.spectrum[(size_t) ch];
            const auto& magnitude = f.magnitude[(size_t) ch];
            std::fill (data.begin(), data.end(), 0.0f);

            for (int k = 0; k < bins; ++k)
            {
                // Percussive against harmonic: this channel's magnitude at this bin across the
                // frames round it, and across the bins round it in this frame.
                acrossTime.clear();
                acrossFrequency.clear();

                for (int j = oldest; j <= latest; ++j)
                    acrossTime.push_back (frameRing[(size_t) (j % ring)].magnitude[(size_t) ch][(size_t) k]);

                for (int j = juce::jmax (0, k - medianReach); j <= juce::jmin (bins - 1, k + medianReach); ++j)
                    acrossFrequency.push_back (magnitude[(size_t) j]);

                const auto harmonic = medianOf (acrossTime), percussive = medianOf (acrossFrequency);
                const auto h2 = (double) harmonic * harmonic, p2 = (double) percussive * percussive;
                const auto drums = h2 + p2 > 0.0 ? p2 / (h2 + p2) : 0.0;
                const auto harmonicShare = 1.0 - drums;

                // Of the harmonic share: the bass below the corner; the voice from the mid's part of
                // this channel's bin; the rest is what is left.
                const auto bass = harmonicShare * bassMask[(size_t) k];
                const auto notBass = harmonicShare - bass;
                auto voice = notBass * voiceMask[(size_t) k];

                if (channels >= 2)
                    voice *= magnitude[(size_t) k] > 0.0f ? juce::jmin (1.0, (double) f.mixMagnitude[(size_t) k] / magnitude[(size_t) k]) : 0.0;

                const auto rest = notBass - voice;
                const auto gain = voice * gains[0] + bass * gains[1] + drums * gains[2] + rest * gains[3];
                data[(size_t) (2 * k)] = (float) (spectrum[(size_t) (2 * k)] * gain);
                data[(size_t) (2 * k + 1)] = (float) (spectrum[(size_t) (2 * k + 1)] * gain);
            }

            fft.performRealOnlyInverseTransform (data.data());
            const auto pos = centre * hop;

            for (int i = 0; i < size; ++i)
                out.addSample (ch, pos + i, data[(size_t) i] * window[(size_t) i] / 2.0f);
        }
    };

    for (int step = 0; step < steps; ++step)
    {
        if (progress && (step % 16) == 0 && ! progress ((float) step / (float) steps))
            return nullptr;

        analyse (step);

        if (step >= medianReach)
            shape (step - medianReach, step);
    }

    for (int centre = juce::jmax (0, steps - medianReach); centre < steps; ++centre)
        shape (centre, steps - 1);

    return AudioEdits::processRange (s, start, end, [&out] (juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            b.copyFrom (ch, 0, out, ch, size, b.getNumSamples());
    });
}
}
