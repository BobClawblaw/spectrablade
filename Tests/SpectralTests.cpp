// Samples outside an edit are compared bit for bit on purpose.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/AudioEdits.h"
#include "../Source/SpectralEdit.h"
#include "../Source/Spectrogram.h"
#include "TestUtils.h"
#include "SpectralTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;
    constexpr double twoPi = juce::MathConstants<double>::twoPi;

    // Tones centred on analysis bins (4096 points at 48 kHz), so level readings carry no scalloping.
    constexpr double low = 85 * rate / 4096;    // 996.1 Hz
    constexpr double high = 427 * rate / 4096;  // 5003.9 Hz

    double toDb (double gain)                     { return 20.0 * std::log10 (juce::jmax (gain, 1.0e-30)); }

    /** ~1 kHz at -6 dBFS plus ~5 kHz at -12 dBFS, stereo, 4 s. */
    juce::AudioBuffer<float> twoTones()
    {
        juce::AudioBuffer<float> b (2, (int) rate * 4);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (ch, i, (float) (0.5 * std::sin (twoPi * low * i / rate) + 0.25 * std::sin (twoPi * high * i / rate)));

        return b;
    }

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    juce::String db (double v)                    { return juce::String (v, 2) + " dB"; }

    void testCalibration()
    {
        for (auto level : { 0.0, -40.0, -100.0 })
        {
            juce::AudioBuffer<float> b (1, 16384);

            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (0, i, (float) (std::pow (10.0, level / 20.0) * std::sin (twoPi * low * i / rate)));

            const auto measured = Spectrogram::measurePeakDb (b, 0, 8192, rate, low);
            check (std::abs (measured - level) < 0.1, "spectrogram: a " + juce::String (level, 0) + " dBFS sine reads " + db (measured));
        }

        // The image: the 1 kHz row is bright, a row between the tones is dark.
        const auto snapshot = AudioEdits::fromBuffer (twoTones());
        const auto image = Spectrogram::render (*snapshot, 0, rate, 0.0, 400.0, 200, 300);
        const auto at = [&] (double f) { return image.getPixelAt (100, (int) Spectrogram::frequencyToY (f, rate, 300)).getBrightness(); };
        check (image.getWidth() == 200 && image.getHeight() == 300 && at (1000.0) > 0.8f && at (2500.0) < 0.3f,
               "spectrogram: tones show as bright rows (1 kHz " + juce::String (at (1000.0), 2) + ", 2.5 kHz "
                   + juce::String (at (2500.0), 2) + ")");
    }

    void testIdentity()
    {
        juce::AudioBuffer<float> noise (2, (int) rate * 2);
        juce::Random random (7);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < noise.getNumSamples(); ++i)
                noise.setSample (ch, i, random.nextFloat() * 2.0f - 1.0f);

        const auto source = AudioEdits::fromBuffer (juce::AudioBuffer<float> (noise));
        const auto result = readAll (*SpectralEdit::changeBand (*source, 30000, 60000, { 100.0, 20000.0 }, rate, 0.0f));
        float worst = 0.0f;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < noise.getNumSamples(); ++i)
                worst = juce::jmax (worst, std::abs (result.getSample (ch, i) - noise.getSample (ch, i)));

        check (toDb (worst) < -120.0, "spectral: a 0 dB change gives the input back (worst error " + db (toDb (worst)) + ")");
    }

    void testRemoveAndAttenuate()
    {
        const auto input = twoTones();
        const auto source = AudioEdits::fromBuffer (juce::AudioBuffer<float> (input));
        const juce::int64 start = 48000, end = 144000;   // 1 s to 3 s

        // Remove 3-8 kHz: the 5 kHz tone goes, 1 kHz stays.
        const auto removed = readAll (*SpectralEdit::changeBand (*source, start, end, { 3000.0, 8000.0 }, rate, -std::numeric_limits<float>::infinity()));
        const auto mid = 96000;
        const auto fiveRemoved = Spectrogram::measurePeakDb (removed, 0, mid, rate, high);
        const auto oneKept = Spectrogram::measurePeakDb (removed, 0, mid, rate, low);
        check (fiveRemoved < -100.0f, "spectral: removing 3-8 kHz takes the 5 kHz tone down to " + db (fiveRemoved)
                                          + (fiveRemoved <= Spectrogram::floorDb ? " (the analysis floor)" : ""));
        check (std::abs (oneKept - -6.02f) < 0.05f, "spectral: the 1 kHz tone outside the band stays at " + db (oneKept));

        bool untouched = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < (int) start - 4096; ++i)
                untouched = untouched && removed.getSample (ch, i) == input.getSample (ch, i);

        check (untouched && std::abs (Spectrogram::measurePeakDb (removed, 0, 12000, rate, high) - -12.04f) < 0.05f,
               "spectral: audio before the selection is untouched");

        const auto attenuated = readAll (*SpectralEdit::changeBand (*source, start, end, { 3000.0, 8000.0 }, rate, -12.0f));
        const auto drop = Spectrogram::measurePeakDb (input, 0, mid, rate, high) - Spectrogram::measurePeakDb (attenuated, 0, mid, rate, high);
        check (std::abs (drop - 12.0f) < 0.05f, "spectral: attenuating by 12 dB lowers the tone by " + db (drop));
    }

    void testKeepBand()
    {
        // What plays with a box selected: everything outside 3-8 kHz over 1 s to 3 s removed, so
        // the 5 kHz tone stays, the 1 kHz tone goes, and the audio before the selection is untouched.
        const auto input = twoTones();
        const auto source = AudioEdits::fromBuffer (juce::AudioBuffer<float> (input));
        const juce::int64 start = 48000, end = 144000;
        const auto kept = readAll (*SpectralEdit::changeOutsideBand (*source, start, end, { 3000.0, 8000.0 }, rate, -std::numeric_limits<float>::infinity()));
        const auto mid = 96000;
        const auto five = Spectrogram::measurePeakDb (kept, 0, mid, rate, high);
        const auto one = Spectrogram::measurePeakDb (kept, 0, mid, rate, low);
        bool untouched = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < (int) start - 4096; ++i)
                untouched = untouched && kept.getSample (ch, i) == input.getSample (ch, i);

        check (std::abs (five - -12.04f) < 0.05f && one < -100.0f && untouched,
               "spectral: keeping only 3-8 kHz leaves the 5 kHz tone at " + db (five) + ", takes the 1 kHz tone outside the band down to " + db (one)
                   + (one <= Spectrogram::floorDb ? " (the analysis floor)" : "") + ", and leaves the audio before the selection untouched");
    }
}

void runSpectralTests()
{
    testCalibration();
    testIdentity();
    testRemoveAndAttenuate();
    testKeepBand();
}
