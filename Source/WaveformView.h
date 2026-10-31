#pragma once

#include "AudioEngine.h"

/** The editor view: time ruler, marker lane, one lane per channel, selection, cursor and playhead.
    Drag to select, shift-click to extend, wheel or pinch to zoom, horizontal scroll to pan.
    In the marker lane, click a marker to jump to it (a region selects its range), drag to move it,
    double-click to rename, right-click for more. Dropping a file opens it. */
class WaveformView final : public juce::Component,
                           public juce::FileDragAndDropTarget,
                           private juce::ChangeListener,
                           private juce::ScrollBar::Listener,
                           private juce::Timer
{
public:
    explicit WaveformView (AudioEngine& engine);
    ~WaveformView() override;

    std::function<void (const juce::File&)> onFileDropped;
    std::function<void()> onSelectionChanged;

    /** Empty when there is no selection. */
    juce::Range<juce::int64> getSelection() const       { return selection; }
    juce::int64 getCursor() const                       { return cursor; }
    void setSelection (juce::Range<juce::int64>);
    void setCursor (juce::int64 frame);
    void selectAll();

    void zoomIn();
    void zoomOut();
    void zoomToFit();
    void zoomToSelection();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void timerCallback() override;

    juce::Rectangle<int> getWaveArea() const;
    juce::Rectangle<int> getMarkerLane() const;
    const Markers& shownMarkers() const;
    int markerAt (juce::Point<float>) const;
    void drawMarkers (juce::Graphics&);
    void renameMarker (int index);
    void showMarkerMenu (int index);
    double frameToX (juce::int64 frame) const;
    juce::int64 xToFrame (float x) const;
    void setView (double startFrame, double framesPerPixel);
    void zoomAround (float x, double factor);
    void invalidateWaveform();
    void renderWaveform();
    void drawRuler (juce::Graphics&, juce::Rectangle<int>);

    AudioEngine& engine;
    juce::ScrollBar scrollBar { false };

    double viewStart = 0.0;          // first visible frame
    double framesPerPixel = 1.0;
    juce::Range<juce::int64> selection;
    juce::int64 cursor = 0;
    juce::int64 dragAnchor = 0;
    juce::uint32 lastVersion = 0;
    int lastResetCount = 0;

    // A marker being dragged: the document's markers with that one moved, shown until mouse-up.
    int draggedMarker = -1;
    juce::int64 dragGrabOffset = 0;
    Markers dragMarkers;
    bool markerWasMoved = false;

    juce::Image waveformImage;
    bool waveformValid = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};
