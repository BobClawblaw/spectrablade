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

//==============================================================================
namespace
{
    constexpr int gridOrder = 16, grid = 1 << gridOrder;   // the response sampled at 65,536 points round the circle
    constexpr int half = (LinearPhase::taps - 1) / 2;

    double magnitudeAt (const Coefficients& c, int bin)
    {
        const auto z = std::polar (1.0, -juce::MathConstants<double>::twoPi * bin / grid);
        return std::abs ((c.b0 + c.b1 * z + c.b2 * z * z) / (1.0 + c.a1 * z + c.a2 * z * z));
    }

    /** A zero-phase impulse (taps long, under a Hann window) from a real spectrum over the grid's
        bins 0 to grid / 2. */
    std::vector<float> impulseOf (const std::vector<double>& spectrum)
    {
        juce::dsp::FFT fft (gridOrder);
        std::vector<float> work ((size_t) (2 * grid), 0.0f);

        for (int k = 0; k <= grid / 2; ++k)
            work[(size_t) (2 * k)] = (float) spectrum[(size_t) k];

        fft.performRealOnlyInverseTransform (work.data());
        std::vector<float> h ((size_t) LinearPhase::taps);

        for (int n = -half; n <= half; ++n)
        {
            const auto window = 0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * n / (half + 1));
            h[(size_t) (n + half)] = (float) (window * work[(size_t) ((n + grid) % grid)]);
        }

        return h;
    }
}

void LinearPhase::prepare (const std::vector<Band>& bands, double sampleRate, int numChannels)
{
    channelsPrepared = juce::jmax (1, numChannels);
    const auto stereo = channelsPrepared >= 2;
    const auto bins = (size_t) (grid / 2 + 1);

    // Per bin, the matrix taking (left, right) to (left, right): ll, lr, rl, rr; and the product for
    // the channels past the first two (the bands on every channel). Composed band by band, in order.
    std::vector<double> ll (bins, 1.0), lr (bins, 0.0), rl (bins, 0.0), rr (bins, 1.0), others (bins, 1.0);
    auto leftTouched = false, rightTouched = false, crossed = false, othersTouched = false;

    for (const auto& band : bands)
    {
        const auto c = coefficients (band, sampleRate);
        const auto channel = band.channel;

        if (! stereo && (channel == Channel::right || channel == Channel::side))
            continue;

        if (channel == Channel::all)
            othersTouched = true;

        const auto onLeft = channel == Channel::all || channel == Channel::left || channel == Channel::mid || channel == Channel::side;
        const auto onRight = stereo && (channel == Channel::all || channel == Channel::right || channel == Channel::mid || channel == Channel::side);
        leftTouched = leftTouched || onLeft;
        rightTouched = rightTouched || onRight;
        crossed = crossed || (stereo && (channel == Channel::mid || channel == Channel::side));

        for (size_t k = 0; k < bins; ++k)
        {
            const auto h = magnitudeAt (c, (int) k);

            if (channel == Channel::all)
                others[k] *= h;

            if (! stereo || channel == Channel::all || channel == Channel::left || channel == Channel::right)
            {
                // A diagonal step: rows scaled.
                const auto sl = channel == Channel::right ? 1.0 : h, sr = channel == Channel::left ? 1.0 : h;
                ll[k] *= sl;  lr[k] *= sl;
                rl[k] *= sr;  rr[k] *= sr;
                continue;
            }

            // Mid: (h + 1) / 2 on the diagonal, (h - 1) / 2 across; side: the cross term's sign turned.
            const auto d = (h + 1.0) / 2.0, x = (channel == Channel::mid ? h - 1.0 : 1.0 - h) / 2.0;
            const auto nll = d * ll[k] + x * rl[k], nlr = d * lr[k] + x * rr[k];
            const auto nrl = x * ll[k] + d * rl[k], nrr = x * lr[k] + d * rr[k];
            ll[k] = nll;  lr[k] = nlr;  rl[k] = nrl;  rr[k] = nrr;
        }
    }

    impulses.assign (stereo ? 5 : 1, {});
    plain.assign ((size_t) channelsPrepared, true);

    if (leftTouched)
    {
        impulses[0] = impulseOf (ll);
        plain[0] = false;
    }

    if (stereo)
    {
        if (rightTouched)
        {
            impulses[1] = impulseOf (rr);
            plain[1] = false;
        }

        if (crossed)
        {
            impulses[2] = impulseOf (lr);
            impulses[3] = impulseOf (rl);
        }

        if (othersTouched)
        {
            impulses[4] = impulseOf (others);

            for (int ch = 2; ch < channelsPrepared; ++ch)
                plain[(size_t) ch] = false;
        }
    }

    // The convolvers: each channel from itself (a unit impulse at the centre where no band touches
    // it, so the convolver is still in step), and the two from each other.
    juce::AudioBuffer<float> self (channelsPrepared, taps);
    self.clear();

    for (int ch = 0; ch < channelsPrepared; ++ch)
    {
        const auto& h = ch == 0 ? impulses[0] : ch == 1 ? impulses[1] : impulses[4];

        if (h.empty())
            self.setSample (ch, half, 1.0f);
        else
            self.copyFrom (ch, 0, h.data(), taps);
    }

    straight = std::make_unique<Convolver::Core>();
    straight->prepare (self, channelsPrepared);
    straight->setMix (100.0);
    across.reset();

    if (crossed)
    {
        juce::AudioBuffer<float> cross (2, taps);
        cross.copyFrom (0, 0, impulses[2].data(), taps);   // left from right
        cross.copyFrom (1, 0, impulses[3].data(), taps);   // right from left
        across = std::make_unique<Convolver::Core>();
        across->prepare (cross, 2);
        across->setMix (100.0);
    }

    delays.assign ((size_t) channelsPrepared, std::vector<float> ((size_t) latency, 0.0f));
    pointers.assign ((size_t) channelsPrepared, nullptr);
    delayPos = 0;
}

