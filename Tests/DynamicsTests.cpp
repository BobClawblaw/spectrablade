#include <JuceHeader.h>
#include "../Source/Dynamics.h"
#include "../Source/Loudness.h"
#include "TestUtils.h"
#include "DynamicsTests.h"

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

    bool near (double a, double b, double tolerance)    { return std::abs (a - b) <= tolerance; }
    double db (double gain)                              { return 20.0 * std::log10 (gain); }
}

void runDynamicsTests()
{
    // The static curve: 4:1 over -20 dB with a 10 dB knee.
    const Dynamics::Compressor c { -20.0, 4.0, 10.0, 10.0, 100.0, 0.0 };
    const auto g = [&c] (double level) { return Dynamics::staticGainDb (c, level); };
    auto hard = c;
    hard.kneeDb = 0.0;
    check (juce::exactlyEqual (g (-40.0), 0.0) && juce::exactlyEqual (g (-25.0), 0.0) && near (g (-20.0), -0.75 * 10.0 / 8.0, 1.0e-12)
               && near (g (-15.0), -0.75 * 5.0, 1.0e-12) && near (g (0.0), -15.0, 1.0e-12)
               && near (Dynamics::staticGainDb (hard, -10.0), -7.5, 1.0e-12) && juce::exactlyEqual (Dynamics::staticGainDb (hard, -20.0), 0.0),
           "compressor curve: nothing below the knee, 3/4 of each dB over taken off above it, the knee's middle at a 1/8 of its width times that");

    // Steps: a constant -40 dB (below the knee), then a constant -6 dB, then -40 dB again, on two channels
    // with the right channel at half the left's level.
    constexpr int segment = 48000;
    juce::AudioBuffer<float> steps (2, 3 * segment);

    for (int i = 0; i < steps.getNumSamples(); ++i)
    {
        const auto level = (float) std::pow (10.0, (i >= segment && i < 2 * segment ? -6.0 : -40.0) / 20.0);
        steps.setSample (0, i, level);
        steps.setSample (1, i, -0.5f * level);
    }

    const auto stepsSnap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (steps));
    const auto squashed = readAll (*Dynamics::compress (*stepsSnap, 0, 3 * segment, rate, c));
    const auto gainAt = [&] (int i) { return db ((double) squashed.getSample (0, i) / steps.getSample (0, i)); };
    const auto target = g (-6.0);
    const auto attackFrame = segment + (int) (c.attackMs / 1000.0 * rate) - 1;
    const auto releaseFrame = 2 * segment + (int) (c.releaseMs / 1000.0 * rate) - 1;
    const auto settledLow = std::abs (gainAt (segment - 1)) < 1.0e-6;
    const auto settledHigh = near (gainAt (2 * segment - 1), target, 1.0e-4);
    const auto attackRight = near (gainAt (attackFrame), target * (1.0 - std::exp (-1.0)), 0.01);
    const auto releaseRight = near (gainAt (releaseFrame), target * std::exp (-1.0), 0.01);
    auto linked = true;

    for (int i = 0; i < squashed.getNumSamples() && linked; ++i)
        linked = juce::exactlyEqual (squashed.getSample (1, i), -0.5f * squashed.getSample (0, i)) || near (squashed.getSample (1, i), -0.5 * squashed.getSample (0, i), 1.0e-7);

    std::cout << "      compressor: settled at " << gainAt (2 * segment - 1) << " dB (static " << target << "), after the attack time "
              << gainAt (attackFrame) << ", after the release time " << gainAt (releaseFrame) << std::endl;
    check (settledLow && settledHigh && attackRight && releaseRight && linked,
           "compressor on steps: no gain change below the knee; settles on the static curve to 1e-4 dB; 63 % of the way down after the attack time and back up after the release time (to 0.01 dB); both channels get the same gain");

    auto loud = c;
    loud.makeupDb = 6.0;
    const auto made = readAll (*Dynamics::compress (*stepsSnap, 0, 3 * segment, rate, loud));
    const auto makeupRight = near (db ((double) made.getSample (0, 2 * segment - 1) / squashed.getSample (0, 2 * segment - 1)), 6.0, 1.0e-4);
    const auto part = readAll (*Dynamics::compress (*stepsSnap, segment, 2 * segment, rate, c));
    auto outsideKept = true;

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 3 * segment; ++i)
            if (i < segment || i >= 2 * segment)
                outsideKept = outsideKept && juce::exactlyEqual (part.getSample (ch, i), steps.getSample (ch, i));

    auto badRatio = c;
    badRatio.ratio = 0.5;
    auto badAttack = c;
    badAttack.attackMs = 0.0;
    check (makeupRight && outsideKept && ! Dynamics::compress (*stepsSnap, 0, 100, rate, badRatio) && ! Dynamics::compress (*stepsSnap, 0, 100, rate, badAttack)
               && ! Dynamics::compress (*stepsSnap, 10, 10, rate, c),
           "compressor: makeup adds its gain; frames outside the range bit for bit; a ratio under 1, no attack or an empty range refused");

    // The limiter: half a second of a quiet sine, then a 997 Hz sine peaking at +6 dBFS, then random
    // peaks up to +12 dBFS, then quiet again, on two channels.
    juce::Random random (1234);
    constexpr int quiet = 24000, sineLength = 48000, noiseLength = 24000, total = quiet + sineLength + noiseLength + quiet;
    juce::AudioBuffer<float> hot (2, total);

    for (int i = 0; i < total; ++i)
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto s = std::sin (juce::MathConstants<double>::twoPi * 997.0 * i / rate + ch);
            const auto inSine = i >= quiet && i < quiet + sineLength;
            const auto inNoise = i >= quiet + sineLength && i < quiet + sineLength + noiseLength;
            hot.setSample (ch, i, inSine ? (float) (2.0 * s) : inNoise ? 4.0f * (random.nextFloat() * 2.0f - 1.0f) : (float) (0.1 * s));
        }

    const auto hotSnap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (hot));
    const Dynamics::Limiter l { 0.0, -1.0, 50.0, 5.0 };
    const auto limited = readAll (*Dynamics::limit (*hotSnap, 0, total, rate, l));
    const auto ceiling = (float) std::pow (10.0, -1.0 / 20.0);
    const auto ahead = (int) std::lround (0.005 * rate);
    float worst = 0.0f, sinePeak = 0.0f;
    auto beforeKept = true;

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < total; ++i)
        {
            worst = juce::jmax (worst, std::abs (limited.getSample (ch, i)));

            if (i < quiet - ahead - 1)
                beforeKept = beforeKept && juce::exactlyEqual (limited.getSample (ch, i), hot.getSample (ch, i));

            if (i >= quiet + sineLength / 2 && i < quiet + sineLength)
                sinePeak = juce::jmax (sinePeak, std::abs (limited.getSample (ch, i)));
        }

    std::cout << "      limiter: loudest sample " << db (worst) << " dBFS, the sine's peak once settled " << db (sinePeak) << " dBFS" << std::endl;
    check (worst <= ceiling && beforeKept && near (db (sinePeak), -1.0, 0.1),
           "limiter: no sample of a +6 dBFS sine or +12 dBFS noise above a -1 dBFS ceiling; the steady sine held at the ceiling (to 0.1 dB); everything before the look-ahead bit for bit");

    // Input gain into the ceiling, and a gain that only falls as far as each peak needs.
    const auto pushed = readAll (*Dynamics::limit (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (steps)), 0, 3 * segment, rate,
                                                   Dynamics::Limiter { 30.0, -3.0, 50.0, 5.0 }));
    const auto pushedLow = near (db ((double) pushed.getSample (0, segment / 2) / steps.getSample (0, segment / 2)), 30.0, 1.0e-4)
                         && pushed.getSample (0, segment - 1) < pushed.getSample (0, segment / 2);  // already falling into the step
    const auto pushedHigh = near (db (pushed.getSample (0, 2 * segment - 1)), -3.0, 1.0e-4);
    check (pushedLow && pushedHigh && ! Dynamics::limit (*hotSnap, 0, 100, rate, Dynamics::Limiter { 0.0, 1.0, 50.0, 5.0 })
               && ! Dynamics::limit (*hotSnap, 0, 100, rate, Dynamics::Limiter { 0.0, -1.0, 0.0, 5.0 }),
           "limiter with 30 dB of input gain: a -40 dB stretch comes up 30 dB, falling only in the look-ahead before the loud part, a -6 dB stretch sits exactly at a -3 dBFS ceiling; a ceiling above 0 or no release refused");

    // True peak: a quarter-rate sine with its crests between samples, so the samples sit 3 dB below
    // the waveform's 0 dBTP peak (its abrupt start and end ring a little above that).
    constexpr int quarterLength = 96000;
    juce::AudioBuffer<float> between (2, quarterLength);

    for (int i = 0; i < quarterLength; ++i)
        for (int ch = 0; ch < 2; ++ch)
            between.setSample (ch, i, (float) std::sin (juce::MathConstants<double>::halfPi * i + juce::MathConstants<double>::pi / 4.0));

    const auto betweenSnap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (between));
    const auto measuredBefore = Loudness::measure (*betweenSnap, 0, quarterLength, rate)->truePeak;
    const auto sampleOnly = readAll (*Dynamics::limit (*betweenSnap, 0, quarterLength, rate, Dynamics::Limiter { 0.0, -1.0, 50.0, 5.0, false }));
    auto sampleUntouched = true;

    for (int i = 0; i < quarterLength && sampleUntouched; ++i)
        sampleUntouched = juce::exactlyEqual (sampleOnly.getSample (0, i), between.getSample (0, i));

    const auto truePeakSnap = Dynamics::limit (*betweenSnap, 0, quarterLength, rate, Dynamics::Limiter { 0.0, -1.0, 50.0, 5.0, true });
    const auto measuredAfter = Loudness::measure (*truePeakSnap, 0, quarterLength, rate)->truePeak;
    const auto ownAfter = Dynamics::truePeakDb (*truePeakSnap, 0, quarterLength, rate);
    std::cout << "      true peak: before " << measuredBefore << " dBTP, after " << measuredAfter << " dBTP (the limiter's own reading " << ownAfter << ")" << std::endl;
    check (measuredBefore > -0.1 && sampleUntouched && measuredAfter <= -1.0 + 1.0e-3 && measuredAfter > -1.1 && near (ownAfter, measuredAfter, 1.0e-6),
           "true-peak limiter: a quarter-rate sine at about 0 dBTP with samples at -3 dBFS is left alone by sample-peak limiting at -1, and held at -1 dBTP by true-peak limiting (within 0.1 dB below, at most 0.001 dB above), as Loudness measures it");

    // Matching loudness through the limiter: noise with a click every half second up to -3 dBFS, so
    // plain gain to -14 LUFS would stop short at the -1 dBTP ceiling.
    constexpr int tenSeconds = 480000;
    juce::AudioBuffer<float> peaky (2, tenSeconds);
    juce::Random noise (99);

    for (int i = 0; i < tenSeconds; ++i)
        for (int ch = 0; ch < 2; ++ch)
            peaky.setSample (ch, i, 0.05f * (noise.nextFloat() * 2.0f - 1.0f) + (i % 24000 == 100 ? 0.65f : 0.0f));

    const auto peakySnap = AudioEdits::fromBuffer (std::move (peaky));
    const auto before = *Loudness::measure (*peakySnap, 0, tenSeconds, rate);
    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto matched = Dynamics::matchLoudness (*peakySnap, 0, tenSeconds, rate, before.integrated, -14.0, -1.0);
    const auto seconds = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
    const auto after = *Loudness::measure (*matched.snapshot, 0, tenSeconds, rate);
    std::cout << "      match through the limiter: " << before.integrated << " LUFS and " << before.truePeak << " dBTP before; "
              << after.integrated << " LUFS and " << after.truePeak << " dBTP after, with " << matched.gainDb << " dB of gain, in " << seconds << " s for 10 s of stereo" << std::endl;
    check (before.truePeak > -1.0 - (-14.0 - before.integrated) && near (after.integrated, -14.0, 0.05) && after.truePeak <= -1.0 + 1.0e-3
               && near (matched.integrated, after.integrated, 1.0e-9) && ! Dynamics::matchLoudness (*peakySnap, 5, 5, rate, before.integrated, -14.0, -1.0).snapshot,
           "match loudness through the limiter: peaky audio that plain gain could raise only part of the way reaches -14 LUFS (to 0.05 LU) with its true peak at or under -1 dBTP; an empty range refused");
}
