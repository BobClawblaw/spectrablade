#include "RackCopy.h"
#include "TrackEffects.h"

RackCopy::RackCopy (juce::AudioProcessorGraph::Node::Ptr s, std::unique_ptr<juce::AudioPluginInstance> c)
    : AudioProcessor (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
      slot (std::move (s)), copy (std::move (c))
{
    if (auto* original = dynamic_cast<juce::AudioPluginInstance*> (slot->getProcessor()))
    {
        juce::MemoryBlock state;
        original->getStateInformation (state);
        copy->setStateInformation (state.getData(), (int) state.getSize());
    }

    // The slot's until the copy is prepared and says its own.
    setLatencySamples (slot->getProcessor()->getLatencySamples());
}

void RackCopy::prepareToPlay (double sampleRate, int maximumBlockSize)
{
    auto& p = *copy;
    p.enableAllBuses();

    if (p.getBusCount (true) > 0)
        p.setChannelLayoutOfBus (true, 0, juce::AudioChannelSet::stereo());
    if (p.getBusCount (false) > 0)
        p.setChannelLayoutOfBus (false, 0, juce::AudioChannelSet::stereo());

    p.setRateAndBufferSizeDetails (sampleRate, maximumBlockSize);
    p.prepareToPlay (sampleRate, maximumBlockSize);
    work.setSize (juce::jmax (2, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()), maximumBlockSize);
    setLatencySamples (p.getLatencySamples());
}

void RackCopy::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    auto& p = *copy;
    const auto n = buffer.getNumSamples();

    if (auto* original = dynamic_cast<juce::AudioPluginInstance*> (slot->getProcessor()))
        TrackEffects::followParameters (*original, p);

    if (n > work.getNumSamples())
        work.setSize (work.getNumChannels(), n, false, false, true);

    juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), work.getNumChannels(), n);
    view.clear();
    const auto ins = juce::jmin (2, juce::jmax (1, p.getBusCount (true) > 0 ? p.getChannelCountOfBus (true, 0) : 0));
    view.copyFrom (0, 0, buffer, 0, 0, n);

    if (ins == 1)
    {
        view.addFrom (0, 0, buffer, 1, 0, n);
        view.applyGain (0, 0, n, 0.5f);
    }
    else
    {
        view.copyFrom (1, 0, buffer, 1, 0, n);
    }

    midi.clear();

    if (slot->isBypassed())
        p.processBlockBypassed (view, midi);
    else
        p.processBlock (view, midi);

    const auto outs = p.getTotalNumOutputChannels();
    buffer.copyFrom (0, 0, view, 0, 0, n);
    buffer.copyFrom (1, 0, view, outs >= 2 ? 1 : 0, 0, n);
}
