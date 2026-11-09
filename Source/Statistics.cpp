#include "Statistics.h"
#include "SampleRateConverter.h"
#include "Theme.h"

namespace Statistics
{
    namespace
    {
        constexpr float fullScale = 32767.0f / 32768.0f;   // the loudest a 16-bit file reaches

        double toDb (double value)
        {
            return value > 0.0 ? 20.0 * std::log10 (value) : -HUGE_VAL;
        }

        /** Running totals of one channel. */
        struct Running
        {
            float peak = 0.0f;
            juce::int64 peakAt = 0;
            float minimum = 0.0f, maximum = 0.0f;
            juce::int64 fullScaleSamples = 0;
            double sum = 0.0, sumSquares = 0.0;
            double windowSum = 0.0;
            int windowFill = 0;
            double maxRms = 0.0, minRms = HUGE_VAL;
            bool anyWindowWithSound = false;
            int bits = 8;
            std::array<juce::int64, histogramBins> histogram {};
            juce::int64 windows = 0;

            void window (double rms)
            {
                ++windows;
                maxRms = juce::jmax (maxRms, rms);

                if (rms > 0.0)
                {
                    minRms = juce::jmin (minRms, rms);
                    anyWindowWithSound = true;
                }

                const auto db = toDb (rms);
                const auto bin = db <= -100.0 ? 0 : juce::jlimit (0, histogramBins - 1, (int) std::floor (db + 100.0));
                ++histogram[(size_t) bin];
            }

            void add (const float* samples, int count, juce::int64 firstFrame, int windowFrames)
            {
                for (int i = 0; i < count; ++i)
                {
                    const auto x = samples[i];
                    const auto a = std::abs (x);

                    if (a > peak)
                    {
                        peak = a;
                        peakAt = firstFrame + i;
                    }

                    minimum = juce::jmin (minimum, x);
                    maximum = juce::jmax (maximum, x);

                    if (a >= fullScale)
                        ++fullScaleSamples;

                    sum += x;
                    sumSquares += (double) x * x;
                    windowSum += (double) x * x;

                    if (++windowFill == windowFrames)
                    {
                        window (std::sqrt (windowSum / windowFrames));
                        windowSum = 0.0;
                        windowFill = 0;
                    }

                    // The bits a sample needs: 24 when it is a whole number of 24-bit steps, 16 or 8
                    // when that number is a whole number of 16- or 8-bit steps too, 32 (a float)
                    // otherwise.
                    if (bits < 32)
                    {
                        const auto steps = (double) x * 8388608.0;
                        const auto whole = std::round (steps);

                        if (std::abs (steps - whole) > 1.0e-3 || a > 1.0f)
                            bits = 32;
                        else if (bits < 24 && std::fmod (whole, 256.0) != 0.0)
                            bits = 24;
                        else if (bits < 16 && std::fmod (whole, 65536.0) != 0.0)
                            bits = 16;
                    }
                }
            }

            Channel finish (juce::int64 frames, double truePeakDb)
            {
                // A trailing part of a window counts only when there is no whole one.
                if (windows == 0 && windowFill > 0)
                    window (std::sqrt (windowSum / windowFill));

                Channel c;
                c.peakDb = toDb (peak);
                c.peakAt = peakAt;
                c.minimum = minimum;
                c.maximum = maximum;
                c.truePeakDb = juce::jmax (truePeakDb, c.peakDb);
                c.fullScaleSamples = fullScaleSamples;
                c.dcOffset = frames > 0 ? sum / (double) frames : 0.0;
                c.rmsDb = frames > 0 ? toDb (std::sqrt (sumSquares / (double) frames)) : -HUGE_VAL;
                c.maxRmsDb = windows > 0 ? toDb (maxRms) : -HUGE_VAL;
                c.minRmsDb = anyWindowWithSound ? toDb (minRms) : -HUGE_VAL;
                c.bitDepth = bits;
                c.histogram = histogram;
                c.windows = windows;
                return c;
            }
        };
    }

    double Channel::dynamicRange() const noexcept
    {
        if (windows < 2 || ! std::isfinite (maxRmsDb) || ! std::isfinite (minRmsDb))
            return 0.0;

        return maxRmsDb - minRmsDb;
    }

