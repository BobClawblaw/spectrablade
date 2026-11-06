#pragma once

#include <JuceHeader.h>
#include "WaveformView.h"

/** The strip under the file editor. On the left, the times: the cursor, the selection (start, end
    and length) and the view (start and end); a click on the cursor's runs Go to Time, on the
    selection's Set Selection. Fields that don't fit are left out from the right. On the right, the
    zoom controls, horizontal (out, in, the whole file) and vertical (out, in, back to full scale)
    with the vertical zoom shown between them. Each button runs its command, so it does what the
    menu item and keys do. */
class EditorBar final : public juce::Component,
                        public juce::SettableTooltipClient,
                        private juce::Timer,
                        private juce::ChangeListener
{
public:
    struct Commands
    {
        int zoomOut = 0, zoomIn = 0, zoomFit = 0;
        int verticalOut = 0, verticalIn = 0, verticalReset = 0;
        int goToTime = 0, setSelection = 0;
    };

    EditorBar (juce::ApplicationCommandManager&, WaveformView&, const AudioDocument&, Commands);
    ~EditorBar() override;

    /** Runs a button's command; by default through the command manager. */
    std::function<void (int commandId)> run;

    juce::TextButton& getButton (int commandId);

    /** The vertical zoom as shown, e.g. "1x" or "8x". */
    static juce::String verticalText (double zoom);
    juce::String getShownVertical() const              { return shownVertical; }

    /** A time field as shown: its name, its text, and where it is (empty when it doesn't fit). */
    struct Field
    {
        juce::String name, text;
        int command = 0;                 // run when it is clicked, or 0
        juce::Rectangle<int> bounds;
    };

    const std::vector<Field>& getFields() const        { return fields; }

    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;

    /** The tooltips again, after a key change. */
    void refreshTooltips();

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int height = 24;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override   { refreshTooltips(); }
    void update();
    void layOutFields();
    juce::Rectangle<int> verticalReadout() const;

    juce::ApplicationCommandManager& commands;
    WaveformView& view;
    const AudioDocument& document;
    Commands ids;
    juce::OwnedArray<juce::TextButton> horizontal, vertical;
    juce::String shownVertical;
    std::vector<Field> fields;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditorBar)
};
