#pragma once

#include "Dynamics.h"
#include "Modulation.h"
#include "Reverb.h"
#include "Convolver.h"
#include "EffectPresets.h"
#include "GraphicEq.h"
#include "Multiband.h"
#include "ParametricEq.h"

/** SpectraBlade's own effects, offered beside scanned plug-ins. They live in a plug-in format of
    their own ("SpectraBlade"), so racks, track chains, sessions and offline renders create,
    save and restore them by their PluginDescription exactly as they do VST3 and Audio Units. They
    need no scan: their descriptions are added to the plug-in list directly. */
namespace BuiltInEffects
{
    constexpr const char* formatName = "SpectraBlade";
    constexpr const char* parametricEqId = "spectrablade:parametric-eq";
    constexpr const char* compressorId = "spectrablade:compressor";
    constexpr const char* limiterId = "spectrablade:limiter";
    constexpr const char* chorusId = "spectrablade:chorus";
    constexpr const char* flangerId = "spectrablade:flanger";
    constexpr const char* phaserId = "spectrablade:phaser";
    constexpr const char* reverbId = "spectrablade:reverb";
    constexpr const char* convolutionId = "spectrablade:convolution-reverb";
    constexpr const char* graphicEq10Id = "spectrablade:graphic-eq-10";
    constexpr const char* graphicEq31Id = "spectrablade:graphic-eq-31";
    constexpr const char* multibandId = "spectrablade:multiband-compressor";
    constexpr const char* deEsserId = "spectrablade:de-esser";

    /** The format to add to the app's AudioPluginFormatManager. */
    std::unique_ptr<juce::AudioPluginFormat> createFormat();

    /** Every built-in effect's description. */
    juce::Array<juce::PluginDescription> getDescriptions();

    /** Puts every built-in effect into the list (again, if it was already there). */
    void addTo (juce::KnownPluginList&);

    /** Sets a float parameter to `plain` exactly when some normalised value gives it, else as near
        as one does (a skewed range can't give every float). */
    void setExactly (juce::AudioParameterFloat&, float plain);

    /** What every built-in effect shares: its description, mono or stereo in and out alike, no MIDI,
        one program, hosted parameters (so automation and sessions find each by its ID), and a
        saved state of each parameter's plain value (Hz, dB, ms, a choice's index, on or off) that
        restores every value exactly. */
    class BuiltInEffect : public juce::AudioPluginInstance
    {
    public:
        void fillInPluginDescription (juce::PluginDescription& d) const override   { d = description; }
        const juce::String getName() const override                  { return description.name; }
        void releaseResources() override                              {}
        bool isBusesLayoutSupported (const BusesLayout&) const override;
        double getTailLengthSeconds() const override                  { return 0.0; }
        bool acceptsMidi() const override                             { return false; }
        bool producesMidi() const override                            { return false; }
        juce::AudioProcessorEditor* createEditor() override;
        bool hasEditor() const override                               { return true; }
        int getNumPrograms() override                                 { return 1; }
        int getCurrentProgram() override                              { return 0; }
        void setCurrentProgram (int) override                         {}
        const juce::String getProgramName (int) override              { return {}; }
        void changeProgramName (int, const juce::String&) override    {}
        void getStateInformation (juce::MemoryBlock&) override;
        void setStateInformation (const void*, int) override;

        /** Its parameters as a preset: each one's plain value (a choice's index, 1 or 0 for on or
            off) under its ID; the presets' key is the effect's identifier. */
        EffectPresets::Values getPresetValues() const;
        juce::String getPresetKey() const                             { return description.fileOrIdentifier; }

        /** Sets the parameters a preset names (as host changes, each as near as it holds the
            value); ones it doesn't name, and names that aren't parameters, are left alone. */
        void setPresetValues (const EffectPresets::Values&);

        /** Most channels processLinked takes. */
        static constexpr int mostLinkedChannels = 8;

        /** Processes every channel of the audio in place as one, where the effect can link them
            (its gain from the loudest of them all, the same gain on each): a 5.1 track's six. False,
            with nothing done, where it can't. Same thread as processBlock. */
        virtual bool processLinked (juce::AudioBuffer<float>&)        { return false; }