    std::optional<Result> measure (const AudioSnapshot& snapshot, juce::int64 start, juce::int64 end, double sampleRate,
                                   const std::function<bool (float)>& progress)
    {
        constexpr int chunk = 1 << 16;
        constexpr int context = 256;   // frames either side of a chunk, so the oversampling filter has context
        const auto channels = snapshot.getNumChannels();
        const auto frames = juce::jmax ((juce::int64) 0, end - start);
        const auto windowFrames = juce::jmax (1, (int) std::llround (windowSeconds * sampleRate));
        std::vector<Running> running ((size_t) channels);
        std::vector<double> truePeaks ((size_t) channels, 0.0);
        juce::AudioBuffer<float> buffer (channels, chunk), withContext (channels, chunk + 2 * context), oversampled;

        for (auto pos = start; pos < end; pos += chunk)
        {
            const auto count = (int) juce::jmin ((juce::int64) chunk, end - pos);
            snapshot.read (buffer, 0, pos, count);

            for (int ch = 0; ch < channels; ++ch)
                running[(size_t) ch].add (buffer.getReadPointer (ch), count, pos, windowFrames);

            // True peak, as Loudness finds it: the chunk with some of the file around it (silence
            // outside the measured range, as if it were a file of its own), oversampled 4x.
            withContext.clear();
            const auto before = (int) juce::jmin ((juce::int64) context, pos - start);
            const auto after = (int) juce::jmin ((juce::int64) context, end - (pos + count));
            snapshot.read (withContext, context - before, pos - before, before + count + after);
            juce::AudioBuffer<float> span (withContext.getArrayOfWritePointers(), channels, 0, count + 2 * context);
            SampleRateConverter::convert (span, sampleRate, sampleRate * 4.0, oversampled);

            for (int ch = 0; ch < channels; ++ch)
                truePeaks[(size_t) ch] = juce::jmax (truePeaks[(size_t) ch],
                                                     (double) oversampled.getMagnitude (ch, context * 4, count * 4));

            if (progress != nullptr && ! progress (0.6f * (float) ((double) (pos + count - start) / (double) juce::jmax ((juce::int64) 1, frames))))
                return std::nullopt;
        }

        Result r;
        r.start = start;
        r.end = end;
        r.sampleRate = sampleRate;

        for (int ch = 0; ch < channels; ++ch)
            r.channels.push_back (running[(size_t) ch].finish (frames, toDb (truePeaks[(size_t) ch])));

        if (frames > 0)
        {
            const auto loudness = Loudness::measure (snapshot, start, end, sampleRate, [&] (float p)
            {
                return progress == nullptr || progress (0.6f + 0.4f * p);
            });

            if (! loudness.has_value())
                return std::nullopt;

            r.loudness = *loudness;
        }

        if (progress != nullptr)
            progress (1.0f);

        return r;
    }

    Result measure (const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        std::vector<AudioSnapshot::Segment> segments;
        const auto frames = (juce::int64) buffer.getNumSamples();

        if (frames > 0)
            segments.push_back ({ std::make_shared<const AudioBlock> (juce::AudioBuffer<float> (buffer)), 0, frames });

        const AudioSnapshot snapshot (buffer.getNumChannels(), std::move (segments));
        return *measure (snapshot, 0, frames, sampleRate);
    }

    juce::String channelName (int channel, int numChannels)
    {
        static const juce::StringArray stereo { "Left", "Right" },
                                       fiveOne { "Left", "Right", "Centre", "LFE", "Left Surround", "Right Surround" },
                                       sevenOne { "Left", "Right", "Centre", "LFE", "Left Rear", "Right Rear", "Left Side", "Right Side" };

        if (numChannels == 1)
            return "Mono";

        const auto* names = numChannels == 2 ? &stereo : numChannels == 6 ? &fiveOne : numChannels == 8 ? &sevenOne : nullptr;

        if (names != nullptr && channel >= 0 && channel < names->size())
            return (*names)[channel];

        return "Channel " + juce::String (channel + 1);
    }

    juce::String formatDb (double value, const juce::String& unit)
    {
        if (! std::isfinite (value))
            return (value < 0.0 ? "-inf " : "inf ") + unit;

        return juce::String (value, 2) + " " + unit;
    }

