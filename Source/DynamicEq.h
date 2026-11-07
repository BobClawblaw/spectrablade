#pragma once

#include "AudioEdits.h"
#include "EqDrawing.h"

/** A dynamic EQ: five bands, each a bell or a shelf that moves only while its own part of the sound
    is over (or under) a threshold, by as much as that part is past it (at the ratio), up to the
    band's gain. A band set to cut -6 dB at 3 kHz over -30 dB leaves the sound alone until the 3 kHz
    part gets loud, then takes it down, and lets go again as it falls.

    Each band is the sound plus its own part scaled: out = in + (factor - 1) x part, where the part
    is a band-pass around a bell's frequency (unity at the centre, the width set by its Q) or a
    first-order low- or high-pass at a shelf's corner. So a band moving 0 dB passes the sound bit for
    bit; a bell's width stays the same however far it moves; a shelf is 6 dB an octave. The level a
    band reacts to is its part's: mean-square over a few cycles of its frequency, the loudest channel
    leading (so a stereo image stays put), a sine reading its peak in dBFS. A shelf's part hears the
    far side of its corner faintly (6 dB an octave down), so a loud bass can nudge a high shelf's
    level a little. The bands run in order, each on what the ones before it put out. */
namespace DynamicEq
{
    constexpr int numBands = 5;
    constexpr double rangeDb = 24.0;

    enum class Shape { bell, lowShelf, highShelf };

    /** Over: the band acts while its part is louder than the threshold (to tame what gets loud).
        Under: while it is quieter (to lift what gets lost). */
    enum class Mode { over, under };

    struct Band
    {
        bool on = true;
        Shape shape = Shape::bell;
        double frequency = 1000.0;     // Hz: a bell's centre, a shelf's corner
        double q = 1.0;                // a bell's width (0.1 to 20); shelves don't use it
        double gainDb = 0.0;           // the most it moves, -24 to +24 dB: below 0 cuts, above 0 boosts
        double thresholdDb = -30.0;    // -80 to 0 dB
        double ratio = 4.0;            // 1 to 20: the gain moves (1 - 1/ratio) dB for each dB past the threshold
        double attackMs = 10.0;        // 0.1 to 500 ms: the time to move 63% of the way as it starts acting
        double releaseMs = 150.0;      // 5 to 5,000 ms: and as it lets go
        Mode mode = Mode::over;
    };

    struct Settings
    {
        std::array<Band, numBands> bands;
        double outputDb = 0.0;         // -24 to +24 dB, after the bands
    };

    /** A low shelf at 100 Hz, bells at 400 Hz, 1 kHz and 3 kHz and a high shelf at 8 kHz, all on, each
        moving 0 dB: nothing changes until a band is given a gain. */
    Settings defaultSettings();

    /** Whether the settings can be used at this rate: every band's frequency from 20 Hz to 20 kHz and
        below 0.45 of the rate, and every value within the ranges above. */
    bool isValid (const Settings&, double sampleRate);

    /** The settings with each frequency brought under 0.45 of the rate, as the live effect plays them. */
    Settings withinRate (Settings, double sampleRate);

    /** The gain a band settles at for a steady level of its part, in dB (0 when not acting). */
    double staticGainDb (const Band&, double levelDb);

    /** The band's response at a frequency while it moves gainDb, in dB. */
    double responseDb (const Band&, double sampleRate, double frequency, double gainDb);

    /** The bands a frame at a time across the channels: apply runs through it, and so does the live
        effect, so they give the same samples. */
    class Core
    {
    public:
        void prepare (int numChannels, double sampleRate);
        void reset() noexcept;
        void set (const Settings&);   // valid at the prepared rate; the gains carry on

        void process (float* const* channels, int numChannels, int numFrames) noexcept;

        /** Each band's gain at the last frame (0 for a band not acting), and its part's level then. */
        const std::array<double, numBands>& getGainDb() const noexcept    { return gainDb; }
        std::array<double, numBands> getLevelDb() const noexcept;

    private:
        struct Coefficients { double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
        struct State { double z1 = 0, z2 = 0; };

