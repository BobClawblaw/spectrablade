#pragma once

#include "AudioDocument.h"
#include "Loudness.h"

/** The numbers of a stretch of audio: for each channel its peak and where it is, its true peak,
    the samples at full scale, the DC offset, the RMS of the whole and of its loudest and quietest
    windows, the spread between those, the bits the samples need and a histogram of the windows'
    RMS; and the loudness of all channels together as Loudness measures it. */
namespace Statistics
{
    constexpr double windowSeconds = 0.05;    // RMS windows
    constexpr int histogramBins = 100;        // one a dB, -100 dB (bin 0) to 0 dB (bin 99)

    struct Channel
    {
        double peakDb = -HUGE_VAL;            // the largest absolute sample, dBFS
        juce::int64 peakAt = 0;               // the frame it is at (the first, when several)
        float minimum = 0.0f, maximum = 0.0f; // the lowest and highest sample values
        double truePeakDb = -HUGE_VAL;        // dBTP, after 4x oversampling
        juce::int64 fullScaleSamples = 0;     // samples at or beyond 32767/32768 either way
        double dcOffset = 0.0;                // the mean sample value, as a fraction of full scale
        double rmsDb = -HUGE_VAL;             // of every frame together
        double maxRmsDb = -HUGE_VAL;          // the loudest window
        double minRmsDb = -HUGE_VAL;          // the quietest window with any sound in it
        int bitDepth = 0;                     // 8, 16 or 24 when every sample fits that many bits; 32 for floats
        std::array<juce::int64, histogramBins> histogram {};   // windows by their RMS, a bin a dB; silent windows in bin 0
        juce::int64 windows = 0;              // windows counted

        /** The spread between the loudest and quietest windows, 0 with fewer than two with sound. */
        double dynamicRange() const noexcept;
    };

    struct Result
    {
        std::vector<Channel> channels;
        Loudness::Result loudness;
        juce::int64 start = 0, end = 0;
        double sampleRate = 0.0;
    };

    /** Measures frames [start, end) of the snapshot. progress gets 0..1 and returns false to cancel. */
    std::optional<Result> measure (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate,
                                   const std::function<bool (float)>& progress = {});

    /** For a buffer (tests). */
    Result measure (const juce::AudioBuffer<float>&, double sampleRate);

    /** The name of a channel for a count of them: Left and Right for two, Mono for one, the 5.1 and
        7.1 names for six and eight, "Channel 3" otherwise. */
    juce::String channelName (int channel, int numChannels);

    /** "-6.02 dB", "-inf dB". */
    juce::String formatDb (double value, const juce::String& unit = "dB");

    /** The rows of the table the panel shows: a name and a value for each channel. */
    struct Row
    {
        juce::String name;
        std::vector<juce::String> values;
    };

    /** The channel rows, then the loudness rows (one value each). formatTime turns the peak's frame
        into text. */
    std::vector<Row> rows (const Result&, const std::function<juce::String (juce::int64)>& formatTime);

    /** The rows as tab-separated text with a heading line, for the clipboard. */
    juce::String toText (const Result&, const std::function<juce::String (juce::int64)>& formatTime);

    /** The panel: Scan measures what readSource gives (the selection or the whole file, or a clip
        in a session) in the background, with its progress shown; the table of rows follows, a
        histogram of the windows' RMS under it, and Copy puts the table on the clipboard. */
    class Panel final : public juce::Component,
                        private juce::Timer
    {
    public:
        struct Source
        {
            AudioDocument::Snapshot audio;
            juce::int64 start = 0, end = 0;
            double sampleRate = 0.0;

            bool operator== (const Source& o) const noexcept   { return audio == o.audio && start == o.start && end == o.end && juce::exactlyEqual (sampleRate, o.sampleRate); }
        };

        std::function<std::optional<Source>()> readSource;
        std::function<juce::String (juce::int64)> formatTime;   // frames to text; the panel's own form without one

        Panel();
        ~Panel() override;

        void scan();
        bool isScanning() const noexcept;
        const std::optional<Result>& getResult() const noexcept { return result; }
        juce::String getStatus() const                 { return status.getText(); }

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        class Worker;

        void timerCallback() override;
        void finished (std::optional<Result>);
        void copy();
        juce::String timeText (juce::int64 frame) const;
        juce::Rectangle<int> tableArea() const;
        int rowHeight() const;

        juce::TextButton scanButton { "Scan" }, copyButton { "Copy" };
        juce::Label status;
        std::unique_ptr<Worker> worker;
        std::optional<Result> result;
        std::optional<Source> scanned;    // what the result is of
        std::vector<Row> shown;
        float progress = 0.0f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
    };
}
