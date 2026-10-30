#pragma once

#include "AudioEngine.h"

/** The effect rack: one row per plug-in slot, processed top to bottom. */
class RackView final : public juce::Component,
                       private juce::ChangeListener
{
public:
    explicit RackView (AudioEngine& engine);
    ~RackView() override;

    std::function<void (int slotIndex)> onEditSlot;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class SlotRow;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void rebuildRows();
    void showAddMenu();

    AudioEngine& engine;
    juce::Label title { {}, "Effects Rack" };
    juce::TextButton addButton { "+ Add Effect" };
    juce::OwnedArray<SlotRow> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RackView)
};