    std::vector<Row> rows (const Result& r, const std::function<juce::String (juce::int64)>& formatTime)
    {
        std::vector<Row> out;
        const auto n = (int) r.channels.size();

        const auto row = [&] (const juce::String& name, const std::function<juce::String (const Channel&)>& value)
        {
            Row added { name, {} };

            for (const auto& c : r.channels)
                added.values.push_back (value (c));

            out.push_back (std::move (added));
        };

        row ("Peak Amplitude",        [] (const Channel& c) { return formatDb (c.peakDb); });
        row ("Peak At",               [&] (const Channel& c) { return std::isfinite (c.peakDb) ? formatTime (c.peakAt) : juce::String ("-"); });
        row ("True Peak",             [] (const Channel& c) { return formatDb (c.truePeakDb, "dBTP"); });
        row ("Minimum Sample",        [] (const Channel& c) { return juce::String (c.minimum, 5); });
        row ("Maximum Sample",        [] (const Channel& c) { return juce::String (c.maximum, 5); });
        row ("Samples at Full Scale", [] (const Channel& c) { return juce::String (c.fullScaleSamples); });
        row ("DC Offset",             [] (const Channel& c) { return juce::String (c.dcOffset * 100.0, 3) + " %"; });
        row ("Total RMS",             [] (const Channel& c) { return formatDb (c.rmsDb); });
        row ("Maximum RMS",           [] (const Channel& c) { return formatDb (c.maxRmsDb); });
        row ("Minimum RMS",           [] (const Channel& c) { return formatDb (c.minRmsDb); });
        row ("Dynamic Range",         [] (const Channel& c) { return formatDb (c.dynamicRange()); });
        row ("Bit Depth",             [] (const Channel& c) { return c.bitDepth == 32 ? juce::String ("32-bit float") : juce::String (c.bitDepth) + "-bit"; });

        if (n > 0)
        {
            out.push_back ({ "Integrated Loudness", { Loudness::format (r.loudness.integrated, "LUFS") } });
            out.push_back ({ "Loudness Range",      { juce::String (r.loudness.range, 1) + " LU" } });
            out.push_back ({ "Maximum Momentary",   { Loudness::format (r.loudness.maxMomentary, "LUFS") } });
            out.push_back ({ "Maximum Short-term",  { Loudness::format (r.loudness.maxShortTerm, "LUFS") } });
        }

        return out;
    }

    juce::String toText (const Result& r, const std::function<juce::String (juce::int64)>& formatTime)
    {
        const auto n = (int) r.channels.size();
        juce::String text ("Statistics\t" + formatTime (r.start) + " to " + formatTime (r.end) + "\n\n");

        for (int ch = 0; ch < n; ++ch)
            text << "\t" << channelName (ch, n);

        text << "\n";

        for (const auto& row : rows (r, formatTime))
        {
            text << row.name;

            for (const auto& v : row.values)
                text << "\t" << v;

            text << "\n";
        }

        return text;
    }

    //==============================================================================
    class Panel::Worker final : public juce::Thread
    {
    public:
        Worker (Source s) : juce::Thread ("Statistics"), source (std::move (s)) {}
        ~Worker() override   { stopThread (10000); }

        void run() override
        {
            result = measure (*source.audio, source.start, source.end, source.sampleRate, [this] (float p)
            {
                progress.store (p);
                return ! threadShouldExit();
            });
        }

        Source source;
        std::optional<Result> result;
        std::atomic<float> progress { 0.0f };
    };

    Panel::Panel()
    {
        for (auto* b : { &scanButton, &copyButton })
            addAndMakeVisible (*b);

        addAndMakeVisible (status);
        status.setFont (juce::FontOptions (Theme::smallText));
        status.setMinimumHorizontalScale (0.8f);
        status.setText ("Scan measures the selection, or the whole file without one.", juce::dontSendNotification);
        Theme::follow (status, juce::Label::textColourId, Theme::dimText);

        scanButton.setTooltip ("Measure the selection, or the whole file without one (a clip in a session)");
        copyButton.setTooltip ("Copy the table as text");
        scanButton.onClick = [this] { scan(); };
        copyButton.onClick = [this] { copy(); };
        copyButton.setEnabled (false);
        startTimerHz (10);
    }