        double rate = 48000.0;
        Settings settings;
        std::array<bool, numBands> active {};
        std::array<Coefficients, numBands> filters {};
        std::array<double, numBands> attack {}, release {}, detector {}, meanSquare {}, gainDb {};
        std::vector<std::array<State, numBands>> states;   // a channel each
        std::vector<double> parts;
        double output = 1.0;
    };

    /** Runs the dynamic EQ over [start, end), primed on up to a second of the audio before (kept as it
        was), so the range starts as it would in a longer pass. Nullptr for settings that aren't valid
        or an empty range. */
    AudioEdits::Snapshot apply (const AudioSnapshot&, juce::int64 start, juce::int64 end, double sampleRate, const Settings&);

    //==============================================================================
    /** The settings over a drawing. Each band switched on has a numbered handle at its frequency and
        its gain (the most it moves): drag it to set both, turn the wheel over it for a bell's Q,
        double-click it to switch the band off (a ring on the 0 dB line; double-click that to switch it
        on), or double-click anywhere else to put the first band switched off there. Each band's most
        is drawn as a thin line in its colour, and what it is doing now shaded under it; the bands
        together glow (now) over a faint line (at most).

        Under the drawing, one band's settings at a time: the band picked by its number (or its handle),
        on or off, bell or shelf, over or under, and its frequency, Q, gain, threshold, ratio, attack
        and release; and the output. Beside the drawing, the picked band's level as it plays, with its
        threshold across it: drag the threshold there to set it by eye.

        With readOutput set, the spectrum of what is playing is drawn behind; with readLive set, the
        bands' gains and levels as they play. */
    class Panel final : public juce::Component,
                        private juce::Timer
    {
    public:
        Panel (const Settings&, double sampleRate);
        ~Panel() override;

        const Settings& getSettings() const noexcept      { return settings; }
        void setSettings (const Settings&);               // without onChange
        std::function<void()> onChange;

        struct Readings
        {
            std::array<double, numBands> gainDb {};
            std::array<double, numBands> levelDb { -150.0, -150.0, -150.0, -150.0, -150.0 };
        };

        std::function<double (juce::AudioBuffer<float>&)> readOutput;
        std::function<Readings()> readLive;
        void setReadings (const Readings&);
        const Readings& getReadings() const noexcept      { return readings; }

        int getSelectedBand() const noexcept              { return selected; }
        void selectBand (int);

        // The drawing's scales.
        juce::Rectangle<float> getCurveArea() const;
        juce::Rectangle<float> getMeterArea() const;
        float xForFrequency (double) const;
        double frequencyForX (float) const;
        float yForDb (double) const;
        double dbForY (float) const;
        float yForLevel (double levelDb) const;           // on the meter, -80 to 0 dB
        double levelForY (float) const;
        juce::Point<float> pointFor (int band) const;     // its handle
        int bandAt (juce::Point<float>) const;            // a handle within grabRadius, -1 for none
        int offBandAt (juce::Point<float>) const;         // a ring within grabRadius, -1 for none
        static constexpr float grabRadius = 10.0f;

        struct Controls
        {
            std::array<juce::TextButton, numBands> tabs;
            juce::ToggleButton on { "On" };
            juce::ComboBox shape, mode;
            juce::Slider frequency, q, gain, threshold, ratio, attack, release, output;
        };

        Controls& getControls() noexcept                  { return controls; }

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    private:
        void timerCallback() override;
        void showSelected();       // the controls from the picked band
        void readControls();       // the picked band from the controls
        void changed();
        juce::Path responsePath (int band, bool now) const;   // a band (or -1, all) at most or now
        juce::String describe (int band) const;

        Settings settings;
        double sampleRate;
        Readings readings;
        Controls controls;
        juce::OwnedArray<juce::Label> labels;
        int selected = 0, hovered = -1, dragging = -1;
        bool draggingThreshold = false, showing = false;
        EqDrawing::HeldSpectrum spectrum;
        juce::AudioBuffer<float> recent;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Panel)
    };
}