void LinearPhase::reset()
{
    if (straight != nullptr)
        straight->reset();

    if (across != nullptr)
        across->reset();

    for (auto& d : delays)
        std::fill (d.begin(), d.end(), 0.0f);

    delayPos = 0;
}

void LinearPhase::process (float* const* channels, int numChannels, int numFrames) noexcept
{
    const auto n = juce::jmin (numChannels, channelsPrepared);

    if (n <= 0 || straight == nullptr)
        return;

    // The cross terms take both channels as they came in.
    const auto crossed = across != nullptr && n >= 2;

    if (crossed)
    {
        swapped.setSize (2, numFrames, false, false, true);
        swapped.copyFrom (0, 0, channels[1], numFrames);
        swapped.copyFrom (1, 0, channels[0], numFrames);
    }

    // A channel no band touches is only delayed, its samples untouched by the convolver's rounding;
    // the convolver gets scratch space for it instead, so it stays in step on the other channels.
    junk.setSize (n, numFrames, false, false, true);

    for (int ch = 0; ch < n; ++ch)
    {
        if (! plain[(size_t) ch])
        {
            pointers[(size_t) ch] = channels[ch];
            continue;
        }

        pointers[(size_t) ch] = junk.getWritePointer (ch);
        auto& d = delays[(size_t) ch];
        auto pos = delayPos;

        for (int i = 0; i < numFrames; ++i)
        {
            std::swap (d[(size_t) pos], channels[ch][i]);

            if (++pos == latency)
                pos = 0;
        }
    }

    delayPos = (delayPos + numFrames) % latency;
    straight->process (pointers.data(), n, numFrames);

    if (crossed)
    {
        across->process (swapped.getArrayOfWritePointers(), 2, numFrames);

        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::add (channels[ch], swapped.getReadPointer (ch), numFrames);
    }
}

AudioEdits::Snapshot applyLinearPhase (const AudioSnapshot& s, juce::int64 start, juce::int64 end, double sampleRate, const std::vector<Band>& bands)
{
    start = juce::jlimit ((juce::int64) 0, s.getNumFrames(), start);
    end = juce::jlimit (start, s.getNumFrames(), end);

    if (bands.empty() || start == end || end - start > std::numeric_limits<int>::max() / 4)
        return nullptr;

    for (auto& band : bands)
        if (! isValid (band, sampleRate))
            return nullptr;

    const auto prime = (int) juce::jmin (start, (juce::int64) LinearPhase::taps);

    return AudioEdits::processRange (s, start - prime, end, [&bands, sampleRate, prime] (juce::AudioBuffer<float>& b)
    {
        const auto channels = b.getNumChannels(), frames = b.getNumSamples();
        LinearPhase engine;
        engine.prepare (bands, sampleRate, channels);

        // The audio plus the engine's delay of silence after it, through the engine; what comes out
        // from the delay on is the range, in place.
        juce::AudioBuffer<float> work (channels, frames + LinearPhase::latency);
        work.clear();

        for (int ch = 0; ch < channels; ++ch)
            work.copyFrom (ch, 0, b, ch, 0, frames);

        engine.process (work.getArrayOfWritePointers(), channels, work.getNumSamples());

        for (int ch = 0; ch < channels; ++ch)
            b.copyFrom (ch, prime, work, ch, LinearPhase::latency + prime, frames - prime);
    });
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
