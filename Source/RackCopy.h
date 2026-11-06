#pragma once

#include <JuceHeader.h>

/** Another instance of an effects-rack plug-in, for two more of the master's channels (a 5.1
    master's or file's C and LFE, or Ls and Rs, played to their own outputs): it plays as the rack
    slot does, with the slot's parameters (set to the same values before each block), bypassed when
    the slot is, and with the copy's latency (the graph lines the outputs up). It is given the
    slot's whole state when it is made. A mono input gets the average of the two sides, and a mono
    output plays on both. */
class RackCopy final : public juce::AudioProcessor
{
public:
    RackCopy (juce::AudioProcessorGraph::Node::Ptr slot, std::unique_ptr<juce::AudioPluginInstance> copy);

    /** The rack slot this copies. */
    juce::AudioProcessor* getOriginal() const noexcept   { return slot->getProcessor(); }

    const juce::String getName() const override           { return copy->getName() + " (copy)"; }
    void prepareToPlay (double sampleRate, int maximumBlockSize) override;
    void releaseResources() override                      { copy->releaseResources(); }
    void reset() override                                 { copy->reset(); }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    double getTailLengthSeconds() const override          { return copy->getTailLengthSeconds(); }
    bool acceptsMidi() const override                     { return false; }
    bool producesMidi() const override                    { return false; }
    juce::AudioProcessorEditor* createEditor() override   { return nullptr; }
    bool hasEditor() const override                       { return false; }
    int getNumPrograms() override                         { return 1; }
    int getCurrentProgram() override                      { return 0; }
    void setCurrentProgram (int) override                 {}
    const juce::String getProgramName (int) override      { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override   {}

private:
    juce::AudioProcessorGraph::Node::Ptr slot;   // kept alive while the copy plays
    std::unique_ptr<juce::AudioPluginInstance> copy;
    juce::AudioBuffer<float> work;
    juce::MidiBuffer midi;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RackCopy)
};
