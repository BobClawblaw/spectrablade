#include <JuceHeader.h>
#include "../Source/Equalizer.h"
#include "TestUtils.h"
#include "EqTests.h"

namespace
{
    using TestUtils::check;
    using Equalizer::Band;
    using Equalizer::BandType;

    constexpr double rate = 48000.0;

    juce::AudioBuffer<float> sine (double hz, int frames, int channels = 1, float amplitude = 0.25f)
    {
        juce::AudioBuffer<float> b (channels, frames);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / rate + ch));

        return b;
    }

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    double rmsDb (const juce::AudioBuffer<float>& b, int ch, int from, int to)
    {
        double sum = 0.0;

        for (int i = from; i < to; ++i)
            sum += (double) b.getSample (ch, i) * b.getSample (ch, i);

        return 10.0 * std::log10 (sum / (to - from));
    }

    /** The level change the bands make to a 0.25 sine at hz over its second half, in dB. */
    double measuredDb (const std::vector<Band>& bands, double hz)
    {
        const auto in = sine (hz, 96000);
        const auto out = readAll (*Equalizer::apply (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (in)), 0, 96000, rate, bands));
        return rmsDb (out, 0, 48000, 96000) - rmsDb (in, 0, 48000, 96000);
    }
}

void runEqTests()
{
    const Band bell { BandType::peak, 1000.0, 6.0, 1.0 };
    const Band lowShelf { BandType::lowShelf, 200.0, 6.0, 0.7071067811865476 };
    const Band highShelf { BandType::highShelf, 5000.0, -9.0, 0.7071067811865476 };
    const Band highPass { BandType::highPass, 1000.0, 0.0, 0.7071067811865476 };
    const Band lowPass { BandType::lowPass, 1000.0, 0.0, 0.7071067811865476 };
    const auto near = [] (double a, double b, double tolerance) { return std::abs (a - b) <= tolerance; };
    const auto r = [] (const Band& band, double hz) { return Equalizer::responseDb (band, rate, hz); };

    check (near (r (bell, 1000.0), 6.0, 1.0e-9) && std::abs (r (bell, 20.0)) < 0.01 && std::abs (r (bell, 20000.0)) < 0.1
               && near (r (lowShelf, 200.0), 3.0, 1.0e-9) && near (r (lowShelf, 20.0), 6.0, 0.05) && std::abs (r (lowShelf, 10000.0)) < 0.01
               && near (r (highShelf, 5000.0), -4.5, 1.0e-9) && near (r (highShelf, 23000.0), -9.0, 0.05) && std::abs (r (highShelf, 100.0)) < 0.01
               && near (r (highPass, 1000.0), -3.0103, 1.0e-4) && near (r (highPass, 500.0), 10.0 * std::log10 (1.0 / 17.0), 0.03) && std::abs (r (highPass, 10000.0)) < 0.01
               && near (r (lowPass, 1000.0), -3.0103, 1.0e-4) && std::abs (r (lowPass, 50.0)) < 0.01,
           "eq responses: a bell's gain at its frequency, half a shelf's gain at its frequency, -3 dB at a pass's corner and within 0.03 dB of the analogue -12.30 an octave past it, flat far away");

    check (near (measuredDb ({ bell }, 1000.0), 6.0, 0.01) && near (measuredDb ({ bell }, 3000.0), r (bell, 3000.0), 0.01)
               && near (measuredDb ({ highPass }, 100.0), r (highPass, 100.0), 0.05) && near (measuredDb ({ lowShelf }, 50.0), r (lowShelf, 50.0), 0.01)
               && near (measuredDb ({ bell, highPass }, 700.0), r (bell, 700.0) + r (highPass, 700.0), 0.01),
           "eq on sines: each sine's level changes by the band's response at its frequency, and two bands by the sum");

    // Frames outside the range are kept bit for bit; the range starts as it would in a longer pass.
    const auto tone = sine (440.0, 144000, 2);
    const auto toneSnap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (tone));
    const auto part = readAll (*Equalizer::apply (*toneSnap, 72000, 96000, rate, { bell, highShelf }));
    const auto whole = readAll (*Equalizer::apply (*toneSnap, 0, 144000, rate, { bell, highShelf }));
    auto outsideKept = true, startsLikeWhole = true;
    float worstStart = 0.0f;

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 144000; ++i)
        {
            if (i < 72000 || i >= 96000)
                outsideKept = outsideKept && juce::exactlyEqual (part.getSample (ch, i), tone.getSample (ch, i));
            else
                worstStart = juce::jmax (worstStart, std::abs (part.getSample (ch, i) - whole.getSample (ch, i)));
        }

    startsLikeWhole = worstStart < 1.0e-6f;
    std::cout << "      eq: worst difference from the whole-file pass inside the range " << worstStart << std::endl;
    check (outsideKept && startsLikeWhole, "eq on a range: every frame outside it bit for bit, the range within 1e-6 of a pass over the whole file");

    const auto flat = readAll (*Equalizer::apply (*toneSnap, 0, 144000, rate, { Band { BandType::peak, 1000.0, 0.0, 1.0 } }));
    float worstFlat = 0.0f;

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 144000; ++i)
            worstFlat = juce::jmax (worstFlat, std::abs (flat.getSample (ch, i) - tone.getSample (ch, i)));

    const auto refused = [&toneSnap] (const Band& band) { return Equalizer::apply (*toneSnap, 0, 1000, rate, { band }) == nullptr; };
    check (worstFlat < 1.0e-6f && refused ({ BandType::peak, 0.0, 6.0, 1.0 }) && refused ({ BandType::peak, 24000.0, 6.0, 1.0 })
               && refused ({ BandType::peak, 1000.0, 6.0, 0.0 }) && refused ({ BandType::peak, 1000.0, 60.0, 1.0 })
               && Equalizer::apply (*toneSnap, 0, 1000, rate, {}) == nullptr && Equalizer::apply (*toneSnap, 500, 500, rate, { bell }) == nullptr,
           "eq: a 0 dB bell gives the audio back within 1e-6; no frequency, one at Nyquist, no q, 60 dB, no bands or an empty range refused");
}
