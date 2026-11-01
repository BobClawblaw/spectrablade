#pragma once

#include "AudioEngine.h"
#include "Spectrogram.h"

/** The editor view: time ruler, marker lane, one lane per channel, selection, cursor and playhead.
    Drag to select, shift-click to extend, wheel or pinch to zoom, horizontal scroll to pan.
    Each channel shows its waveform, its spectrogram, or both. Option-drag in a spectrogram selects
    a box of time and frequency for spectral edits.
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

    enum class Display { waveform, spectrogram, both };
    void setDisplay (Display);
    Display getDisplay() const                          { return display; }

    /** The frequency range (Hz) of a box selected in the spectrogram; empty when the selection
        covers all frequencies. */
    juce::Range<double> getSpectralBand() const         { return band; }

    /** Empty when there is no selection. */
    juce::Range<juce::int64> getSelection() const       { return selection; }
    juce::int64 getCursor() const                       { return cursor; }
    juce::Range<double> getVisibleFrames() const        { return { viewStart, viewStart + framesPerPixel * juce::jmax (1, getWidth()) }; }
    void setSelection (juce::Range<juce::int64>);
    void setCursor (juce::int64 frame);
    void selectAll();

    /** Moves the cursor to a frame and scrolls it into view (a tenth of the way in) when it is
        out of view, keeping the zoom. */
    void showFrame (juce::int64 frame);

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

    /** A channel's waveform and spectrogram areas (either may be empty), relative to the wave area. */
    struct Lane { juce::Rectangle<int> wave, spectrum; };
    Lane getLane (int channel, int numChannels) const;
    void drawFrequencyScale (juce::Graphics&, juce::Rectangle<int> spectrum, double sampleRate);
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
    Display display = Display::waveform;
    juce::Range<double> band;
    double bandAnchor = 0.0;
    int bandLane = -1;               // channel whose spectrogram an Option-drag started in
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
