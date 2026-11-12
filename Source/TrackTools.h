#pragma once

#include <JuceHeader.h>
#include "MultitrackView.h"
#include "AudioEngine.h"

/** The toolbar under the multitrack view. On the left, the tools, the one in hand lit: Select,
    Draw (drag along an instrument track's lane to make a note clip), Split (click a clip to cut
    it there), Stretch (drag a clip's edge), Glue (click a note clip to join it with the next),
    Range (drag across the lanes to select a time range) and Mute (click a clip to silence it or
    let it be heard again); then what the tool in hand does, in a line. On the right, the session's tempo (a click opens
    Tempo...) and the Bars switch, lit while the ruler counts bars and beats. Each button runs
    its command, so it does what the menu item and keys do. */
class TrackTools final : public juce::Component,
                         private juce::Timer
{
public:
    struct Commands
    {
        int select = 0, draw = 0, split = 0, stretch = 0, glue = 0, range = 0, mute = 0;
        int tempo = 0, barsAndBeats = 0;
    };

    TrackTools (juce::ApplicationCommandManager&, MultitrackView&, AudioEngine&, Commands);
    ~TrackTools() override;

    /** Runs a button's command; by default through the command manager. */
    std::function<void (int commandId)> run;

    juce::Button& getToolButton (MultitrackView::Tool);
    juce::TextButton& getTempoButton()                  { return tempoButton; }
    juce::TextButton& getBarsButton()                   { return barsButton; }

    /** What the tool in hand does, as shown beside the tools. */
    static juce::String hintFor (MultitrackView::Tool);

    /** The tempo as shown, e.g. "120 bpm, 4/4" or "98.5 bpm, 3/4". */
    static juce::String tempoText (double tempo, int beatsPerBar);

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int height = 24;

private:
    class ToolButton;

    void timerCallback() override;
    void update();

    juce::ApplicationCommandManager& commands;
    MultitrackView& view;
    AudioEngine& engine;
    Commands ids;
    juce::OwnedArray<ToolButton> tools;
    juce::TextButton tempoButton, barsButton { "Bars" };
    MultitrackView::Tool shownTool = MultitrackView::Tool::select;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackTools)
};
