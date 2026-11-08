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

}

const juce::StringArray& channelNames()
{
    static const juce::StringArray names { "Stereo", "Mid", "Side", "Left", "Right" };
    return names;
}

bool operator== (const Band& a, const Band& b)
{
    return a.type == b.type && juce::exactlyEqual (a.frequency, b.frequency) && juce::exactlyEqual (a.gainDb, b.gainDb)
        && juce::exactlyEqual (a.q, b.q) && a.channel == b.channel;
}

bool isFlat (const Band& band)
{
    const auto isPass = band.type == BandType::highPass || band.type == BandType::lowPass;
    return ! isPass && juce::exactlyEqual (band.gainDb, 0.0);
}

void Filter::setBand (const Band& band, double sampleRate)
{
    jassert (isValid (band, sampleRate));
    const auto c = coefficients (band, sampleRate);
    b0 = c.b0;  b1 = c.b1;  b2 = c.b2;  a1 = c.a1;  a2 = c.a2;
}

// Transposed direct form II in double precision.
void Filter::process (float* x, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const auto in = (double) x[i];
        const auto y = b0 * in + z1;
        z1 = b1 * in - a1 * y + z2;
        z2 = b2 * in - a2 * y;
        x[i] = (float) y;
    }
}

double Filter::tick (double in) noexcept
{
    const auto y = b0 * in + z1;
    z1 = b1 * in - a1 * y + z2;
    z2 = b2 * in - a2 * y;
    return y;
}

void Stage::set (const Band& b, double sampleRate, int numChannels)
{
    if (b.channel != band.channel || numChannels != channelsSet)
    {
        filters.assign ((size_t) juce::jmax (1, numChannels), {});
        channelsSet = numChannels;
    }

    band = b;

    for (auto& f : filters)
        f.setBand (band, sampleRate);
}

void Stage::reset() noexcept
{
    for (auto& f : filters)
        f.reset();
}

void Stage::process (float* const* channels, int numChannels, int numFrames) noexcept
{
    const auto n = juce::jmin (numChannels, (int) filters.size());

    if (n <= 0)
        return;

    switch (band.channel)
    {
        case Channel::all:
            for (int ch = 0; ch < n; ++ch)
                filters[(size_t) ch].process (channels[ch], numFrames);
            break;

        case Channel::left:
            filters[0].process (channels[0], numFrames);
            break;

        case Channel::right:
            if (n > 1)
                filters[1].process (channels[1], numFrames);
            break;

        case Channel::mid:
        case Channel::side:
            if (n < 2)
            {
                if (band.channel == Channel::mid)   // a mono sound is all mid
                    filters[0].process (channels[0], numFrames);

                break;
            }

            for (int i = 0; i < numFrames; ++i)
            {
                const double l = channels[0][i], r = channels[1][i];
                auto mid = (l + r) / 2.0, side = (l - r) / 2.0;
                (band.channel == Channel::mid ? mid : side) = filters[0].tick (band.channel == Channel::mid ? mid : side);
                channels[0][i] = (float) (mid + side);
                channels[1][i] = (float) (mid - side);
            }
            break;
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
            before.copyFrom (ch, 0, b, ch, 0, prime);

        for (auto& band : bands)
        {
            Stage stage;
            stage.set (band, sampleRate, b.getNumChannels());
            stage.process (b.getArrayOfWritePointers(), b.getNumChannels(), b.getNumSamples());
        }

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            b.copyFrom (ch, 0, before, ch, 0, prime);
    });
}
}
