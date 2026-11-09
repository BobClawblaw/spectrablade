#include <JuceHeader.h>
#include "../Source/TimeFormat.h"
#include "../Source/TimeEntry.h"
#include "TestUtils.h"
#include "TimeFormatTests.h"

namespace
{
    using TestUtils::check;
    using Kind = TimeFormat::Kind;

    constexpr double rate = 48000.0;

    struct Listener final : juce::ChangeListener
    {
        int heard = 0;
        void changeListenerCallback (juce::ChangeBroadcaster*) override   { ++heard; }
    };
}

void runTimeFormatTests()
{
    std::cout << "Time formats:" << std::endl;

    // 83.304 s at 48 kHz in every format.
    {
        const juce::int64 f = 3998592;
        check (TimeFormat::format (f, rate, Kind::minutesSeconds) == "1:23.304" && TimeFormat::format (f, rate, Kind::decimalSeconds) == "83.304"
               && TimeFormat::format (f, rate, Kind::samples) == "3998592",
               "format: minutes and seconds, decimal seconds, samples");
        check (TimeFormat::format (f, rate, Kind::smpte24) == "00:01:23:07" && TimeFormat::format (f, rate, Kind::smpte25) == "00:01:23:07"
               && TimeFormat::format (f, rate, Kind::smpte30) == "00:01:23:09",
               "format: timecode at 24, 25 and 30 fps (" + TimeFormat::format (f, rate, Kind::smpte24) + ", " + TimeFormat::format (f, rate, Kind::smpte25) + ", " + TimeFormat::format (f, rate, Kind::smpte30) + ")");
        check (TimeFormat::format (f, rate, Kind::smpte2997) == "00:01:23:06" && TimeFormat::format (f, rate, Kind::smpte2997Drop) == "00:01:23;08",
               "format: 29.97 non-drop lags, drop frame counts two frames on at the minute (" + TimeFormat::format (f, rate, Kind::smpte2997) + ", " + TimeFormat::format (f, rate, Kind::smpte2997Drop) + ")");
        check (TimeFormat::format ((juce::int64) (3600.0 * rate), rate, Kind::minutesSeconds) == "1:00:00.000"
               && TimeFormat::format ((juce::int64) (3600.0 * rate), rate, Kind::smpte30) == "01:00:00:00"
               && TimeFormat::format ((juce::int64) (3600.0 * rate), rate, Kind::smpte2997Drop) == "01:00:00;00",
               "format: an hour, in minutes and seconds and in timecode (drop frame back on the clock at the hour)");
        check (TimeFormat::format (-5, rate, Kind::samples) == "0" && TimeFormat::format (f, 0.0, Kind::samples) == "-", "format: negative frames read 0, no rate reads a dash");
    }

    // Parsing, and round trips.
    {
        check (TimeFormat::parse ("3998592", rate, Kind::samples) == 3998592 && ! TimeFormat::parse ("83.5", rate, Kind::samples).has_value(),
               "parse: samples take a whole number only");
        check (TimeFormat::parse ("83.304", rate, Kind::decimalSeconds) == 3998592 && TimeFormat::parse ("1:23.304", rate, Kind::minutesSeconds) == 3998592
               && TimeFormat::parse ("1:01:23.5 s", rate, Kind::minutesSeconds) == (juce::int64) (3683.5 * rate) && ! TimeFormat::parse ("1:75", rate, Kind::minutesSeconds).has_value(),
               "parse: seconds, m:ss and h:mm:ss, seconds under 60");
        check (TimeFormat::parse ("00:01:23:07", rate, Kind::smpte24) == 3998000 && TimeFormat::parse ("01:23:07", rate, Kind::smpte24) == 3998000,
               "parse: timecode with and without hours, the first sample of the frame");
        check (TimeFormat::parse ("00:01:00;02", rate, Kind::smpte2997Drop) == 2882880 && ! TimeFormat::parse ("00:01:00;00", rate, Kind::smpte2997Drop).has_value()
               && TimeFormat::parse ("00:10:00;00", rate, Kind::smpte2997Drop).has_value(),
               "parse: drop frame skips 00 and 01 at a minute that is not a tenth");
        check (! TimeFormat::parse ("00:00:60:00", rate, Kind::smpte25).has_value() && ! TimeFormat::parse ("00:00:10:25", rate, Kind::smpte25).has_value()
               && ! TimeFormat::parse ("1:23.5", rate, Kind::smpte25).has_value() && ! TimeFormat::parse ("", rate, Kind::smpte25).has_value(),
               "parse: refuses seconds or frames out of range, fractions and nothing");

        juce::Random random (7);
        int roundTrips = 0;

        for (int i = 0; i < 400; ++i)
        {
            const auto frames = (juce::int64) random.nextInt (juce::Range<int> (0, 4 * 3600 * 48000));

            for (int k = 0; k < (int) Kind::count; ++k)
            {
                const auto kind = (Kind) k;
                const auto text = TimeFormat::format (frames, rate, kind);
                const auto back = TimeFormat::parse (text, rate, kind);

                // Seconds are written to the millisecond, so the text names a frame within half of
                // one; a sample count is exact, and a timecode names the first frame of the one shown.
                const auto exactEnough = kind == Kind::minutesSeconds || kind == Kind::decimalSeconds ? std::abs (*back - frames) <= 24 : *back <= frames;

                if (back.has_value() && exactEnough && TimeFormat::format (*back, rate, kind) == text)
                    ++roundTrips;
            }
        }

        check (roundTrips == 400 * (int) Kind::count, "parse: every format's text parses back to a frame that shows the same, within half a millisecond for seconds and at or before it for the rest (" + juce::String (roundTrips) + " of 3200)");
    }

    // Ruler marks and labels.
    {
        check (juce::approximatelyEqual (TimeFormat::rulerTick (0.001, rate, Kind::minutesSeconds), 0.1)
               && juce::approximatelyEqual (TimeFormat::rulerTick (1.0 / rate, rate, Kind::samples), 100.0 / rate)
               && juce::approximatelyEqual (TimeFormat::rulerTick (0.0004, rate, Kind::smpte25), 0.04)
               && juce::approximatelyEqual (TimeFormat::rulerTick (0.0004, rate, Kind::smpte30), 2.0 / 30.0)
               && juce::approximatelyEqual (TimeFormat::rulerTick (1.0, rate, Kind::smpte25), 120.0),
               "ruler ticks: 1-2-5 seconds, 1-2-5 samples, frames of timecode then seconds");
        check (TimeFormat::rulerLabel (83.3, 0.1, rate, Kind::minutesSeconds) == "1:23.3" && TimeFormat::rulerLabel (83.3, 0.1, rate, Kind::decimalSeconds) == "83.3"
               && TimeFormat::rulerLabel (83.3, 100.0 / rate, rate, Kind::samples) == "3998400" && TimeFormat::rulerLabel (83.304, 0.04, rate, Kind::smpte25) == "01:23:07"
               && TimeFormat::rulerLabel (3600.0, 60.0, rate, Kind::smpte25) == "01:00:00:00" && TimeFormat::rulerLabel (-0.5, 1.0, rate, Kind::decimalSeconds) == "0",
               "ruler labels: the decimals the tick needs, the sample count, timecode without hours until there are some");
    }

    // The format in use: the setting's keys, listeners, and TimeEntry reading it.
    {
        check (TimeFormat::get() == Kind::minutesSeconds && TimeFormat::names().size() == (int) Kind::count
               && TimeFormat::fromKey (TimeFormat::key (Kind::smpte2997Drop)) == Kind::smpte2997Drop && TimeFormat::fromKey ("nonsense") == Kind::minutesSeconds,
               "the setting: minutes and seconds by default, a name and a key for each format, an unknown key reads the default");

        Listener listener;
        TimeFormat::broadcaster().addChangeListener (&listener);
        TimeFormat::set (Kind::samples);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        check (TimeFormat::get() == Kind::samples && listener.heard == 1, "set: the format changes and listeners hear of it");
        TimeFormat::set (Kind::samples);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        check (listener.heard == 1, "set: the same format again says nothing");

        check (TimeEntry::parse ("12345", rate) == 12345 && TimeEntry::parse ("5 s", rate) == 240000 && TimeEntry::parse ("1:23.5", rate) == (juce::int64) (83.5 * rate)
               && TimeEntry::format (48000, rate) == "48000",
               "TimeEntry on Samples: a whole number is samples, seconds and m:ss still read, times show as samples");
        TimeFormat::set (Kind::smpte25);
        check (TimeEntry::parse ("00:00:01:00", rate) == 48000 && TimeEntry::parse ("00:01:00", rate) == 48000 && TimeEntry::parse ("2.5", rate) == 120000
               && TimeEntry::format (48000, rate) == "00:00:01:00",
               "TimeEntry on timecode: hh:mm:ss:ff and mm:ss:ff read at 25 fps, seconds still read, times show as timecode");
        TimeFormat::set (Kind::minutesSeconds);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        TimeFormat::broadcaster().removeChangeListener (&listener);
        check (TimeFormat::get() == Kind::minutesSeconds && TimeEntry::format (48000, rate) == "0:01.000", "back to minutes and seconds");
    }
}
