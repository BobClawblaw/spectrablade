#pragma once

#include <JuceHeader.h>

namespace TestPlugins
{
/** Delays its input by a known latency, which it reports, and scales it. */
class DelayPlugin final : public juce::AudioPluginInstance
{
public:
    DelayPlugin (int latencyFrames, float g)
        : AudioPluginInstance (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true)
                                                .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
          latency (latencyFrames), gain (g)
    {
        setLatencySamples (latency);
    }

    void fillInPluginDescription (juce::PluginDescription& d) const override
    {
        d.name = "Test Delay";
        d.pluginFormatName = "Test";
        d.fileOrIdentifier = "test-delay";
        d.numInputChannels = d.numOutputChannels = 2;
    }

    const juce::String getName() const override  { return "Test Delay"; }

    void prepareToPlay (double, int) override
    {
        line.setSize (2, juce::jmax (1, latency));
        line.clear();
        write = 0;
        ++prepared;
    }

    void releaseResources() override             {}
    void reset() override                        { line.clear(); write = 0; }

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        maxSeen = juce::jmax (maxSeen, b.getNumSamples());

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                const auto in = b.getSample (ch, i);

                if (latency == 0)
                {
                    b.setSample (ch, i, in * gain);
                    continue;
                }

                b.setSample (ch, i, line.getSample (ch, write) * gain);
                line.setSample (ch, write, in);
            }

            if (latency > 0)
                write = (write + 1) % latency;
        }
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override            { return false; }
    bool producesMidi() const override           { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override              { return false; }
    int getNumPrograms() override                { return 1; }
    int getCurrentProgram() override             { return 0; }
    void setCurrentProgram (int) override        {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& m) override { m.append (&gain, sizeof (gain)); }
    void setStateInformation (const void* data, int size) override
    {
        if (size == (int) sizeof (gain))
            std::memcpy (&gain, data, sizeof (gain));
    }

    const int latency;
    float gain;
    int prepared = 0, maxSeen = 0;

private:
    juce::AudioBuffer<float> line;
    int write = 0;
};

/** Scales its input by twice its one parameter ("gain", 0 to 1), read at the start of each block. */
class GainPlugin final : public juce::AudioPluginInstance
{
public:
    struct Gain final : public juce::HostedAudioProcessorParameter
    {
        float value = 0.5f;
        float getValue() const override                         { return value; }
        void setValue (float v) override
        {
            value = v;
            ++changes;

            if (echo)   // as some plug-ins do, report every change, the host's own included
                sendValueChangedMessageToListeners (v);
        }

        float getDefaultValue() const override                  { return 0.5f; }
        juce::String getName (int) const override               { return "Gain"; }
        juce::String getLabel() const override                  { return {}; }
        float getValueForText (const juce::String& t) const override { return t.getFloatValue(); }
        juce::String getParameterID() const override            { return "gain"; }
        int changes = 0;
        bool echo = false;
    };

    GainPlugin()
        : AudioPluginInstance (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true)
                                                .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
    {
        auto p = std::make_unique<Gain>();
        gain = p.get();
        addHostedParameter (std::move (p));
    }

    void fillInPluginDescription (juce::PluginDescription& d) const override
    {
        d.name = "Test Gain";
        d.pluginFormatName = "Test";
        d.fileOrIdentifier = "test-gain";
        d.numInputChannels = d.numOutputChannels = 2;
    }

    const juce::String getName() const override  { return "Test Gain"; }
    void prepareToPlay (double, int) override    {}
    void releaseResources() override             {}

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        const auto g = 2.0f * gain->value;

        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (ch, i, b.getSample (ch, i) * g);
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override            { return false; }
    bool producesMidi() const override           { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override              { return false; }
    int getNumPrograms() override                { return 1; }
    int getCurrentProgram() override             { return 0; }
    void setCurrentProgram (int) override        {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    Gain* gain = nullptr;
};
}
