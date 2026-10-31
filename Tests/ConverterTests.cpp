#include <JuceHeader.h>
#include "../Source/SampleRateConverter.h"
#include "TestUtils.h"
#include "ConverterTests.h"

namespace
{
    using TestUtils::check;

    constexpr double twoPi = juce::MathConstants<double>::twoPi;

    juce::AudioBuffer<float> sine (double rate, double frequency, double amplitude, int frames, int channels = 1)
    {
        juce::AudioBuffer<float> b (channels, frames);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, (float) (amplitude * std::sin (twoPi * frequency * i / rate)));

        return b;
    }

    /** Error of a converted sine against the ideal one, in dB below the signal, away from the edges. */
    double snrDb (const juce::AudioBuffer<float>& out, double rate, double frequency, double amplitude, int margin)
    {
        double signal = 0.0, noise = 0.0;

        for (int i = margin; i < out.getNumSamples() - margin; ++i)
        {
            const auto ideal = amplitude * std::sin (twoPi * frequency * i / rate);
            signal += ideal * ideal;
            noise += juce::square ((double) out.getSample (0, i) - ideal);
        }

        return 10.0 * std::log10 (signal / juce::jmax (noise, 1.0e-300));
    }

    /** Amplitude of the best-fitting sine at frequency (least squares on sin and cos). */
    double fitAmplitude (const juce::AudioBuffer<float>& out, double rate, double frequency, int margin)
    {
        double ss = 0.0, cc = 0.0, sc = 0.0, ys = 0.0, yc = 0.0;

        for (int i = margin; i < out.getNumSamples() - margin; ++i)
        {
            const auto s = std::sin (twoPi * frequency * i / rate), c = std::cos (twoPi * frequency * i / rate);
            const auto y = (double) out.getSample (0, i);
            ss += s * s; cc += c * c; sc += s * c; ys += y * s; yc += y * c;
        }

        const auto det = ss * cc - sc * sc;
        const auto a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
        return std::sqrt (a * a + b * b);
    }

    double rmsDb (const juce::AudioBuffer<float>& out, int margin)
    {
        double sum = 0.0;
        int n = 0;

        for (int i = margin; i < out.getNumSamples() - margin; ++i, ++n)
            sum += juce::square ((double) out.getSample (0, i));

        return 10.0 * std::log10 (juce::jmax (sum / juce::jmax (1, n), 1.0e-300));
    }

    juce::String db (double v)                    { return juce::String (v, 1) + " dB"; }

    void testAccuracy()
    {
        const auto amplitude = std::pow (10.0, -1.0 / 20.0);   // -1 dBFS
        juce::AudioBuffer<float> out;

        for (auto [from, to, freq] : { std::tuple { 44100.0, 48000.0, 1000.0 }, std::tuple { 48000.0, 44100.0, 1000.0 },
                                       std::tuple { 44100.0, 96000.0, 15000.0 }, std::tuple { 96000.0, 44100.0, 5000.0 } })
        {
            SampleRateConverter::convert (sine (from, freq, amplitude, (int) from * 2), from, to, out);
            const auto snr = snrDb (out, to, freq, amplitude, (int) to / 20);
            check (snr > 130.0, "convert " + juce::String (from / 1000.0, 1) + " -> " + juce::String (to / 1000.0, 1) + " kHz, "
                                   + juce::String (freq / 1000.0, 0) + " kHz sine: error " + db (snr) + " below the signal");
        }

        check (SampleRateConverter::getOutputLength (147, 44100.0, 48000.0) == 160
                   && SampleRateConverter::getOutputLength (148, 44100.0, 48000.0) == 162,
               "convert: output length is the input length scaled, rounded up");
    }

    void testPassbandAndAliasing()
    {
        juce::AudioBuffer<float> out;
        double worst = 0.0;

        // Flat to 20 kHz at 44.1 <-> 48 kHz in both directions.
        for (auto freq : { 100.0, 1000.0, 10000.0, 18000.0, 20000.0 })
        {
            for (auto [from, to] : { std::pair { 44100.0, 48000.0 }, std::pair { 48000.0, 44100.0 } })
            {
                SampleRateConverter::convert (sine (from, freq, 0.5, (int) from), from, to, out);
                worst = juce::jmax (worst, std::abs (juce::Decibels::gainToDecibels (fitAmplitude (out, to, freq, 2000) / 0.5)));
            }
        }

        check (worst < 0.001, "convert: passband flat to 20 kHz (worst deviation " + juce::String (worst, 6) + " dB)");

        // Above the output's Nyquist frequency nothing may fold back.
        double leak = -1000.0;

        for (auto freq : { 22200.0, 23000.0, 30000.0 })
        {
            SampleRateConverter::convert (sine (48000.0 * (freq > 24000.0 ? 2.0 : 1.0), freq, 1.0, 96000), 48000.0 * (freq > 24000.0 ? 2.0 : 1.0), 44100.0, out);
            leak = juce::jmax (leak, rmsDb (out, 2000) - juce::Decibels::gainToDecibels (std::sqrt (0.5)));
        }

        check (leak < -135.0, "convert: alias rejection above 22.05 kHz going to 44.1 kHz (worst " + db (leak) + ")");
    }

    void testRoundTrip()
    {
        const auto in = sine (44100.0, 997.0, 0.5, 88200);
        juce::AudioBuffer<float> up, back;
        SampleRateConverter::convert (in, 44100.0, 96000.0, up);
        SampleRateConverter::convert (up, 96000.0, 44100.0, back);

        double signal = 0.0, noise = 0.0;

        for (int i = 2000; i < in.getNumSamples() - 2000; ++i)
        {
            signal += juce::square ((double) in.getSample (0, i));
            noise += juce::square ((double) back.getSample (0, i) - in.getSample (0, i));
        }

        const auto depth = 10.0 * std::log10 (signal / juce::jmax (noise, 1.0e-300));
        check (back.getNumSamples() == in.getNumSamples() && depth > 125.0,
               "convert: 44.1 -> 96 -> 44.1 kHz nulls against the original to " + db (depth));
    }

    void testInexactRatio()
    {
        // A rate that is not a whole number takes the interpolated-phase path.
        juce::AudioBuffer<float> out;
        const auto to = 47999.75;
        SampleRateConverter::convert (sine (44100.0, 1000.0, 0.5, 88200), 44100.0, to, out);
        const auto snr = snrDb (out, to, 1000.0, 0.5, 2000);
        check (snr > 120.0, "convert: an inexact ratio (44.1 -> 47.99975 kHz) stays accurate: " + db (snr));
    }

    void testSnapshotAndMarkers()
    {
        auto buffer = sine (48000.0, 440.0, 0.5, 3 * (1 << 20) / 2, 2);   // spans two storage blocks
        const auto source = AudioEdits::fromBuffer (std::move (buffer));
        const auto converted = SampleRateConverter::convert (*source, 48000.0, 44100.0);
        check (converted != nullptr && converted->getNumChannels() == 2
                   && converted->getNumFrames() == SampleRateConverter::getOutputLength (source->getNumFrames(), 48000.0, 44100.0),
               "convert: a snapshot keeps its channels and gets the scaled length");

        const auto markers = SampleRateConverter::convert ({ { 48000, 0, "a" }, { 96000, 24000, "r" } }, 48000.0, 44100.0);
        check (markers[0].start == 44100 && markers[1].start == 88200 && markers[1].length == 22050,
               "convert: markers move to the same times at the new rate");

        int calls = 0;
        const auto cancelled = SampleRateConverter::convert (*source, 48000.0, 96000.0, [&] (float) { return ++calls < 2; });
        check (cancelled == nullptr, "convert: cancelling returns nothing");
    }

    void testUndoRate()
    {
        AudioDocument doc;
        const auto source = AudioEdits::fromBuffer (sine (48000.0, 440.0, 0.5, 48000));
        doc.reset (source, 48000.0, {}, 24, { { 24000, 0, "half" } });
        doc.applyNewRate (SampleRateConverter::convert (*source, 48000.0, 44100.0), 44100.0,
                          SampleRateConverter::convert (doc.getMarkers(), 48000.0, 44100.0), "Convert");
        const auto converted = juce::exactlyEqual (doc.getSampleRate(), 44100.0) && doc.getNumFrames() == 44100
                               && doc.getMarkers()[0].start == 22050;
        doc.undo();
        const auto undone = juce::exactlyEqual (doc.getSampleRate(), 48000.0) && doc.getNumFrames() == 48000
                            && doc.getMarkers()[0].start == 24000 && ! doc.isDirty();
        doc.redo();
        check (converted && undone && juce::exactlyEqual (doc.getSampleRate(), 44100.0),
               "convert: the rate is part of the undo history");
    }

    void testSpeed()
    {
        const auto in = sine (44100.0, 1000.0, 0.5, 44100 * 60, 2);
        juce::AudioBuffer<float> out;
        const auto start = juce::Time::getMillisecondCounterHiRes();
        SampleRateConverter::convert (in, 44100.0, 48000.0, out);
        const auto seconds = (juce::Time::getMillisecondCounterHiRes() - start) / 1000.0;
        std::cout << "      60 s of stereo, 44.1 -> 48 kHz: " << seconds << " s (" << 60.0 / seconds << "x real time)" << std::endl;
        check (seconds < 10.0, "convert: a minute of stereo converts in " + juce::String (seconds, 2) + " s");
    }
}

void runConverterTests()
{
    testAccuracy();
    testPassbandAndAliasing();
    testRoundTrip();
    testInexactRatio();
    testSnapshotAndMarkers();
    testUndoRate();
    testSpeed();
}
