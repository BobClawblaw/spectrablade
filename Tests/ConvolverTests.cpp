#include <JuceHeader.h>
#include "../Source/Convolver.h"
#include "TestUtils.h"
#include "ConvolverTests.h"

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

    juce::AudioBuffer<float> noise (int channels, int frames, int seed)
    {
        juce::Random r (seed);
        juce::AudioBuffer<float> b (channels, frames);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, r.nextFloat() - 0.5f);

        return b;
    }

    Convolver::Impulse impulseOf (juce::AudioBuffer<float> samples, double impulseRate = rate)
    {
        return { std::move (samples), impulseRate, "test" };
    }
}

void runConvolverTests()
{
    // Against direct convolution in double precision: a 3,000-frame stereo impulse on 20,000 frames
    // of noise, all wet, the whole file (so the tail is cut at its end).
    {
        const auto in = noise (2, 20000, 31);
        const auto impulse = impulseOf (noise (2, 3000, 32));
        const auto shaped = Convolver::atRate (impulse, rate);
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        const auto out = readAll (*Convolver::apply (*snap, 0, 20000, rate, impulse, 100.0));
        double worst = 0.0, peak = 0.0;

        for (int ch = 0; ch < 2; ++ch)
            for (int n = 0; n < 20000; ++n)
            {
                double y = 0.0;

                for (int k = 0; k < shaped.getNumSamples() && k <= n; ++k)
                    y += (double) shaped.getSample (ch, k) * in.getSample (ch, n - k);

                worst = juce::jmax (worst, std::abs (y - out.getSample (ch, n)));
                peak = juce::jmax (peak, std::abs (y));
            }

        double e0 = 0.0, e1 = 0.0;

        for (int k = 0; k < shaped.getNumSamples(); ++k)
        {
            e0 += (double) shaped.getSample (0, k) * shaped.getSample (0, k);
            e1 += (double) shaped.getSample (1, k) * shaped.getSample (1, k);
        }

        std::cout << "      convolution: largest difference from direct " << worst << " (peak " << peak << "); impulse energy " << e0 << ", " << e1 << std::endl;
        check (worst < 1.0e-5 * peak && std::abs (juce::jmax (e0, e1) - 1.0) < 1.0e-6,
               "convolution reverb: all wet, each channel is its impulse channel convolved with it, to within 1e-5 of the peak of direct convolution in "
               "double precision; the impulse is scaled to an energy of 1 in its loudest channel");
    }

    // An impulse of a single click 100 frames in: all wet, the input 100 frames late; no mix, the input bit for bit.
    {
        juce::AudioBuffer<float> click (1, 101);
        click.clear();
        click.setSample (0, 100, 0.25f);
        const auto in = noise (2, 5000, 33);
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        const auto late = readAll (*Convolver::apply (*snap, 0, 5000, rate, impulseOf (click), 100.0));
        const auto same = readAll (*Convolver::apply (*snap, 0, 5000, rate, impulseOf (click), 0.0));
        double worst = 0.0;
        auto unchanged = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int n = 0; n < 5000; ++n)
            {
                worst = juce::jmax (worst, std::abs ((double) late.getSample (ch, n) - (n < 100 ? 0.0 : in.getSample (ch, n - 100))));
                unchanged = unchanged && juce::exactlyEqual (same.getSample (ch, n), in.getSample (ch, n));
            }

        std::cout << "      convolution with a click: largest difference from the input 100 frames late " << worst << std::endl;
        check (worst < 1.0e-6 && unchanged,
               "convolution reverb: a mono click impulse 100 frames in (scaled to 1) gives every channel 100 frames late, to 1e-6; no mix gives the input bit for bit");
    }

    // The tail: a range ending mid-file rings on for the impulse's length, added on top, and no further.
    {
        const auto in = noise (2, 20000, 34);
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        const auto out = readAll (*Convolver::apply (*snap, 0, 5000, rate, impulseOf (noise (1, 2000, 35)), 50.0));
        auto inTail = false, after = true;

        for (int ch = 0; ch < 2; ++ch)
            for (int n = 5000; n < 20000; ++n)
            {
                if (n < 7000)
                    inTail = inTail || ! juce::exactlyEqual (out.getSample (ch, n), in.getSample (ch, n));
                else
                    after = after && juce::exactlyEqual (out.getSample (ch, n), in.getSample (ch, n));
            }

        check (inTail && after, "convolution reverb: the tail is added for the impulse's length after the range, and the audio after it is kept bit for bit");
    }

    // The impulse's rate: one at 24 kHz is resampled to twice as many frames at 48 kHz.
    {
        const auto shaped = Convolver::atRate (impulseOf (noise (1, 1200, 36), 24000.0), rate);
        check (shaped.getNumSamples() == 2400 && shaped.getNumChannels() == 1,
               "convolution reverb: an impulse at 24 kHz is resampled to 48 kHz (1,200 frames become 2,400)");
    }

    // Impulses that can't be used.
    {
        juce::AudioBuffer<float> silent (1, 100);
        silent.clear();
        const auto in = noise (2, 1000, 37);
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        check (Convolver::problemWith (impulseOf (silent)).isNotEmpty() && Convolver::problemWith (impulseOf (noise (3, 100, 38))).isNotEmpty()
                   && Convolver::problemWith (impulseOf (noise (1, 100, 39), 0.0)).isNotEmpty()
                   && Convolver::problemWith (impulseOf (noise (1, (int) (10.5 * rate), 40))).isNotEmpty()
                   && Convolver::problemWith (impulseOf (noise (2, 100, 41))).isEmpty()
                   && Convolver::apply (*snap, 0, 1000, rate, impulseOf (silent), 50.0) == nullptr
                   && Convolver::apply (*snap, 0, 1000, rate, impulseOf (noise (1, 100, 42)), 101.0) == nullptr,
               "convolution reverb: a silent impulse, three channels, no rate or over 10 s refused, as is a mix over 100 %");
    }
}
