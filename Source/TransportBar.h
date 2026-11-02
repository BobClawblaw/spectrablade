#pragma once

#include <JuceHeader.h>

/** The main window's controls along the top: transport buttons, the time, zoom buttons; and the
    panel along the bottom that shows the selection's and the view's start, end and duration. */
namespace TransportBar
{
    /** The commands the buttons run, as the main window numbers them. */
    struct Commands
    {
        int goToStart = 0, playPause = 0, stop = 0, record = 0, loop = 0, goToEnd = 0;
        int zoomIn = 0, zoomOut = 0, zoomSelection = 0, zoomFit = 0;
    };

    /** A button's tooltip: the command's name and its keys as they are set now, e.g. "Loop Playback (⌘L)". */
    juce::String tooltipFor (juce::ApplicationCommandManager&, int commandId);

    /** The shapes drawn on the buttons, in a 1 x 1 square. */
    namespace Icons
    {
        juce::Path goToStart();
        juce::Path goToEnd();
        juce::Path play();
        juce::Path pause();
        juce::Path stop();
        juce::Path record();
        juce::Path loop();
        juce::Path zoomIn();
        juce::Path zoomOut();
        juce::Path zoomSelection();
        juce::Path zoomFit();
    }

    /** A square button with an icon; lit in its own colour while its toggle state is on. */
    class IconButton final : public juce::Button
    {
    public:
        IconButton (const juce::String& name, juce::Path icon, juce::Colour onColour);

        void setIcon (juce::Path);
        juce::Colour onColour;          // the fill while on
        bool tintedWhenOff = false;     // the icon drawn in onColour while off too (Play, Record)

    protected:
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;

    private:
        juce::Path icon;
    };

    /** The row of transport and zoom buttons with the time between them. A click runs the button's
        command through the command manager, so it does exactly what the menu item and keys do. */
    class Bar final : public juce::Component,
                      private juce::ChangeListener
    {
    public:
        Bar (juce::ApplicationCommandManager&, Commands);
        ~Bar() override;

        /** Shows the state: Play or Pause, recording, looping, and the time ("REC" while recording). */
        void update (bool playing, bool recording, bool looping, const juce::String& time);

        /** The tooltips again, after a key change. */
        void refreshTooltips();

        IconButton& getButton (int commandId);
        juce::String getTimeText() const                     { return timeText; }

        int getIdealWidth() const;
        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void changeListenerCallback (juce::ChangeBroadcaster*) override  { refreshTooltips(); }

        juce::ApplicationCommandManager& commands;
        Commands ids;
        juce::OwnedArray<IconButton> transport, zoom;
        juce::String timeText;
        bool showingRecording = false;
        juce::Rectangle<int> timeArea;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Bar)
    };

    /** Selection and View, each with its start, end and duration. A click on the panel calls onClick. */
    class SelectionPanel final : public juce::Component,
                                 public juce::SettableTooltipClient
    {
    public:
        SelectionPanel();

        /** Empty ranges show dashes. */
        void set (juce::Range<juce::int64> selection, juce::Range<juce::int64> view, double sampleRate);

        /** The text in a cell: row 0 is Selection, 1 View; column 0 Start, 1 End, 2 Duration. */
        juce::String getCell (int row, int column) const     { return cells[(size_t) row][(size_t) column]; }

        std::function<void()> onClick;

        void paint (juce::Graphics&) override;
        void mouseUp (const juce::MouseEvent&) override;

        static juce::String format (juce::int64 frames, double sampleRate);

    private:
        std::array<std::array<juce::String, 3>, 2> cells;
    };
}
