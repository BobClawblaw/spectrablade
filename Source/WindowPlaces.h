#pragma once

#include <JuceHeader.h>

/** Remembers where windows were left. Each watched window's place is written to the settings as it
    moves or resizes, and put back when it opens again, kept where its title bar can be reached on
    the screens there are now (a screen it was on may have gone). */
class WindowPlaces final : private juce::ComponentListener
{
public:
    explicit WindowPlaces (juce::PropertiesFile&);
    ~WindowPlaces() override;

    /** Puts the window where the place saved under this name left it, if there is one, and saves
        its place under that name from now on. With keepSize, only the position comes back (for a
        window whose content sets its size, such as a plug-in's). Returns whether a saved place was used. */
    bool watch (juce::ResizableWindow&, const juce::String& name, bool keepSize = false);

    /** A window's bounds, moved (and shrunk to fit if need be) onto the first screen when no screen
        shows at least 100 by 20 pixels of its top edge; unchanged otherwise. Screens are their
        usable areas, the main one first. */
    static juce::Rectangle<int> reachable (juce::Rectangle<int> bounds, const juce::Array<juce::Rectangle<int>>& screens);

    static juce::String keyFor (const juce::String& name)   { return "windowPlace " + name; }

private:
    void componentMovedOrResized (juce::Component&, bool wasMoved, bool wasResized) override;
    void componentBeingDeleted (juce::Component&) override;

    juce::PropertiesFile& settings;
    std::vector<std::pair<juce::Component*, juce::String>> watched;
};

/** A bar between two panes side by side, dragged left and right to share the room between them. */
class DividerBar final : public juce::Component
{
public:
    DividerBar();

    std::function<void (int parentX)> onDrag;   // the mouse's x in the parent as the bar is dragged
    std::function<void()> onDragEnd;

    void paint (juce::Graphics&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
};