        /** Whether its main input and output can be up to mostLinkedChannels wide (as many
            channels, discrete), processBlock then linking them as processLinked does: how the
            effects rack plays a 5.1 master's or file's channels through it together. */
        virtual bool linksChannels() const                            { return false; }

    protected:
        /** With sidechain, a second input bus (off until the host turns it on) for a key. */
        BuiltInEffect (const juce::PluginDescription&, const juce::String& stateTag, bool sidechain = false);

        /** What an effect keeps besides its parameters, written into and read back from its state. */
        virtual void addToState (juce::XmlElement&) const          {}
        virtual void readFromState (const juce::XmlElement&)       {}

        juce::AudioParameterFloat* addFloat (const juce::String& id, const juce::String& name, juce::NormalisableRange<float>,
                                             float start, const juce::String& label = {});
        juce::AudioParameterChoice* addChoice (const juce::String& id, const juce::String& name, const juce::StringArray&, int start);
        juce::AudioParameterBool* addBool (const juce::String& id, const juce::String& name, bool start);

    private:
        juce::PluginDescription description;
        juce::String tag;

        static BusesProperties busesFor (bool sidechain);

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BuiltInEffect)
    };

    /** The parametric EQ, live: five bands as Effects > Parametric EQ has them, each on, type,
        frequency, gain and Q an automatable parameter, run block by block with the Equalizer
        filters. A band switched off, flat (a bell or shelf at 0 dB) or at or past Nyquist at the
        current rate passes the audio unchanged and starts afresh when it comes back. Played over
        a whole file from its start, it gives exactly the samples Equalizer::apply gives for the
        same bands. No latency. Mono or stereo, the same on every channel. */
    class ParametricEqProcessor final : public BuiltInEffect
    {
    public:
        static constexpr int numBands = 5;

        ParametricEqProcessor();

        /** The bands as they are set, in order, switched on or not. */
        std::array<std::pair<bool, Equalizer::Band>, numBands> getBands() const;

        /** The bands that change anything at this rate, in order. */
        std::vector<Equalizer::Band> getActiveBands (double sampleRate) const;

        /** Sets band index's on switch and band (the gain is ignored for passes), as host changes. */
        void setBand (int index, bool on, const Equalizer::Band&);

        juce::AudioParameterBool& onParameter (int index) const      { return *bandParameters[(size_t) index].on; }
        juce::AudioParameterChoice& typeParameter (int index) const  { return *bandParameters[(size_t) index].type; }
        juce::AudioParameterFloat& frequencyParameter (int index) const { return *bandParameters[(size_t) index].frequency; }
        juce::AudioParameterFloat& gainParameter (int index) const   { return *bandParameters[(size_t) index].gain; }
        juce::AudioParameterFloat& qParameter (int index) const      { return *bandParameters[(size_t) index].q; }

        void prepareToPlay (double sampleRate, int maximumBlockSize) override;
        void reset() override;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
        juce::AudioProcessorEditor* createEditor() override;

    private:
        struct BandParameters
        {
            juce::AudioParameterBool* on = nullptr;
            juce::AudioParameterChoice* type = nullptr;
            juce::AudioParameterFloat* frequency = nullptr;
            juce::AudioParameterFloat* gain = nullptr;
            juce::AudioParameterFloat* q = nullptr;
        };

        Equalizer::Band bandFromParameters (int index) const;

        std::array<BandParameters, numBands> bandParameters;
        double rate = 48000.0;

        // Each band's filters, one a channel, and the band they were last set for (nothing when
        // the band was passing the audio through), so they are reset only when a band comes back.
        std::array<std::vector<Equalizer::Filter>, numBands> filters;
        std::array<std::optional<Equalizer::Band>, numBands> filtersSetFor;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParametricEqProcessor)
    };

    /** The Parametric EQ panel's bands as a preset, under the live EQ's parameter IDs (band1On,
        band1Type, band1Frequency, band1Gain, band1Q, ...), so the window and the live EQ share them. */
    EffectPresets::Values eqPresetValues (const ParametricEq::Settings&);

    /** The bands a preset sets, over `start` (bands or values it doesn't name stay as they are). */
    ParametricEq::Settings eqSettingsFrom (const EffectPresets::Values&, ParametricEq::Settings start);

    /** The live EQ's window: the Parametric EQ panel, its bands and curve, on the processor's
        parameters. Moving a control sets the parameters as a gesture, so a track's automation
        writing records it; changes from elsewhere (automation, a restored state) show within a
        tenth of a second. */
    class ParametricEqEditor final : public juce::AudioProcessorEditor,
                                     private juce::Timer
    {
    public:
        explicit ParametricEqEditor (ParametricEqProcessor&);
        ~ParametricEqEditor() override;

        ParametricEq& getPanel() noexcept                             { return *panel; }

        /** Shows the processor's bands now, as the timer does. */
        void refresh();

        void resized() override;

    private:
        void timerCallback() override                                 { refresh(); }
        void panelChanged();

        ParametricEqProcessor& eq;
        std::unique_ptr<ParametricEq> panel;
        EffectPresets::Bar presets;
        ParametricEq::Settings shown;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParametricEqEditor)
    };

    /** The compressor, live: Effects > Compressor's settings as parameters, over the same ranges, run a frame at a time
        with Dynamics::CompressorCore, so played over a whole file from its start it gives exactly
        the samples Dynamics::compress gives. A change of settings carries the gain on. No latency.
        With Sidechain on, the level comes from its sidechain input (a key track on a session
        track, silence elsewhere) in place of its own input. */
    class CompressorProcessor final : public BuiltInEffect
    {
    public:
        CompressorProcessor();

        /** The settings as the parameters hold them. */
        Dynamics::Compressor getSettings() const;

        /** Sets every parameter (as host changes) to the settings, as near as each holds them. */
        void setSettings (const Dynamics::Compressor&);

        void prepareToPlay (double sampleRate, int maximumBlockSize) override;
        void reset() override                                         { core.reset(); }
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

        /** Linked, from the loudest of all the channels; not with Sidechain on (every pair then
            hears the same key, so they move together anyway). */
        bool processLinked (juce::AudioBuffer<float>&) override;
        bool linksChannels() const override                           { return true; }

        juce::AudioParameterFloat* threshold = nullptr;
        juce::AudioParameterFloat* ratio = nullptr;
        juce::AudioParameterFloat* knee = nullptr;
        juce::AudioParameterFloat* attack = nullptr;
        juce::AudioParameterFloat* release = nullptr;
        juce::AudioParameterFloat* makeup = nullptr;
        juce::AudioParameterBool* sidechain = nullptr;   // the level from the sidechain input, not its own

    private:
        Dynamics::CompressorCore core;
        Dynamics::Compressor applied;
        double rate = 48000.0;

        void refreshSettings();
        void compress (juce::AudioBuffer<float>&, int channels, int firstKey, int keyChannels);

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompressorProcessor)
    };

    /** The limiter, live: gain before it, a ceiling and a release as parameters (over Effects >
        Limiter's ranges), and a look-ahead set when it is made (5 ms in a rack or on a track), which
        it reports as its latency (the host lines everything else up with it). It
        runs a frame at a time with Dynamics::LimiterCore on sample peaks, so its output, a
        look-ahead late, is exactly what Dynamics::limit gives for the same settings. No sample
        comes out above the ceiling. */
    class LimiterProcessor final : public BuiltInEffect
    {
    public:
        /** A look-ahead from 0.1 to 50 ms. */
        explicit LimiterProcessor (double lookaheadMs = 5.0);

        /** The settings as the parameters hold them (sample peaks, its look-ahead). */
        Dynamics::Limiter getSettings() const;

        /** Sets the gain, ceiling and release (as host changes); the look-ahead and true peaks are ignored. */
        void setSettings (const Dynamics::Limiter&);

        void prepareToPlay (double sampleRate, int maximumBlockSize) override;
        void reset() override;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

        /** Linked, from the peak of all the channels, so none goes above the ceiling. */
        bool processLinked (juce::AudioBuffer<float>&) override;
        bool linksChannels() const override                           { return true; }

        juce::AudioParameterFloat* gain = nullptr;
        juce::AudioParameterFloat* ceiling = nullptr;
        juce::AudioParameterFloat* release = nullptr;

    private:
        Dynamics::LimiterCore core;
        double lookaheadMs;
        double rate = 48000.0;
        juce::AudioBuffer<float> delay;   // the last look-ahead's frames, a ring
        int delayPosition = 0;

        void limit (juce::AudioBuffer<float>&, int channels);

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterProcessor)
    };

    /** Chorus or flanger, live: Effects > Chorus's or Flanger's settings as parameters over the
        same ranges, run with Modulation::DelayCore, so played over a whole file from its start it
        gives exactly the samples Modulation::sweptDelay gives. No latency. */
    class SweptDelayProcessor final : public BuiltInEffect
    {
    public:
        enum class Kind { chorus, flanger };
        explicit SweptDelayProcessor (Kind);

        Modulation::Delay getSettings() const;
        void setSettings (const Modulation::Delay&);

        void prepareToPlay (double sampleRate, int maximumBlockSize) override;
        void reset() override                                         { core.reset(); }
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

        juce::AudioParameterFloat* rate = nullptr;
        juce::AudioParameterFloat* delay = nullptr;
        juce::AudioParameterFloat* depth = nullptr;
        juce::AudioParameterFloat* feedback = nullptr;
        juce::AudioParameterFloat* mix = nullptr;
        juce::AudioParameterFloat* spread = nullptr;

    private:
        Modulation::DelayCore core;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SweptDelayProcessor)
    };

    /** The phaser, live, with Modulation::PhaserCore: exactly Modulation::phaser's samples. No latency. */
    class PhaserProcessor final : public BuiltInEffect
    {
    public:
        PhaserProcessor();

        Modulation::Phaser getSettings() const;
        void setSettings (const Modulation::Phaser&);

        void prepareToPlay (double sampleRate, int maximumBlockSize) override;
        void reset() override                                         { core.reset(); }
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

        juce::AudioParameterFloat* rate = nullptr;
        juce::AudioParameterFloat* low = nullptr;
        juce::AudioParameterFloat* high = nullptr;
        juce::AudioParameterChoice* stages = nullptr;   // 2, 4, ... 12
        juce::AudioParameterFloat* feedback = nullptr;
        juce::AudioParameterFloat* mix = nullptr;
        juce::AudioParameterFloat* spread = nullptr;

    private:
        Modulation::PhaserCore core;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PhaserProcessor)
    };

    /** The reverb, live: Effects > Reverb's settings as parameters over the same ranges, run with
        RoomReverb::Core, so over a whole file from its start it gives exactly the samples
        RoomReverb::apply gives. No latency; its tail is the pre-delay plus the decay. */
    class ReverbProcessor final : public BuiltInEffect
    {
    public:
        ReverbProcessor();

        RoomReverb::Settings getSettings() const;
        void setSettings (const RoomReverb::Settings&);

        void prepareToPlay (double sampleRate, int maximumBlockSize) override;
        void reset() override                                         { core.reset(); }
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
        double getTailLengthSeconds() const override                  { return RoomReverb::tailSeconds (getSettings()); }

        juce::AudioParameterFloat* preDelay = nullptr;
        juce::AudioParameterFloat* decay = nullptr;
        juce::AudioParameterFloat* size = nullptr;
        juce::AudioParameterFloat* damping = nullptr;
        juce::AudioParameterFloat* width = nullptr;
        juce::AudioParameterFloat* mix = nullptr;

    private:
        RoomReverb::Core core;
        RoomReverb::Settings applied;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbProcessor)
    };

    /** The convolution reverb, live: an impulse response (kept in its state, samples and all, so a
        session doesn't need the file) and the mix as a parameter, run with Convolver::Core, so its
        output, Convolver::blockSize frames late (reported as latency), is exactly what
        Convolver::apply gives. With no impulse it passes the sound through, as late. */
    class ConvolutionProcessor final : public BuiltInEffect
    {
    public:
        ConvolutionProcessor();
        ~ConvolutionProcessor() override;

        /** A usable impulse (Convolver::problemWith empty); on the message thread. */
        void setImpulse (Convolver::Impulse);
        const Convolver::Impulse& getImpulse() const noexcept         { return impulse; }

        void prepareToPlay (double sampleRate, int maximumBlockSize) override;
        void reset() override;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
        double getTailLengthSeconds() const override;
        juce::AudioProcessorEditor* createEditor() override;

        juce::AudioParameterFloat* mix = nullptr;

    private:
        void addToState (juce::XmlElement&) const override;
        void readFromState (const juce::XmlElement&) override;
        void rebuild();

        Convolver::Impulse impulse;
        double rate = 48000.0;
        juce::SpinLock coreLock;
        std::unique_ptr<Convolver::Core> core;   // swapped under the lock
        bool coreHasImpulse = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ConvolutionProcessor)
    };

    /** The convolution reverb's window: its presets (the mix), the impulse's name, a button to load
        another from an audio file, and the mix. */
    class ConvolutionEditor final : public juce::AudioProcessorEditor
    {
    public:
        explicit ConvolutionEditor (ConvolutionProcessor&);
        ~ConvolutionEditor() override;

        void resized() override;
        void paint (juce::Graphics&) override;

    private:
        void chooseImpulse();
        void showImpulse();

        ConvolutionProcessor& convolution;
        EffectPresets::Bar presets;
        juce::Label impulseName;
        juce::TextButton load { "Load Impulse..." };
        juce::Slider mix { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Label mixLabel { {}, "Mix" };
        std::unique_ptr<juce::SliderParameterAttachment> mixAttachment;
        std::unique_ptr<juce::FileChooser> chooser;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ConvolutionEditor)
    };

    /** The graphic EQ, live, with 10 or 31 bands: each band's gain a parameter (band1 ... bandN, in
        dB), run block by block with the Equalizer filters as the parametric EQ is. A band at 0 dB or
        at or past half the rate passes the sound unchanged and starts afresh when it comes back.
        Over a whole file from its start, exactly GraphicEq::apply's samples. No latency. */
    class GraphicEqProcessor final : public BuiltInEffect
    {
    public:
        explicit GraphicEqProcessor (int numBands);

        int getNumBands() const noexcept                              { return (int) gains.size(); }
        std::vector<double> getGains() const;
        void setGains (const std::vector<double>&);

        void prepareToPlay (double sampleRate, int maximumBlockSize) override;
        void reset() override;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
        juce::AudioProcessorEditor* createEditor() override;

        std::vector<juce::AudioParameterFloat*> gains;

    private:
        double rate = 48000.0;
        std::vector<std::vector<Equalizer::Filter>> filters;   // a band's, one a channel
        std::vector<std::optional<double>> filtersSetFor;      // the gain they were set for

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GraphicEqProcessor)
    };

    /** The live graphic EQ's window: its presets and a slider a band. */
    class GraphicEqEditor final : public juce::AudioProcessorEditor,
                                  private juce::Timer
    {
    public:
        explicit GraphicEqEditor (GraphicEqProcessor&);
        ~GraphicEqEditor() override;

        GraphicEq::Panel& getPanel() noexcept                         { return panel; }
        void refresh();

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void timerCallback() override                                 { refresh(); }
        void panelChanged();

        GraphicEqProcessor& eq;
        EffectPresets::Bar presets;
        GraphicEq::Panel panel;
        std::vector<double> shown;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GraphicEqEditor)
    };

    /** The multiband compressor, live: the crossovers, each band's threshold, ratio, attack, release
        and makeup, and the output as parameters (over Effects > Multiband Compressor's ranges; each
        band's knee is 6 dB). Crossovers set closer than an octave, or past what the rate allows, are
        moved apart as the offline effect would refuse them. It runs a frame at a time with
        Multiband::CompressorCore, so over a whole file from its start it gives Multiband::compress's
        samples. No latency. */
    class MultibandProcessor final : public BuiltInEffect
    {
    public:
        MultibandProcessor();

        /** The settings as played: the parameters' with the crossovers moved apart if need be. */
        Multiband::Compressor getSettings() const;

        /** The settings exactly as the parameters hold them. */
        Multiband::Compressor getParameterSettings() const;
        void setSettings (const Multiband::Compressor&);

        /** The parameters, and settings as their values, in one order: the crossovers, the output, then
            each band's threshold, ratio, attack, release and makeup. */
        std::vector<juce::AudioParameterFloat*> parameterOrder() const;
        static std::vector<float> valuesOf (const Multiband::Compressor&);

        /** Each band's reduction at the end of the last block, safe to read from the window. */
        std::array<double, 3> getReductionDb() const noexcept;

        juce::AudioProcessorEditor* createEditor() override;

        void prepareToPlay (double sampleRate, int maximumBlockSize) override;
        void reset() override;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

        static const std::array<const char*, 3> bandNames;   // "low", "mid", "high", the parameter IDs' prefixes

    private:
        juce::AudioParameterFloat* lowCrossover = nullptr;
        juce::AudioParameterFloat* highCrossover = nullptr;
        juce::AudioParameterFloat* output = nullptr;
        struct BandParameters { juce::AudioParameterFloat *threshold, *ratio, *attack, *release, *makeup; };
        std::array<BandParameters, 3> bands {};

        Multiband::CompressorCore core;
        std::array<std::atomic<double>, 3> reductionDb {};   // the core's after the last block, for the window
        std::vector<float> applied;   // the parameters' values the core was last set from
        double rate = 48000.0;
        int channelsPrepared = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultibandProcessor)
    };

    /** The live multiband compressor's window: its presets and the panel, each band's gain reduction
        shown under its name as it plays. */
    class MultibandEditor final : public juce::AudioProcessorEditor,
                                  private juce::Timer
    {
    public:
        explicit MultibandEditor (MultibandProcessor&);
        ~MultibandEditor() override;

        Multiband::Panel& getPanel() noexcept                         { return panel; }
        void refresh();

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void timerCallback() override                                 { refresh(); }
        void panelChanged();

        MultibandProcessor& effect;
        EffectPresets::Bar presets;
        Multiband::Panel panel;
        std::vector<float> shown;        // the parameters' values the panel was last set from
        std::vector<float> panelValues;  // the panel's own values then (its sliders snap, the parameters round)

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultibandEditor)
    };

    /** The de-esser, live: frequency, threshold, reduction and what to turn down as parameters (over
        Effects > De-esser's ranges), and Listen, which puts out only what it takes away. It runs a frame at a time with Multiband::DeEsserCore, so over a
        whole file from its start it gives Multiband::deEss's samples. No latency. */
    class DeEsserProcessor final : public BuiltInEffect
    {
    public:
        DeEsserProcessor();

        Multiband::DeEsser getSettings() const;
        void setSettings (const Multiband::DeEsser&);

        double getReductionDb() const noexcept                        { return reductionDb.load(); }

        /** The plain window, with the reduction shown under the parameters as it plays. */
        juce::AudioProcessorEditor* createEditor() override;

        void prepareToPlay (double sampleRate, int maximumBlockSize) override;
        void reset() override;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    private:
        std::atomic<double> reductionDb { 0.0 };   // the core's at the end of the last block, for the window
        juce::AudioParameterFloat* frequency = nullptr;
        juce::AudioParameterFloat* threshold = nullptr;
        juce::AudioParameterFloat* reduction = nullptr;
        juce::AudioParameterChoice* mode = nullptr;

    public:
        juce::AudioParameterBool* listen = nullptr;

    private:
        Multiband::DeEsserCore core;
        Multiband::DeEsser applied;
        double rate = 48000.0;
        int channelsPrepared = 0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeEsserProcessor)
    };
}
