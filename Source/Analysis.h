#pragma once

#include <JuceHeader.h>

/** What the analysis panels compute from the output: its spectrum, and how the two channels
    relate (their correlation, and the points a goniometer draws). */
namespace Analysis
{
    constexpr int fftOrder = 13;
    constexpr int fftSize = 1 << fftOrder;   // 8,192 frames: 5.9 Hz between bins at 48 kHz

    /** The spectrum of the first fftSize samples (fewer are padded with silence), Hann-windowed:
        each bin's level in dBFS, a full-scale sine reading 0 dB at its bin. fftSize / 2 + 1 bins,
        bin i at i * sampleRate / fftSize Hz. Levels below -150 dB read -150. */
    std::vector<float> spectrumDb (const float* samples, int numSamples);

    /** The level in dB a spectrum shows at a frequency: the highest bin within a sixth of an octave
        of it, since one point of a display often stands for several bins. */
    float levelAt (const std::vector<float>& spectrum, double frequency, double sampleRate);

    /** The correlation of two channels, from +1 (the same, as mono) through 0 (unrelated) to -1
        (one the other upside down); 0 when either is silent. */
    float correlation (const float* left, const float* right, int numSamples);

    /** A frame as a goniometer draws it: the mid, (L + R) / 2, up and the side, (L - R) / 2, across
        (left to the left), so a full-scale mono signal reaches 1 up and none across, and one channel
        alone draws a diagonal. */
    juce::Point<float> goniometerPoint (float left, float right);

    /** The newest output kept for the analysis panels: the audio thread writes each block, the
        message thread copies the newest frames. Stereo; a mono block is kept in both channels. */
    class OutputTap
    {
    public:
        static constexpr int size = 16384;   // frames kept, a power of two

        /** Keeps a block (audio thread). */
        void write (const juce::AudioBuffer<float>&);

        /** The newest frames, as many as dest holds (up to size), oldest first; frames never
            written are silence. For display: a block written while this copies may be half new. */
        void copyRecent (juce::AudioBuffer<float>& dest) const;

    private:
        float frames[2][size] {};
        std::atomic<juce::int64> written { 0 };
    };

    /** The Frequency Analysis panel: the spectrum of the last fftSize frames that went to the
        outputs (both channels mixed), on a log frequency scale from 20 Hz to 20 kHz and a dB
        scale from -90 to 0, falling back slowly, with the frequency and level under the mouse. */
    class SpectrumPanel final : public juce::Component,
                                private juce::Timer
    {
    public:
        /** Fills the buffer (2 channels) with the newest output; returns the sample rate, 0 if none. */
        std::function<double (juce::AudioBuffer<float>&)> readOutput;

        SpectrumPanel();

        /** Takes in a spectrum, as each tick does: a rise shows at once, a fall at 0.6 dB a tick. */
        void take (const std::vector<float>& spectrum, double sampleRate);
        const std::vector<float>& getShown() const      { return shown; }

        /** The x of a frequency across the plot (20 Hz at 0, 20 kHz at width), and back. */
        static float xFor (double frequency, float width);
        static double frequencyAt (float x, float width);

        void paint (juce::Graphics&) override;
        void mouseMove (const juce::MouseEvent& e) override   { hover = e.position; repaint(); }
        void mouseExit (const juce::MouseEvent&) override     { hover.reset(); repaint(); }

        static constexpr float floorDb = -90.0f;

    private:
        void timerCallback() override;
        juce::Rectangle<float> plotArea() const;

        juce::AudioBuffer<float> recent { 2, fftSize };
        std::vector<float> shown;
        double rate = 0.0;
        std::optional<juce::Point<float>> hover;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumPanel)
    };

    /** The Phase Meter panel: a goniometer of the last 2,048 frames that went to the outputs, and
        under it their correlation on a bar from -1 to +1 with its value. */
    class PhasePanel final : public juce::Component,
                             private juce::Timer
    {
    public:
        std::function<double (juce::AudioBuffer<float>&)> readOutput;

        PhasePanel();

        /** Takes in a block of output, as each tick does; the correlation shown moves a quarter of
            the way to the block's each time. */
        void take (const juce::AudioBuffer<float>&);
        float getCorrelation() const                    { return shownCorrelation; }

        void paint (juce::Graphics&) override;

        static constexpr int frames = 2048;

    private:
        void timerCallback() override;

        juce::AudioBuffer<float> recent { 2, frames };
        std::vector<juce::Point<float>> points;
        float shownCorrelation = 0.0f;
        bool first = true;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PhasePanel)
    };
}
