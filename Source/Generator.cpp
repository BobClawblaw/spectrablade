#include "Generator.h"

namespace Generator
{
    namespace
    {
        /** The polyBLEP correction for a jump at phase 0 of a wave whose phase advances dt a sample. */
        double polyBlep (double t, double dt)
        {
            if (t < dt)
            {
                t /= dt;
                return t + t - t * t - 1.0;
            }

            if (t > 1.0 - dt)
            {
                t = (t - 1.0) / dt;
                return t * t + t + t + 1.0;
            }

            return 0.0;
        }

        void fade (juce::AudioBuffer<float>& b, double fadeMs, double sampleRate)
        {
            const auto n = juce::jmin (b.getNumSamples() / 2, (int) std::round (fadeMs * 0.001 * sampleRate));

            if (n > 0)
            {
                b.applyGainRamp (0, n, 0.0f, 1.0f);
                b.applyGainRamp (b.getNumSamples() - n, n, 1.0f, 0.0f);
            }
        }
    }

    bool isValid (const Tone& t, double sampleRate)
    {
        const auto nyquist = 0.5 * sampleRate;
        return sampleRate > 0.0 && t.frequency >= 1.0 && t.frequency < nyquist && t.endFrequency >= 1.0 && t.endFrequency < nyquist
               && t.levelDb >= -96.0 && t.levelDb <= 0.0 && t.fadeMs >= 0.0 && t.fadeMs <= 1000.0;
    }

    double maxLevelDb (Colour c)
    {
        return c == Colour::white ? -6.0 : -20.0;
    }

    bool isValid (const Noise& n)
    {
        return n.levelDb >= -96.0 && n.levelDb <= maxLevelDb (n.colour) && n.fadeMs >= 0.0 && n.fadeMs <= 1000.0;
    }

    juce::AudioBuffer<float> tone (const Tone& t, int numChannels, int frames, double sampleRate)
    {
        juce::AudioBuffer<float> b (juce::jmax (1, numChannels), juce::jmax (0, frames));
        b.clear();

        if (frames <= 0 || ! isValid (t, sampleRate))
            return b;

        const auto gain = juce::Decibels::decibelsToGain (t.levelDb);
        const auto octaves = std::log2 (t.endFrequency / t.frequency);
        auto* x = b.getWritePointer (0);
        double phase = 0.0, triangle = -1.0;   // the triangle rises from -1 to 1 while the square is high

        for (int i = 0; i < frames; ++i)
        {
            const auto f = t.frequency * std::exp2 (octaves * i / juce::jmax (1, frames - 1));
            const auto dt = f / sampleRate;
            double v = 0.0;

            switch (t.wave)
            {
                case Wave::sine:     v = std::sin (juce::MathConstants<double>::twoPi * phase); break;
                case Wave::sawtooth: v = 2.0 * phase - 1.0 - polyBlep (phase, dt); break;
                case Wave::square:
                case Wave::triangle:
                {
                    v = (phase < 0.5 ? 1.0 : -1.0) + polyBlep (phase, dt) - polyBlep (std::fmod (phase + 0.5, 1.0), dt);

                    if (t.wave == Wave::triangle)
                    {
                        // The smoothed square, integrated into a triangle.
                        triangle += 4.0 * dt * v;
                        v = triangle;
                    }

                    break;
                }
            }

            x[i] = (float) (gain * v);
            phase += dt;
            phase -= std::floor (phase);
        }

        fade (b, t.fadeMs, sampleRate);

        for (int ch = 1; ch < b.getNumChannels(); ++ch)
            b.copyFrom (ch, 0, b, 0, 0, frames);

        return b;
    }

    juce::AudioBuffer<float> noise (const Noise& n, int numChannels, int frames, double sampleRate)
    {
        juce::AudioBuffer<float> b (juce::jmax (1, numChannels), juce::jmax (0, frames));
        b.clear();

        if (frames <= 0 || ! isValid (n))
            return b;

        juce::Random random (n.seed);
        const auto target = juce::Decibels::decibelsToGain (n.levelDb);

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            auto* x = b.getWritePointer (ch);
            double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, brown = 0;

            for (int i = 0; i < frames; ++i)
            {
                const auto w = 2.0 * random.nextDouble() - 1.0;

                switch (n.colour)
                {
                    case Colour::white: x[i] = (float) w; break;
                    case Colour::pink:
                    {
                        // Paul Kellet's filter: within 0.05 dB of -3 dB an octave above 9 Hz at 44.1 kHz.
                        b0 = 0.99886 * b0 + w * 0.0555179;
                        b1 = 0.99332 * b1 + w * 0.0750759;
                        b2 = 0.96900 * b2 + w * 0.1538520;
                        b3 = 0.86650 * b3 + w * 0.3104856;
                        b4 = 0.55000 * b4 + w * 0.5329522;
                        b5 = -0.7616 * b5 - w * 0.0168980;
                        x[i] = (float) (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362);
                        b6 = w * 0.115926;
                        break;
                    }
                    case Colour::brown:
                        // Integrated white noise, let leak slowly back to zero so it can't wander off.
                        brown = 0.998 * brown + 0.05 * w;
                        x[i] = (float) brown;
                        break;
                }
            }

            const auto rms = b.getRMSLevel (ch, 0, frames);

            if (rms > 0.0f)
                b.applyGain (ch, 0, frames, (float) target / rms);
        }

        fade (b, n.fadeMs, sampleRate);
        return b;
    }

    AudioEdits::Snapshot insert (const AudioSnapshot& s, juce::int64 at, const juce::AudioBuffer<float>& audio)
    {
        const auto frames = audio.getNumSamples();

        if (frames <= 0 || audio.getNumChannels() != s.getNumChannels())
            return nullptr;

        at = juce::jlimit ((juce::int64) 0, s.getNumFrames(), at);
        auto widened = AudioEdits::insertSilence (s, at, frames);

        if (widened == nullptr)
            return nullptr;

        return AudioEdits::processRange (*widened, at, at + frames, [&audio, frames] (juce::AudioBuffer<float>& b)
        {
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                b.copyFrom (ch, 0, audio, ch, 0, frames);
        });
    }
}
