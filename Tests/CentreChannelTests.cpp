// Samples outside an edit are compared bit for bit on purpose.
#pragma clang diagnostic ignored "-Wfloat-equal"

#include <JuceHeader.h>
#include "../Source/CentreChannel.h"
#include "TestUtils.h"
#include "CentreChannelTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;
    constexpr int second = (int) rate;

    AudioDocument::Snapshot snapshotOf (const std::vector<float>& left, const std::vector<float>& right)
    {
        juce::AudioBuffer<float> b (2, (int) left.size());

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            b.setSample (0, i, left[(size_t) i]);
            b.setSample (1, i, right[(size_t) i]);
        }

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, (juce::int64) left.size() } };
        return std::make_shared<const AudioSnapshot> (2, std::move (segments));
    }

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    double db (double x) { return 20.0 * std::log10 (juce::jmax (x, 1.0e-15)); }

    /** The amplitude of a tone of hz in one channel over [from, from + one second). */
    double amplitudeAt (const juce::AudioBuffer<float>& b, int ch, double hz, int from)
    {
        double re = 0.0, im = 0.0;

        for (int i = from; i < from + second; ++i)
        {
            const auto w = juce::MathConstants<double>::twoPi * hz * i / rate;
            re += b.getSample (ch, i) * std::cos (w);
            im += b.getSample (ch, i) * std::sin (w);
        }

        return 2.0 * std::sqrt (re * re + im * im) / second;
    }

    float sine (double hz, int i) { return (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / rate); }

    /** Tones apart: 440 Hz in the middle (both channels), 1 kHz on the left only, 1.5 kHz on the
        right only, and 50 Hz in the middle, below the band. */
    AudioDocument::Snapshot tones()
    {
        std::vector<float> l ((size_t) (4 * second)), r (l.size());

        for (int i = 0; i < (int) l.size(); ++i)
        {
            const auto middle = 0.3f * sine (440.0, i) + 0.2f * sine (50.0, i);
            l[(size_t) i] = middle + 0.2f * sine (1000.0, i);
            r[(size_t) i] = middle + 0.2f * sine (1500.0, i);
        }

        return snapshotOf (l, r);
    }

    void testTones()
    {
        const auto in = tones();
        const auto before = readAll (*in);
        const auto removed = CentreChannel::process (*in, 0, in->getNumFrames(), rate, { false, 100.0f, 8000.0f, 20.0f });
        const auto kept = CentreChannel::process (*in, 0, in->getNumFrames(), rate, { true, 100.0f, 8000.0f, 20.0f });

        if (removed == nullptr || kept == nullptr)
        {
            check (false, "centre channel: a stereo file is processed");
            return;
        }

        const auto r = readAll (*removed), k = readAll (*kept);
        const auto change = [&] (const juce::AudioBuffer<float>& after, int ch, double hz)
        {
            return db (amplitudeAt (after, ch, hz, 2 * second)) - db (amplitudeAt (before, ch, hz, 2 * second));
        };

        // Removed: the middle tone down by the reduction in both channels, the sides kept.
        const auto middleDown = juce::jmax (change (r, 0, 440.0), change (r, 1, 440.0));
        const auto sidesKept = juce::jmax (std::abs (change (r, 0, 1000.0)), std::abs (change (r, 1, 1500.0)));
        const auto lowKept = juce::jmax (std::abs (change (r, 0, 50.0)), std::abs (change (r, 1, 50.0)));
        check (middleDown < -19.5 && middleDown > -20.5 && sidesKept < 0.1 && lowKept < 0.1,
               "centre channel: removed by 20 dB, a tone in the middle goes down " + juce::String (-middleDown, 2) + " dB in both channels, "
               "tones on one side only keep their level (within " + juce::String (sidesKept, 3) + " dB), and one in the middle below the "
               "band is left (within " + juce::String (lowKept, 3) + " dB)");

        // Kept alone: the sides down by the reduction, the middle kept.
        const auto sidesDown = juce::jmax (change (k, 0, 1000.0), change (k, 1, 1500.0));
        const auto middleKept = juce::jmax (std::abs (change (k, 0, 440.0)), std::abs (change (k, 1, 440.0)));
        const auto lowLeft = juce::jmax (std::abs (change (k, 0, 50.0)), std::abs (change (k, 1, 50.0)));
        check (sidesDown < -19.5 && sidesDown > -20.5 && middleKept < 0.1 && lowLeft < 0.1,
               "centre channel: extracted by 20 dB, tones on one side go down " + juce::String (-sidesDown, 2) + " dB, the one in the middle "
               "keeps its level (within " + juce::String (middleKept, 3) + " dB), and below the band the mix is left (within "
                   + juce::String (lowLeft, 3) + " dB)");
    }

    void testMix()
    {
        // A voice-like sound in the middle (a 180 Hz buzz with vibrato, in syllables) over white
        // noise on each side, as loud as each other and unrelated, in the voice's bands too.
        const auto length = 6 * second;
        std::vector<float> voice ((size_t) length), left ((size_t) length), right ((size_t) length);
        juce::Random random (5);
        double phase = 0.0;

        for (int i = 0; i < length; ++i)
        {
            const auto t = i / rate;
            phase += juce::MathConstants<double>::twoPi * 180.0 * (1.0 + 0.03 * std::sin (juce::MathConstants<double>::twoPi * 5.0 * t)) / rate;
            double buzz = 0.0;

            for (int h = 1; h <= 24; ++h)
                buzz += std::sin (h * phase) / h;

            const auto syllable = std::sin (juce::MathConstants<double>::twoPi * 3.0 * t) > 0.0 ? 1.0 : 0.0;
            voice[(size_t) i] = (float) (0.2 * buzz * syllable);
            left[(size_t) i] = 0.1f * (2.0f * random.nextFloat() - 1.0f);
            right[(size_t) i] = 0.1f * (2.0f * random.nextFloat() - 1.0f);
        }

        std::vector<float> l ((size_t) length), r ((size_t) length);

        for (size_t i = 0; i < (size_t) length; ++i)
        {
            l[i] = voice[i] + left[i];
            r[i] = voice[i] + right[i];
        }

        const auto in = snapshotOf (l, r);
        const auto removed = CentreChannel::process (*in, 0, length, rate, { false, 20.0f, 20000.0f, 40.0f });
        const auto kept = CentreChannel::process (*in, 0, length, rate, { true, 20.0f, 20000.0f, 40.0f });

        if (removed == nullptr || kept == nullptr)
        {
            check (false, "centre channel: a mix is processed");
            return;
        }

        // How far each comes from what it should be (the sides alone; the voice alone), against
        // the power of what was to go, over the middle 4 s.
        const auto missBy = [&] (const juce::AudioBuffer<float>& out, const std::vector<float>& wantL, const std::vector<float>& wantR,
                                 const std::vector<float>& goneL, const std::vector<float>& goneR)
        {
            double miss = 0.0, gone = 0.0;

            for (int i = second; i < 5 * second; ++i)
            {
                const auto el = out.getSample (0, i) - wantL[(size_t) i], er = out.getSample (1, i) - wantR[(size_t) i];
                miss += (double) el * el + (double) er * er;
                gone += (double) goneL[(size_t) i] * goneL[(size_t) i] + (double) goneR[(size_t) i] * goneR[(size_t) i];
            }

            return 10.0 * std::log10 (miss / gone);
        };

        const auto voiceLeft = missBy (readAll (*removed), left, right, voice, voice);
        const auto sidesLeft = missBy (readAll (*kept), voice, voice, left, right);
        const auto power = [] (const std::vector<float>& x)
        {
            double sum = 0.0;

            for (int i = second; i < 5 * second; ++i)
                sum += (double) x[(size_t) i] * x[(size_t) i];

            return sum;
        };
        const auto voiceOverNoise = 10.0 * std::log10 (power (voice) / power (left));

        check (voiceLeft < -15.0 && sidesLeft < -8.0,
               "centre channel: a voice in the middle (" + juce::String (voiceOverNoise, 1) + " dB over the noise in each channel) with "
               "unrelated noise on each side: removed, it is the noise alone but for an error " + juce::String (-voiceLeft, 1)
                   + " dB below the voice; extracted, the voice alone but for an error " + juce::String (-sidesLeft, 1) + " dB below the noise");
    }

    void testRangeAndRefusals()
    {
        const auto in = tones();
        const auto frame = CentreChannel::frameSize (rate);
        const juce::int64 start = second, end = 3 * second;
        const auto r = CentreChannel::process (*in, start, end, rate, {});
        auto outsideSame = r != nullptr, insideChanged = false;

        if (r != nullptr)
        {
            const auto a = readAll (*in), b = readAll (*r);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < a.getNumSamples(); ++i)
                {
                    if (i < start - frame || i >= end + frame)
                        outsideSame = outsideSame && a.getSample (ch, i) == b.getSample (ch, i);
                    else if (i >= start && i < end)
                        insideChanged = insideChanged || std::abs (a.getSample (ch, i) - b.getSample (ch, i)) > 0.01f;
                }
        }

        // No reduction: the mix comes back as it was, to float rounding.
        const auto none = CentreChannel::process (*in, 0, in->getNumFrames(), rate, { false, 100.0f, 8000.0f, 0.0f });
        auto largest = none != nullptr ? 0.0f : 1.0f;

        if (none != nullptr)
        {
            const auto a = readAll (*in), b = readAll (*none);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    largest = juce::jmax (largest, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
        }

        check (outsideSame && insideChanged && largest < 1.0e-5f,
               "centre channel: a range changes the audio in it and leaves the audio more than a frame outside it bit for bit; at no "
               "reduction the mix comes back as it was (within " + juce::String (largest, 8) + ")");

        // Mono, an empty range, settings out of range, and stopping: nothing.
        juce::AudioBuffer<float> one (1, second);
        one.clear();
        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (one)), 0, (juce::int64) second } };
        const auto mono = std::make_shared<const AudioSnapshot> (1, std::move (segments));
        int calls = 0;
        const auto stopped = CentreChannel::process (*in, 0, in->getNumFrames(), rate, {}, [&] (float) { return ++calls < 2; });

        check (CentreChannel::process (*mono, 0, second, rate, {}) == nullptr && CentreChannel::process (*in, 10, 10, rate, {}) == nullptr
                   && stopped == nullptr && calls == 2
                   && ! CentreChannel::isValid ({ false, 19.0f, 8000.0f, 20.0f }) && ! CentreChannel::isValid ({ false, 100.0f, 20001.0f, 20.0f })
                   && ! CentreChannel::isValid ({ false, 5000.0f, 4000.0f, 20.0f }) && ! CentreChannel::isValid ({ false, 100.0f, 8000.0f, 41.0f })
                   && ! CentreChannel::isValid ({ false, 100.0f, 8000.0f, -1.0f }) && ! CentreChannel::isValid ({ true, std::nanf (""), 8000.0f, 20.0f })
                   && CentreChannel::isValid ({ true, 20.0f, 20000.0f, 40.0f }) && CentreChannel::isValid ({ false, 20.0f, 21.0f, 0.0f }),
               "centre channel: a mono file, an empty range and settings out of range are refused, and it can be stopped");
    }
}

void runCentreChannelTests()
{
    testTones();
    testMix();
    testRangeAndRefusals();
}
