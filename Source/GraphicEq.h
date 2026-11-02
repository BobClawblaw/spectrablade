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

    /** A slider a band, its frequency under it, and Flat; onChange is called as one moves. */
    class Panel final : public juce::Component
    {
    public:
        explicit Panel (int numBands);

        std::vector<double> getGains() const;
        void setGains (const std::vector<double>&);   // without onChange

        std::function<void()> onChange;

        void resized() override;

    private:
        juce::OwnedArray<juce::Slider> sliders;
        juce::OwnedArray<juce::Label> labels;
        juce::TextButton flat { "Flat" };
        juce::Label top { {}, "+12 dB" }, bottom { {}, "-12 dB" };
    };
}
