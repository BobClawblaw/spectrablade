#pragma once

#include "Equalizer.h"
#include "Analysis.h"
#include "EqDrawing.h"

/** Five equalizer bands at once, each with an on switch, a type, a frequency, a gain and a Q,
    over a drawing of the response they make together (20 Hz to 20 kHz across, -24 to +24 dB up).

    Each band has a colour of its own: its own response is shaded in it, and its row of controls is
    marked with it. Each band switched on has a numbered handle on the curve at its frequency: drag
    it to set the frequency and the gain (so the curve passes under the mouse there), turn the wheel
    over it for the Q, and double-click it to switch the band off. A band switched off shows as a
    ring on the 0 dB line: double-click it to switch it back on. Double-clicking anywhere else puts
    the first band switched off there. The handle under the mouse, or the one last dragged, says its
    type, frequency, gain and Q beside it.

    With readOutput set, the spectrum of what is playing is drawn behind the curve (-90 to 0 dBFS
    up the drawing), falling back slowly. */
class ParametricEq final : public juce::Component,
                           private juce::Timer
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

    /** Shows these settings, as from outside (automation, a restored state): the controls move
        without reporting a change. */
    void setSettings (const Settings&);

    /** The bands that change anything, in order: those switched on, leaving out bells and shelves at
        0 dB (passes always count). */
    std::vector<Equalizer::Band> getActiveBands() const;

    /** The response of the active bands together at a frequency, in dB. */
    double responseDb (double frequency) const;

    /** Called after every change to a band. */
    std::function<void()> onChange;

    /** Fills the buffer (2 channels, Analysis::fftSize frames) with the newest output; returns the
        sample rate, 0 if none. Unset: no spectrum. */
    std::function<double (juce::AudioBuffer<float>&)> readOutput;

    /** The spectrum drawn behind the curve (dB by bin, as Analysis::spectrumDb), as from readOutput. */
    void takeSpectrum (const std::vector<float>& spectrumDb, double spectrumRate);
    const std::vector<float>& getSpectrum() const noexcept   { return spectrum.levels; }

    /** Each band's colour, the same in every theme. */
    static juce::Colour bandColour (int index);

    /** The band picked out (its handle ringed, its row marked): the one last pressed; -1 for none. */
    int getSelectedBand() const noexcept             { return selected; }

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

    /** The band switched off whose ring (at its frequency, on the 0 dB line) is nearest, within
        grabRadius pixels; -1 for none. */
    int offBandAt (juce::Point<float>) const;
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
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void resized() override;

private:
    void readRow (int index);
    void timerCallback() override;
    juce::Path responsePath (int band) const;   // one band's response, or (-1) all of them together
    void paintReadout (juce::Graphics&, int band) const;

    Settings settings;
    double sampleRate;
    std::array<std::unique_ptr<Row>, numBands> rows;
    int dragging = -1, hovered = -1, selected = -1;
    EqDrawing::HeldSpectrum spectrum;
    juce::AudioBuffer<float> recent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParametricEq)
};
