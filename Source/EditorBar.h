#pragma once

#include <JuceHeader.h>
#include "WaveformView.h"

/** The strip under the file editor: the zoom controls, horizontal (out, in, the whole file) and
    vertical (out, in, back to full scale) with the vertical zoom shown between them. Each button
    runs its command, so it does what the menu item and keys do. */
class EditorBar final : public juce::Component,
                        private juce::Timer,
                        private juce::ChangeListener
{
public:
    struct Commands
    {
        int zoomOut = 0, zoomIn = 0, zoomFit = 0;
        int verticalOut = 0, verticalIn = 0, verticalReset = 0;
    };

    EditorBar (juce::ApplicationCommandManager&, WaveformView&, Commands);
    ~EditorBar() override;

    /** Runs a button's command; by default through the command manager. */
    std::function<void (int commandId)> run;

    juce::TextButton& getButton (int commandId);

    /** The vertical zoom as shown, e.g. "1x" or "8x". */
    static juce::String verticalText (double zoom);
    juce::String getShownVertical() const              { return shownVertical; }

    /** The tooltips again, after a key change. */
    void refreshTooltips();

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int height = 24;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override   { refreshTooltips(); }
    void update();
    juce::Rectangle<int> verticalReadout() const;

    juce::ApplicationCommandManager& commands;
    WaveformView& view;
    Commands ids;
    juce::OwnedArray<juce::TextButton> horizontal, vertical;
    juce::String shownVertical;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditorBar)
};