    Panel::~Panel()
    {
        stopTimer();
        worker.reset();
    }

    bool Panel::isScanning() const noexcept
    {
        return worker != nullptr && worker->isThreadRunning();
    }

    void Panel::scan()
    {
        if (isScanning())
        {
            worker.reset();   // stops it
            status.setText ("Stopped.", juce::dontSendNotification);
            scanButton.setButtonText ("Scan");
            return;
        }

        const auto source = readSource != nullptr ? readSource() : std::nullopt;

        if (! source.has_value() || source->audio == nullptr || source->end <= source->start)
        {
            status.setText ("Open a file and select what to measure, or select a clip in a session.", juce::dontSendNotification);
            return;
        }

        progress = 0.0f;
        worker = std::make_unique<Worker> (*source);
        worker->startThread (juce::Thread::Priority::normal);
        scanButton.setButtonText ("Stop");
        status.setText ("Measuring...", juce::dontSendNotification);
    }

    void Panel::timerCallback()
    {
        if (worker != nullptr)
        {
            if (worker->isThreadRunning())
            {
                progress = worker->progress.load();
                status.setText ("Measuring... " + juce::String (juce::roundToInt (progress * 100.0f)) + "%", juce::dontSendNotification);
                repaint();
                return;
            }

            auto done = std::move (worker);
            scanned = done->source;
            finished (std::move (done->result));
            return;
        }

        // Say when what is shown is no longer what is selected.
        if (scanned.has_value() && result.has_value() && readSource != nullptr)
        {
            const auto now = readSource();
            const auto changed = ! now.has_value() || ! (*now == *scanned);
            const auto range = timeText (result->start) + " to " + timeText (result->end);
            status.setText (changed ? "Measured " + range + "; the audio or the selection has changed since. Scan again for the new one."
                                    : "Measured " + range + ".", juce::dontSendNotification);
        }
    }

    void Panel::finished (std::optional<Result> r)
    {
        scanButton.setButtonText ("Scan");
        result = std::move (r);
        shown = result.has_value() ? rows (*result, [this] (juce::int64 f) { return timeText (f); }) : std::vector<Row>();
        copyButton.setEnabled (result.has_value());
        status.setText (result.has_value() ? "Measured " + timeText (result->start) + " to " + timeText (result->end) + "."
                                           : "Stopped.", juce::dontSendNotification);
        repaint();
    }

    void Panel::copy()
    {
        if (result.has_value())
            juce::SystemClipboard::copyTextToClipboard (toText (*result, [this] (juce::int64 f) { return timeText (f); }));
    }

    juce::String Panel::timeText (juce::int64 frame) const
    {
        if (formatTime != nullptr)
            return formatTime (frame);

        const auto rate = result.has_value() ? result->sampleRate : 0.0;

        if (rate <= 0.0)
            return juce::String (frame);

        const auto total = (juce::int64) std::llround ((double) frame / rate * 1000.0);
        return juce::String::formatted ("%d:%02d.%03d", (int) (total / 60000), (int) ((total / 1000) % 60), (int) (total % 1000));
    }

    int Panel::rowHeight() const
    {
        return 18;
    }

    juce::Rectangle<int> Panel::tableArea() const
    {
        auto area = getLocalBounds().reduced (Theme::gap);
        area.removeFromTop (24 + Theme::gap);   // the buttons and status
        return area;
    }

    void Panel::resized()
    {
        auto top = getLocalBounds().reduced (Theme::gap).removeFromTop (24);
        scanButton.setBounds (top.removeFromLeft (64));
        top.removeFromLeft (Theme::gap);
        copyButton.setBounds (top.removeFromLeft (64));
        top.removeFromLeft (Theme::gap);
        status.setBounds (top);
    }

