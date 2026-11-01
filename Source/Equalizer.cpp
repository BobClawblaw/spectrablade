#include "Equalizer.h"

#include <complex>

namespace Equalizer
{
namespace
{
    struct Coefficients
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    };

    Coefficients coefficients (const Band& band, double rate)
    {
        const auto w = juce::MathConstants<double>::twoPi * band.frequency / rate;
        const auto cosW = std::cos (w);
        const auto alpha = std::sin (w) / (2.0 * band.q);
        const auto a = std::pow (10.0, band.gainDb / 40.0);
        const auto shelf = 2.0 * std::sqrt (a) * alpha;
        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

        switch (band.type)
        {
            case BandType::peak:
                b0 = 1.0 + alpha * a;  b1 = -2.0 * cosW;  b2 = 1.0 - alpha * a;
                a0 = 1.0 + alpha / a;  a1 = -2.0 * cosW;  a2 = 1.0 - alpha / a;
                break;

            case BandType::lowShelf:
                b0 = a * ((a + 1.0) - (a - 1.0) * cosW + shelf);
                b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosW);
                b2 = a * ((a + 1.0) - (a - 1.0) * cosW - shelf);
                a0 = (a + 1.0) + (a - 1.0) * cosW + shelf;
                a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosW);
                a2 = (a + 1.0) + (a - 1.0) * cosW - shelf;
                break;

            case BandType::highShelf:
                b0 = a * ((a + 1.0) + (a - 1.0) * cosW + shelf);
                b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosW);
                b2 = a * ((a + 1.0) + (a - 1.0) * cosW - shelf);
                a0 = (a + 1.0) - (a - 1.0) * cosW + shelf;
                a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosW);
                a2 = (a + 1.0) - (a - 1.0) * cosW - shelf;
                break;

            case BandType::highPass:
                b0 = (1.0 + cosW) / 2.0;  b1 = -(1.0 + cosW);  b2 = b0;
                a0 = 1.0 + alpha;         a1 = -2.0 * cosW;    a2 = 1.0 - alpha;
                break;

            case BandType::lowPass:
                b0 = (1.0 - cosW) / 2.0;  b1 = 1.0 - cosW;     b2 = b0;
                a0 = 1.0 + alpha;         a1 = -2.0 * cosW;    a2 = 1.0 - alpha;
                break;
        }

        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }

    /** One channel through one band, transposed direct form II in double precision. */
    void filter (float* x, int n, const Coefficients& c)
    {
        double z1 = 0.0, z2 = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const auto in = (double) x[i];
            const auto y = c.b0 * in + z1;
            z1 = c.b1 * in - c.a1 * y + z2;
            z2 = c.b2 * in - c.a2 * y;
            x[i] = (float) y;
        }
    }
}

bool isValid (const Band& band, double sampleRate)
{
    return sampleRate > 0.0 && band.frequency > 0.0 && band.frequency < sampleRate / 2.0
        && band.q >= minQ && band.q <= maxQ && std::abs (band.gainDb) <= maxGainDb;
}

double responseDb (const Band& band, double sampleRate, double frequency)
{
    if (! isValid (band, sampleRate))
        return 0.0;

    const auto c = coefficients (band, sampleRate);
    const auto z = std::polar (1.0, -juce::MathConstants<double>::twoPi * frequency / sampleRate);  // z^-1
    const auto h = (c.b0 + c.b1 * z + c.b2 * z * z) / (1.0 + c.a1 * z + c.a2 * z * z);
    return 20.0 * std::log10 (std::abs (h));
}

AudioEdits::Snapshot apply (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const std::vector<Band>& bands)
{
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (bands.empty() || start == end || end - start > std::numeric_limits<int>::max() / 2)
        return nullptr;

    for (auto& band : bands)
        if (! isValid (band, sampleRate))
            return nullptr;

    const auto prime = (int) juce::jmin (start, (juce::int64) std::ceil (sampleRate));

    return AudioEdits::processRange (s, start - prime, end, [&bands, sampleRate, prime] (juce::AudioBuffer<float>& b)
    {
        juce::AudioBuffer<float> before (b.getNumChannels(), prime);

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            before.copyFrom (ch, 0, b, ch, 0, prime);

            for (auto& band : bands)
                filter (b.getWritePointer (ch), b.getNumSamples(), coefficients (band, sampleRate));

            b.copyFrom (ch, 0, before, ch, 0, prime);
        }
    });
}
}
