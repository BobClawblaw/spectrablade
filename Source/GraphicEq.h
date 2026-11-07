#pragma once

#include "Equalizer.h"

/** The graphic equalizer: 10 bands an octave apart or 31 a third of an octave apart, at the
    standard centre frequencies, each a bell (Equalizer's peak band) as wide as the spacing, with up
    to 12 dB of boost or cut. Bands at 0 dB are left out, as are bands at or past half the rate. */
namespace GraphicEq
{
    constexpr double maxGainDb = 12.0;

    /** The centre frequencies for 10 or 31 bands. */
    const std::vector<double>& centres (int numBands);

    /** Each band's Q: as wide as the spacing (1.41 an octave, 4.32 a third of one). */
    double bandQ (int numBands);

    /** Whether there are 10 or 31 gains, each within maxGainDb. */
    bool isValid (const std::vector<double>& gainsDb);

    /** The bands that change anything at this rate, low to high. */
    std::vector<Equalizer::Band> bandsFor (const std::vector<double>& gainsDb, double sampleRate);

    /** Runs the bands over [start, end) as Equalizer::apply does. Nullptr for gains that aren't
        valid, all at 0 dB, or an empty range. */
    AudioEdits::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const std::vector<double>& gainsDb);

    /** The bands drawn as bars from the 0 dB line, each in a colour running from warm (low) to
        cool (high), with the response they make together drawn over them and each band's frequency
        under it. Pressing sets the band under the mouse to the gain there; dragging across sets
        every band passed over, along the line the mouse drew, so a shape can be sketched in one
        stroke. Double-clicking a band sets it back to 0 dB; the wheel over one moves it 0.5 dB a
        step (0.1 with Shift). The band under the mouse says its frequency and gain. Flat sets them
        all to 0 dB. onChange is called after each change made here. */
    class Panel final : public juce::Component
    {
    public:
        explicit Panel (int numBands);

        std::vector<double> getGains() const;
        void setGains (const std::vector<double>&);   // without onChange

        std::function<void()> onChange;

        /** The drawing's scales: the bars' area, each band's column centre, and gain up it. */
        juce::Rectangle<float> getBarArea() const;
        float xForBand (int band) const;
        int bandAtX (float x) const;                  // the nearest band's column
        float yForGain (double db) const;
        double gainForY (float y) const;              // to 0.1 dB, within maxGainDb

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    private:
        void setBand (int band, double db);
        void changed();
        float xForFrequency (double hz) const;        // between the column centres, on a log scale

        std::vector<double> gains;
        int hovered = -1;
        juce::Point<float> last;                      // where the drag was at its last step
        juce::TextButton flat { "Flat" };
    };
}