    void Panel::paint (juce::Graphics& g)
    {
        g.fillAll (Theme::base);
        auto area = tableArea();

        if (isScanning())
        {
            // A progress bar where the table will be.
            auto bar = area.removeFromTop (6).toFloat();
            g.setColour (Theme::inset);
            g.fillRoundedRectangle (bar, 3.0f);
            g.setColour (Theme::accent);
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.0f, 1.0f, progress)), 3.0f);
            return;
        }

        if (! result.has_value() || shown.empty())
            return;

        const auto n = (int) result->channels.size();
        const auto rowH = rowHeight();
        const auto nameWidth = juce::jmin (170, area.getWidth() / 2);
        const auto valueWidth = n > 0 ? juce::jmax (40, (area.getWidth() - nameWidth) / n) : 0;

        // The table takes what it needs (a heading and the rows); the histogram has the rest when
        // there is room for one.
        const auto tableHeight = rowH * ((int) shown.size() + 1);
        auto table = area.removeFromTop (juce::jmin (tableHeight, area.getHeight()));
        g.setFont (juce::FontOptions (Theme::bodyText));

        // Heading: the channel names.
        auto heading = table.removeFromTop (rowH);
        g.setColour (Theme::textStrong);
        heading.removeFromLeft (nameWidth);

        for (int ch = 0; ch < n; ++ch)
            g.drawFittedText (channelName (ch, n), heading.removeFromLeft (valueWidth).reduced (4, 0), juce::Justification::centredRight, 1);

        g.setColour (Theme::line);
        g.fillRect (table.getX(), table.getY(), table.getWidth(), 1);

        for (size_t i = 0; i < shown.size(); ++i)
        {
            if (table.getHeight() < rowH)
                break;

            auto row = table.removeFromTop (rowH);
            const auto& r = shown[i];

            if (i % 2 == 1)
            {
                g.setColour (Theme::laneAlternate);
                g.fillRect (row);
            }

            g.setColour (Theme::dimText);
            g.drawFittedText (r.name, row.removeFromLeft (nameWidth).reduced (4, 0), juce::Justification::centredLeft, 1);
            g.setColour (Theme::text);

            if (r.values.size() == 1 && n > 1)
                g.drawFittedText (r.values[0], row.reduced (4, 0), juce::Justification::centredLeft, 1);   // one value for every channel
            else
                for (const auto& v : r.values)
                    g.drawFittedText (v, row.removeFromLeft (valueWidth).reduced (4, 0), juce::Justification::centredRight, 1);
        }

        // The histogram: windows by their RMS, a column a dB from -100 to 0, each channel its own
        // outline over a shared fill.
        area.removeFromTop (Theme::gap);

        if (area.getHeight() < 40 || n == 0)
            return;

        auto plot = area.toFloat();
        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (Theme::smallText));
        auto labels = plot.removeFromBottom (14.0f);
        g.drawText ("-100 dB", labels.removeFromLeft (60.0f), juce::Justification::centredLeft);
        g.drawText ("0 dB", labels.removeFromRight (60.0f), juce::Justification::centredRight);
        g.drawText ("Windows by RMS (" + juce::String (juce::roundToInt (windowSeconds * 1000.0)) + " ms)", labels, juce::Justification::centred);

        g.setColour (Theme::inset);
        g.fillRoundedRectangle (plot, Theme::radius);

        juce::int64 tallest = 1;

        for (const auto& c : result->channels)
            for (auto h : c.histogram)
                tallest = juce::jmax (tallest, h);

        const auto columnWidth = plot.getWidth() / (float) histogramBins;

        for (int ch = 0; ch < n; ++ch)
        {
            const auto& c = result->channels[(size_t) ch];
            juce::Path outline;
            outline.startNewSubPath (plot.getX(), plot.getBottom());

            for (int b = 0; b < histogramBins; ++b)
            {
                const auto h = (float) c.histogram[(size_t) b] / (float) tallest * (plot.getHeight() - 4.0f);
                const auto x = plot.getX() + columnWidth * (float) b;
                outline.lineTo (x, plot.getBottom() - h);
                outline.lineTo (x + columnWidth, plot.getBottom() - h);
            }

            outline.lineTo (plot.getRight(), plot.getBottom());
            outline.closeSubPath();
            const auto colour = ch == 0 ? Theme::accent : ch == 1 ? Theme::accentLight : Theme::text.withAlpha (0.7f);
            g.setColour (colour.withAlpha (0.25f));
            g.fillPath (outline);
            g.setColour (colour);
            g.strokePath (outline, juce::PathStrokeType (1.0f));
        }
    }
}
