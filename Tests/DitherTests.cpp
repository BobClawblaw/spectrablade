#include <JuceHeader.h>
#include "../Source/Dither.h"
#include "../Source/Analysis.h"
#include "TestUtils.h"
#include "DitherTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    juce::AudioBuffer<float> applied (const juce::AudioBuffer<float>& in, const Dither::Settings& s)
    {
        return readAll (*Dither::apply (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (in)), 0, in.getNumSamples(), s));
    }

    /** A quiet 1 kHz sine, 1.2 LSB at 16 bits, so plain rounding would mangle it. */
    juce::AudioBuffer<float> quietSine()
    {
        juce::AudioBuffer<float> b (1, 4 * Analysis::fftSize);

        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, (float) (1.2 / 32768.0 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / rate)));

        return b;
    }

    /** The average power spectrum of the audio's windows, in dB by bin. */
    std::vector<float> spectrumOf (const juce::AudioBuffer<float>& b)
    {
        std::vector<double> power ((size_t) (Analysis::fftSize / 2 + 1), 0.0);
        auto windows = 0;

        for (int pos = 0; pos + Analysis::fftSize <= b.getNumSamples(); pos += Analysis::fftSize / 2)
        {
            const auto db = Analysis::spectrumDb (b.getReadPointer (0, pos), Analysis::fftSize);

            for (size_t k = 0; k < power.size(); ++k)
                power[k] += std::pow (10.0, db[k] / 10.0);

            ++windows;
        }

        std::vector<float> out (power.size());

        for (size_t k = 0; k < power.size(); ++k)
            out[k] = (float) (10.0 * std::log10 (power[k] / windows));

        return out;
    }

    /** The mean level of the bins round a frequency (within a sixth of an octave), in dB; or the loudest of them. */
    double levelNear (const std::vector<float>& spectrum, double hz, bool loudest = false)
    {
        const auto binHz = rate / Analysis::fftSize;
        const auto from = (int) (hz / std::pow (2.0, 1.0 / 12.0) / binHz), to = (int) (hz * std::pow (2.0, 1.0 / 12.0) / binHz);
        auto sum = 0.0, peak = -200.0;

        for (int k = from; k <= to; ++k)
        {
            sum += std::pow (10.0, spectrum[(size_t) k] / 10.0);
            peak = juce::jmax (peak, (double) spectrum[(size_t) k]);
        }

        return loudest ? peak : 10.0 * std::log10 (sum / (to - from + 1));
    }
}

void runDitherTests()
{
    std::cout << "Dither:" << std::endl;

    // Every sample a multiple of the step within full scale; the same twice over; a quiet sine's
    // harmonics are noise, not distortion; shaping moves the noise up; refused settings.
    {
        const auto in = quietSine();
        Dither::Settings flat { 16, Dither::Shaping::none }, light { 16, Dither::Shaping::light }, strong { 16, Dither::Shaping::strong }, deep { 24, Dither::Shaping::light };
        const auto outFlat = applied (in, flat), outLight = applied (in, light), outStrong = applied (in, strong), outDeep = applied (in, deep), again = applied (in, light);
        auto multiples = true, same = true, withinScale = true;

        for (int i = 0; i < in.getNumSamples(); ++i)
        {
            const auto v16 = (double) outLight.getSample (0, i) * 32768.0, v24 = (double) outDeep.getSample (0, i) * 8388608.0;
            multiples = multiples && std::abs (v16 - std::round (v16)) < 1.0e-6 && std::abs (v24 - std::round (v24)) < 1.0e-3;
            same = same && juce::exactlyEqual (outLight.getSample (0, i), again.getSample (0, i));
            withinScale = withinScale && outLight.getSample (0, i) <= 1.0f - 1.0f / 32768.0f && outLight.getSample (0, i) >= -1.0f;
        }

        // Plain rounding of the same sine, for the distortion it makes.
        juce::AudioBuffer<float> truncated (in);

        for (int i = 0; i < in.getNumSamples(); ++i)
            truncated.setSample (0, i, (float) (std::round (in.getSample (0, i) * 32768.0) / 32768.0));

        const auto sFlat = spectrumOf (outFlat), sLight = spectrumOf (outLight), sStrong = spectrumOf (outStrong), sTruncated = spectrumOf (truncated);
        const auto harmonicTruncated = levelNear (sTruncated, 3000.0, true) - levelNear (sTruncated, 1000.0, true), harmonicDithered = levelNear (sFlat, 3000.0, true) - levelNear (sFlat, 1000.0, true);
        const auto noiseFloorFlat = levelNear (sFlat, 2500.0), noiseFlat15k = levelNear (sFlat, 15000.0);
        const auto noiseLight2k = levelNear (sLight, 2500.0), noiseLight15k = levelNear (sLight, 15000.0);
        const auto noiseStrong2k = levelNear (sStrong, 2500.0), noiseStrong15k = levelNear (sStrong, 15000.0);
        const auto fundamentalKept = levelNear (sLight, 1000.0, true) - levelNear (spectrumOf (in), 1000.0, true);

        Dither::Settings bad { 20, Dither::Shaping::none };
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        const auto refused = Dither::apply (*snap, 0, 1000, bad) == nullptr && Dither::apply (*snap, 100, 100, light) == nullptr;

        std::cout << "      a 1.2 LSB sine: the 3rd harmonic " << harmonicTruncated << " dB below it when rounded plainly, " << harmonicDithered << " dithered; the noise at 2.5 kHz / 15 kHz: flat "
                  << noiseFloorFlat << " / " << noiseFlat15k << " dB, light " << noiseLight2k << " / " << noiseLight15k << ", strong " << noiseStrong2k << " / " << noiseStrong15k
                  << "; the sine itself moved " << fundamentalKept << " dB" << std::endl;
        check (multiples && same && withinScale && harmonicTruncated > -28.0 && harmonicDithered < harmonicTruncated - 8.0 && std::abs (fundamentalKept) < 0.5
                   && noiseLight2k < noiseFloorFlat - 6.0 && noiseLight15k > noiseFlat15k + 6.0 && noiseStrong2k < noiseLight2k - 3.0 && noiseStrong15k > noiseLight15k + 3.0
                   && refused,
               "dither: every sample is a multiple of the step (16 and 24 bits) within full scale, and the same twice over; a 1.2 LSB sine rounded plainly "
               "has a 3rd harmonic within 28 dB of it, dithered over 8 dB less; the sine keeps its level within 0.5 dB; Light shaping lowers the noise "
               "at 2.5 kHz by over 6 dB and raises it at 15 kHz by over 6, Strong by over 3 more each way; 20 bits and an empty range are refused");
    }
}
