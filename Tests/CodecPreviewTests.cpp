#include <JuceHeader.h>
#include "../Source/CodecPreview.h"
#include "../Source/AudioEdits.h"
#include "TestUtils.h"
#include "CodecPreviewTests.h"

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

    /** Three seconds of a stereo tone with harmonics and a little noise, with a 1 s silence first
        (so the range tested starts inside the file). */
    juce::AudioBuffer<float> music()
    {
        juce::Random r (41);
        juce::AudioBuffer<float> b (2, 4 * 48000);
        b.clear();

        for (int i = 48000; i < b.getNumSamples(); ++i)
        {
            const auto t = i / rate;
            auto v = 0.01 * (r.nextDouble() * 2.0 - 1.0);

            for (int h = 1; h <= 6; ++h)
                v += 0.15 / h * std::sin (juce::MathConstants<double>::twoPi * 330.0 * h * t);

            b.setSample (0, i, (float) v);
            b.setSample (1, i, (float) (0.8 * v));
        }

        return b;
    }

    double rmsDb (const juce::AudioBuffer<float>& b, int ch, int from, int to)
    {
        return 20.0 * std::log10 (juce::jmax (1.0e-10f, b.getRMSLevel (ch, from, to - from)));
    }
}

void runCodecPreviewTests()
{
    std::cout << "Codec preview:" << std::endl;

    // The range round-tripped through MP3 at 192 kbps and AAC: lined up with the original (the
    // error against it under -20 dB and over -60: changed, but not much), its level within 0.5 dB,
    // the audio outside the range untouched; a bad bit rate is refused; cancelling.
    {
        const auto in = music();
        const auto snap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (in));
        juce::String error;
        auto worstLevel = 0.0, worstError = -200.0, bestError = 0.0;
        auto outsideKept = true, delaysSeen = true;

        for (auto kind : { LossyFiles::Kind::mp3, LossyFiles::Kind::aac })
        {
            LossyFiles::Settings s;
            s.kind = kind;
            s.kbps = 192;
            const auto r = CodecPreview::roundTrip (*snap, 48000, 4 * 48000, rate, s, error);

            if (! r.has_value())
            {
                std::cout << "      " << (kind == LossyFiles::Kind::mp3 ? "MP3" : "AAC") << ": " << error << std::endl;
                check (false, "codec preview: the round trip failed");
                return;
            }

            const auto out = readAll (*r->snapshot);
            const auto level = rmsDb (out, 0, 60000, 180000) - rmsDb (in, 0, 60000, 180000);
            juce::AudioBuffer<float> difference (1, 120000);

            for (int i = 0; i < 120000; ++i)
                difference.setSample (0, i, out.getSample (0, 60000 + i) - in.getSample (0, 60000 + i));

            const auto errorDb = rmsDb (difference, 0, 0, 120000) - rmsDb (in, 0, 60000, 180000);
            worstLevel = juce::jmax (worstLevel, std::abs (level));
            worstError = juce::jmax (worstError, errorDb);
            bestError = juce::jmin (bestError, errorDb);

            for (int i = 0; i < 48000; ++i)
                outsideKept = outsideKept && juce::exactlyEqual (out.getSample (0, i), in.getSample (0, i));

            delaysSeen = delaysSeen && r->delayFrames >= 0 && r->delayFrames < 8192;
            std::cout << "      " << (kind == LossyFiles::Kind::mp3 ? "MP3" : "AAC") << " at 192 kbps: delay " << r->delayFrames << " frames, " << juce::roundToInt (r->bytesPerSecond * 8.0 / 1000.0)
                      << " kbps on disk, level " << level << " dB, the error against the original " << errorDb << " dB" << std::endl;
        }

        LossyFiles::Settings bad;
        bad.kbps = 200;
        const auto refused = ! CodecPreview::roundTrip (*snap, 48000, 4 * 48000, rate, bad, error).has_value();
        LossyFiles::Settings mp3;
        const auto cancelled = ! CodecPreview::roundTrip (*snap, 48000, 4 * 48000, rate, mp3, error, [] (float) { return false; }).has_value();

        check (worstLevel < 0.5 && worstError < -20.0 && bestError > -60.0 && outsideKept && delaysSeen && refused && cancelled,
               "codec preview: MP3 and AAC at 192 kbps come back lined up with the original (the error against it between -60 and -20 dB) with the "
               "level within 0.5 dB and the audio before the range untouched; a bit rate the formats don't offer is refused; it can be cancelled");
    }
}
