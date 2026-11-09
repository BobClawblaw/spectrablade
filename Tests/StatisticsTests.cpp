#include <JuceHeader.h>
#include "../Source/Statistics.h"
#include "TestUtils.h"
#include "StatisticsTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;

    bool near (double a, double b, double tolerance)
    {
        return std::abs (a - b) <= tolerance;
    }

    /** A 16-bit file's value nearest x. */
    float sixteenBit (double x)
    {
        return (float) (std::round (x * 32768.0) / 32768.0);
    }

    juce::String fmt (double v)
    {
        return juce::String (v, 3);
    }
}

void runStatisticsTests()
{
    std::cout << "Statistics:" << std::endl;

    // A stereo buffer, 2 s. Left: a 1 kHz sine at half scale (-6.02 dBFS, RMS -9.03 dB), steady,
    // so its loudest and quietest 50 ms windows are alike. Right: a 16-bit square wave at full
    // scale for 1 s, one at a tenth (-20 dB) for 0.5 s, then silence.
    {
        const int frames = (int) (2.0 * rate);
        juce::AudioBuffer<float> buffer (2, frames);

        for (int i = 0; i < frames; ++i)
        {
            buffer.setSample (0, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / rate));
            const auto square = (i / 24) % 2 == 0 ? 1.0 : -1.0;
            buffer.setSample (1, i, i < rate ? sixteenBit (square * 32767.0 / 32768.0)
                                 : i < rate * 1.5 ? sixteenBit (square * 0.1)
                                 : 0.0f);
        }

        const auto r = Statistics::measure (buffer, rate);
        check (r.channels.size() == 2 && r.end == frames && juce::exactlyEqual (r.sampleRate, rate), "statistics: a channel each, over the whole buffer");

        const auto& left = r.channels[0];
        check (near (left.peakDb, -6.02, 0.02), "left peak: -6.02 dBFS (" + fmt (left.peakDb) + ")");
        check (left.peakAt == 12, "left peak at the first crest, frame 12 (" + juce::String (left.peakAt) + ")");
        check (left.truePeakDb >= left.peakDb - 1.0e-6 && left.truePeakDb < left.peakDb + 0.1,
               "left true peak: at or just above the sample peak (" + fmt (left.truePeakDb) + ")");
        check (near (left.rmsDb, -9.03, 0.05), "left RMS: -9.03 dB (" + fmt (left.rmsDb) + ")");
        check (left.windows == 40, "left: 40 windows of 50 ms in 2 s (" + juce::String (left.windows) + ")");
        check (near (left.maxRmsDb, -9.03, 0.05) && near (left.minRmsDb, -9.03, 0.05) && left.dynamicRange() < 0.1,
               "left: the loudest and quietest windows alike, dynamic range under 0.1 dB (" + fmt (left.dynamicRange()) + ")");
        check (near (left.minimum, -0.5f, 0.001f) && near (left.maximum, 0.5f, 0.001f), "left: samples from -0.5 to 0.5");
        check (left.fullScaleSamples == 0 && near (left.dcOffset, 0.0, 1.0e-4), "left: nothing at full scale, no DC offset");
        check (left.bitDepth == 32, "left: a sine of floats needs 32 bits (" + juce::String (left.bitDepth) + ")");
        check (left.histogram[(size_t) (100 - 10)] == 40, "left histogram: every window in the -10 dB bin");

        const auto& right = r.channels[1];
        check (near (right.peakDb, 0.0, 0.001), "right peak: full scale (" + fmt (right.peakDb) + ")");
        check (right.fullScaleSamples == (juce::int64) rate, "right: every sample of the first second at full scale (" + juce::String (right.fullScaleSamples) + ")");
        check (right.bitDepth == 16, "right: a 16-bit square wave needs 16 bits (" + juce::String (right.bitDepth) + ")");
        check (near (right.maxRmsDb, 0.0, 0.01) && near (right.minRmsDb, -20.0, 0.05),
               "right: loudest window 0 dB, quietest with sound -20 dB (" + fmt (right.maxRmsDb) + ", " + fmt (right.minRmsDb) + ")");
        check (near (right.dynamicRange(), 20.0, 0.05), "right dynamic range: 20 dB (" + fmt (right.dynamicRange()) + ")");
        check (right.histogram[99] == 20 && right.histogram[80] == 10 && right.histogram[0] == 10,
               "right histogram: 20 windows at 0 dB, 10 at -20 dB, 10 silent");
        check (near (right.truePeakDb, 0.0, 1.0) || right.truePeakDb > 0.0, "right true peak: a square wave's overshoot read (" + fmt (right.truePeakDb) + ")");

        const auto loudness = Loudness::measure (buffer, rate);
        check (juce::exactlyEqual (r.loudness.integrated, loudness.integrated) && juce::exactlyEqual (r.loudness.range, loudness.range)
               && juce::exactlyEqual (r.loudness.truePeak, loudness.truePeak),
               "loudness measured alongside, as Loudness measures it (" + fmt (r.loudness.integrated) + " LUFS)");

        // The rows and the text.
        const auto rows = Statistics::rows (r, [] (juce::int64 f) { return juce::String (f); });
        check (rows.size() == 16 && rows[0].name == "Peak Amplitude" && rows[0].values.size() == 2 && rows[12].name == "Integrated Loudness" && rows[12].values.size() == 1,
               "rows: 12 channel rows with two values, then 4 loudness rows with one");
        check (rows[1].values[0] == "12" && rows[11].values[0] == "32-bit float" && rows[11].values[1] == "16-bit" && rows[5].values[1] == "48000",
               "rows: the peak's time through the formatter, the bit depths and the full-scale count as text");
        const auto text = Statistics::toText (r, [] (juce::int64 f) { return juce::String (f); });
        check (text.startsWith ("Statistics\t0 to 96000\n") && text.contains ("\tLeft\tRight\n") && text.contains ("\nPeak Amplitude\t-6.02 dB\t-0.00 dB\n"),
               "text: a heading, the channel names and tab-separated rows");
    }

    // DC and bit depths: a constant at a 16-bit value has that DC offset; multiples of 1/128 fit
    // 8 bits and multiples of 1/8388608 need 24; an empty range measures nothing.
    {
        juce::AudioBuffer<float> buffer (3, 4800);

        for (int i = 0; i < 4800; ++i)
        {
            buffer.setSample (0, i, sixteenBit (0.1));
            buffer.setSample (1, i, (float) ((i % 7) - 3) / 128.0f);
            buffer.setSample (2, i, (float) ((i % 5) - 2) / 8388608.0f);
        }

        const auto r = Statistics::measure (buffer, rate);
        check (near (r.channels[0].dcOffset, 0.1, 1.0e-4) && r.channels[0].bitDepth == 16, "a constant 16-bit value: DC offset 10%, 16 bits");
        check (r.channels[1].bitDepth == 8, "multiples of 1/128: 8 bits (" + juce::String (r.channels[1].bitDepth) + ")");
        check (r.channels[2].bitDepth == 24, "multiples of 1/8388608: 24 bits (" + juce::String (r.channels[2].bitDepth) + ")");
        check (r.channels[0].windows == 2 && r.channels[0].dynamicRange() == 0.0, "100 ms of a constant: two windows, no dynamic range");

        const juce::AudioBuffer<float> empty (2, 0);
        const auto e = Statistics::measure (empty, rate);
        check (e.channels.size() == 2 && ! std::isfinite (e.channels[0].peakDb) && e.channels[0].windows == 0 && e.channels[0].bitDepth == 8,
               "an empty range: -inf peaks, no windows");
        check (Statistics::rows (e, [] (juce::int64 f) { return juce::String (f); })[1].values[0] == "-", "an empty range: no peak time");
    }

    // Names and formats.
    check (Statistics::channelName (0, 1) == "Mono" && Statistics::channelName (1, 2) == "Right" && Statistics::channelName (3, 6) == "LFE"
           && Statistics::channelName (7, 8) == "Right Side" && Statistics::channelName (2, 4) == "Channel 3",
           "channel names: Mono, Left and Right, the 5.1 and 7.1 names, Channel N otherwise");
    check (Statistics::formatDb (-6.0206) == "-6.02 dB" && Statistics::formatDb (-HUGE_VAL) == "-inf dB" && Statistics::formatDb (-1.0, "dBTP") == "-1.00 dBTP",
           "formatDb: two decimals, -inf, the unit given");

    // The panel: Scan measures what readSource gives in the background and shows the rows; a
    // changed source is said so; Scan with nothing to measure says so.
    {
        juce::AudioBuffer<float> buffer (1, 4800);

        for (int i = 0; i < 4800; ++i)
            buffer.setSample (0, i, 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / rate));

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (juce::AudioBuffer<float> (buffer)), 0, 4800 } };
        auto snapshot = std::make_shared<const AudioSnapshot> (1, segments);
        auto source = Statistics::Panel::Source { snapshot, 0, 4800, rate };

        Statistics::Panel panel;
        panel.setSize (400, 400);
        panel.readSource = [&] { return std::optional<Statistics::Panel::Source> (source); };
        panel.formatTime = [] (juce::int64 f) { return juce::String (f) + " fr"; };

        panel.scan();
        check (TestUtils::runUntil ([&] { return ! panel.isScanning() && panel.getResult().has_value(); }, 5000),
               "panel: Scan measures in the background and the result arrives");
        check (panel.getResult().has_value() && near (panel.getResult()->channels[0].peakDb, -12.04, 0.05), "panel: the result is of the source (" + (panel.getResult().has_value() ? fmt (panel.getResult()->channels[0].peakDb) : "none") + ")");
        check (panel.getStatus() == "Measured 0 fr to 4800 fr.", "panel: the status says what was measured (" + panel.getStatus() + ")");

        source.end = 2400;
        juce::MessageManager::getInstance()->runDispatchLoopUntil (250);
        check (panel.getStatus().contains ("changed since"), "panel: a changed selection is said so (" + panel.getStatus() + ")");

        panel.readSource = [] { return std::optional<Statistics::Panel::Source>(); };
        panel.scan();
        check (! panel.isScanning() && panel.getStatus().startsWith ("Open a file"), "panel: Scan with nothing to measure says so");
    }
}
