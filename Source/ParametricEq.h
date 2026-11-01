#pragma once

#include "Equalizer.h"

/** Five equalizer bands at once, each with an on switch, a type, a frequency, a gain and a Q,
    over a drawing of the response they make together (20 Hz to 20 kHz across, -24 to +24 dB up).
    Each band switched on has a point on the curve at its frequency: drag it to set the frequency
    and the gain (so the curve passes under the mouse there), and turn the wheel over it for the Q. */
class ParametricEq final : public juce::Component
{
public:
    static constexpr int numBands = 5;
    static constexpr double minHz = 20.0, maxHz = 20000.0, rangeDb = 24.0;

    struct BandSetting
    {
        bool on = true;
        Equalizer::Band band;
    };

    using Settings = std::array<BandSetting, numBands>;

    /** A low shelf at 100 Hz, bells at 400 Hz, 1 kHz and 3 kHz and a high shelf at 8 kHz, all at 0 dB. */
    static Settings defaultSettings();

    ParametricEq (const Settings&, double sampleRate);

    const Settings& getSettings() const noexcept     { return settings; }

    /** The bands that change anything, in order: those switched on, leaving out bells and shelves at
        0 dB (passes always count). */
    std::vector<Equalizer::Band> getActiveBands() const;

    /** The response of the active bands together at a frequency, in dB. */
    double responseDb (double frequency) const;

    /** Called after every change to a band. */
    std::function<void()> onChange;

    // The drawing's scales: frequency across on a log scale, gain up, inside the curve's area.
    juce::Rectangle<float> getCurveArea() const;
    float xForFrequency (double frequency) const;
    double frequencyForX (float x) const;
    float yForDb (double db) const;
    double dbForY (float y) const;

    /** Where band index's point is drawn: at its frequency, on the curve. */
    juce::Point<float> pointFor (int index) const;

    /** The band switched on whose point is nearest, within grabRadius pixels; -1 for none. */
    int bandAt (juce::Point<float>) const;
    static constexpr float grabRadius = 10.0f;

    struct Row
    {
        juce::ToggleButton on;
        juce::ComboBox type;
        juce::Slider frequency, gain, q;
    };

    Row& getRow (int index)                          { return *rows[(size_t) index]; }

    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void resized() override;

private:
    void readRow (int index);

    Settings settings;
    double sampleRate;
    std::array<std::unique_ptr<Row>, numBands> rows;
    int dragging = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParametricEq)
};
