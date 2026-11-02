#pragma once

#include "ParametricEq.h"

/** SpectraBlade's own effects, offered beside scanned plug-ins. They live in a plug-in format of
    their own ("SpectraBlade"), so racks, track chains, sessions and offline renders create,
    save and restore them by their PluginDescription exactly as they do VST3 and Audio Units. They
    need no scan: their descriptions are added to the plug-in list directly. */
namespace BuiltInEffects
{
    constexpr const char* formatName = "SpectraBlade";
    constexpr const char* parametricEqId = "spectrablade:parametric-eq";

    /** The format to add to the app's AudioPluginFormatManager. */
    std::unique_ptr<juce::AudioPluginFormat> createFormat();

    /** Every built-in effect's description. */
    juce::Array<juce::PluginDescription> getDescriptions();

    /** Puts every built-in effect into the list (again, if it was already there). */
    void addTo (juce::KnownPluginList&);

    /** The parametric EQ, live: five bands as Effects > Parametric EQ has them, each on, type,
        frequency, gain and Q an automatable parameter, run block by block with the Equalizer
        filters. A band switched off, flat (a bell or shelf at 0 dB) or at or past Nyquist at the
        current rate passes the audio unchanged and starts afresh when it comes back. Played over
        a whole file from its start, it gives exactly the samples Equalizer::apply gives for the
        same bands. No latency. Mono or stereo, the same on every channel. */
    class ParametricEqProcessor final : public juce::AudioPluginInstance
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

        void fillInPluginDescription (juce::PluginDescription&) const override;
        const juce::String getName() const override                  { return "Parametric EQ"; }
        void prepareToPlay (double sampleRate, int maximumBlockSize) override;
        void releaseResources() override                              {}
        void reset() override;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
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
        ParametricEq::Settings shown;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParametricEqEditor)
    };
}
