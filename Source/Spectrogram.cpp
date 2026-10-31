#include "Spectrogram.h"

namespace Spectrogram
{
namespace
{
    constexpr int fftOrder = 12;               // 4096 points: 11.7 Hz bins at 48 kHz
    constexpr int fftSize = 1 << fftOrder;

    struct Analyser
    {
        Analyser() : fft (fftOrder), window ((size_t) fftSize), data ((size_t) fftSize * 2)
        {
            // 4-term Blackman-Harris: sidelobes at -92 dB, so quiet detail next to loud tones shows.
            double sum = 0.0;

            for (int i = 0; i < fftSize; ++i)
            {
                const auto p = juce::MathConstants<double>::twoPi * i / (fftSize - 1);
                window[(size_t) i] = (float) (0.35875 - 0.48829 * std::cos (p) + 0.14128 * std::cos (2 * p) - 0.01168 * std::cos (3 * p));
                sum += window[(size_t) i];
            }

            // A full-scale sine peaks at sum/2 in its bin.
            scale = (float) (2.0 / sum);
        }

        /** dB magnitudes of the window centred on frame centre, into db[0 .. fftSize/2]. */
        void analyse (const float* samples, int count, int centre, std::vector<float>& db)
        {
            std::fill (data.begin(), data.end(), 0.0f);
            const auto first = centre - fftSize / 2;

            for (int i = 0; i < fftSize; ++i)
                if (juce::isPositiveAndBelow (first + i, count))
                    data[(size_t) i] = samples[first + i] * window[(size_t) i];

            fft.performFrequencyOnlyForwardTransform (data.data(), true);
            db.resize ((size_t) fftSize / 2 + 1);

            for (int b = 0; b <= fftSize / 2; ++b)
                db[(size_t) b] = juce::Decibels::gainToDecibels (data[(size_t) b] * scale, floorDb);
        }

        juce::dsp::FFT fft;
        std::vector<float> window, data;
        float scale = 1.0f;
    };

    const std::array<juce::PixelARGB, 256>& colourMap()
    {
        static const auto map = []
        {
            juce::ColourGradient g (juce::Colour (0xff000000), 0.0f, 0.0f, juce::Colour (0xffffffff), 1.0f, 0.0f, false);
            g.addColour (0.25, juce::Colour (0xff1a0b4a));
            g.addColour (0.50, juce::Colour (0xff7a1b8c));
            g.addColour (0.72, juce::Colour (0xffe2513a));
            g.addColour (0.90, juce::Colour (0xfffbd35b));
            std::array<juce::PixelARGB, 256> m {};

            for (int i = 0; i < 256; ++i)
                m[(size_t) i] = g.getColourAtPosition (i / 255.0).getPixelARGB();

            return m;
        }();

        return map;
    }
}

float frequencyToY (double frequency, double sampleRate, int height)
{
    const auto nyquist = sampleRate * 0.5;
    const auto t = std::log (juce::jlimit (minFrequency, nyquist, frequency) / minFrequency) / std::log (nyquist / minFrequency);
    return (float) ((1.0 - t) * height);
}

double yToFrequency (float y, double sampleRate, int height)
{
    const auto nyquist = sampleRate * 0.5;
    return minFrequency * std::pow (nyquist / minFrequency, 1.0 - (double) y / juce::jmax (1, height));
}

juce::Image render (const AudioSnapshot& snapshot, int channel, double sampleRate,
                    double viewStart, double framesPerPixel, int width, int height)
{
    juce::Image image (juce::Image::ARGB, juce::jmax (1, width), juce::jmax (1, height), false);
    juce::Image::BitmapData pixels (image, juce::Image::BitmapData::writeOnly);
    const auto& colours = colourMap();

    Analyser analyser;
    std::vector<float> db;
    const auto binHz = sampleRate / fftSize;

    // Bin range per row, worked out once.
    std::vector<std::pair<int, int>> rows ((size_t) height);

    for (int y = 0; y < height; ++y)
    {
        const auto hi = yToFrequency ((float) y, sampleRate, height);
        const auto lo = yToFrequency ((float) y + 1.0f, sampleRate, height);
        const auto b0 = juce::jlimit (0, fftSize / 2, (int) std::round (lo / binHz));
        const auto b1 = juce::jlimit (b0, fftSize / 2, (int) std::round (hi / binHz));
        rows[(size_t) y] = { b0, b1 };
    }

    juce::AudioBuffer<float> chunk (snapshot.getNumChannels(), fftSize);
    const auto total = snapshot.getNumFrames();

    for (int x = 0; x < width; ++x)
    {
        const auto centre = (juce::int64) (viewStart + (x + 0.5) * framesPerPixel);

        if (centre >= total)
        {
            for (int y = 0; y < height; ++y)
                pixels.setPixelColour (x, y, juce::Colour (colours[0]));

            continue;
        }

        snapshot.read (chunk, 0, centre - fftSize / 2, fftSize);   // past either end reads as silence
        analyser.analyse (chunk.getReadPointer (channel), fftSize, fftSize / 2, db);

        for (int y = 0; y < height; ++y)
        {
            auto level = floorDb;

            for (int b = rows[(size_t) y].first; b <= rows[(size_t) y].second; ++b)
                level = juce::jmax (level, db[(size_t) b]);

            const auto index = juce::jlimit (0, 255, (int) ((level - floorDb) / -floorDb * 255.0f));
            *reinterpret_cast<juce::PixelARGB*> (pixels.getPixelPointer (x, y)) = colours[(size_t) index];
        }
    }

    return image;
}

float measurePeakDb (const juce::AudioBuffer<float>& buffer, int channel, int centreFrame, double sampleRate, double frequency)
{
    Analyser analyser;
    std::vector<float> db;
    analyser.analyse (buffer.getReadPointer (channel), buffer.getNumSamples(), centreFrame, db);
    const auto bin = (int) std::round (frequency / (sampleRate / fftSize));
    return juce::jmax (db[(size_t) bin - 1], db[(size_t) bin], db[(size_t) bin + 1]);
}
}
