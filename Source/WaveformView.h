#pragma once

#include "AudioEngine.h"
#include "OpenFiles.h"
#include "FileFades.h"
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

    std::function<void (const juce::Array<juce::File>&)> onFilesDropped;   // every audio file dropped, in order
    std::function<void()> onSelectionChanged;

    enum class Display { waveform, spectrogram, both };
    void setDisplay (Display);
    Display getDisplay() const                          { return display; }

    /** In the Both display, the share of each channel's height the waveform takes, the spectrogram
        having the rest: 0.1 to 0.9, 0.35 to start. Dragging the line between them changes it. */
    void setWaveformShare (double);
    double getWaveformShare() const                     { return waveShare; }
    std::function<void (double)> onWaveformShareChanged;   // when a drag of the line ends

    /** Where the channels are drawn, under the ruler and the markers. */
    juce::Rectangle<int> getWaveArea() const;

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

    /** Where the view is in the file (selection, cursor, zoom, spectral band), to be put back
        with setPlace when the file is shown again. */
    FilePlace getPlace() const;

    /** The fade handles at the top corners of the waveform: drag one sideways to set its fade's
        length and up or down to bend its curve; double-click it to take the fade away. */
    juce::Rectangle<float> getFadeHandle (bool out) const;
    FileFades::Fade getFade (bool out) const          { return fades.get (engine.document, out); }

    /** Puts a kept place back in the file now open, in place of fitting it to the window as a new
        file is; a place with no zoom fits it. */
    void setPlace (const FilePlace&);

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

    /** With no audio open, the view lists these files (the recent ones), each opened with a click. */
    std::function<juce::Array<juce::File>()> getRecentFiles;   // those known to be there, newest first
    std::function<void (const juce::File&)> onRecentFileClicked;

    /** Where the empty view drew each recent file, for clicks (and the tests). */
    const std::vector<std::pair<juce::Rectangle<int>, juce::File>>& getRecentLinks() const noexcept  { return recentLinks; }

private:
    void drawEmpty (juce::Graphics&, juce::Rectangle<int> area);
    std::vector<std::pair<juce::Rectangle<int>, juce::File>> recentLinks;
    int hoveredRecent = -1;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void timerCallback() override;

    juce::Rectangle<int> getMarkerLane() const;

    /** A channel's waveform and spectrogram areas (either may be empty), relative to the wave area. */
    struct Lane { juce::Rectangle<int> wave, spectrum; };
    Lane getLane (int channel, int numChannels) const;
    int dividerAt (juce::Point<float>) const;   // the channel whose waveform/spectrogram line is there, or -1
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
    double waveShare = 0.35;
    int dividerChannel = -1;         // channel whose waveform/spectrogram line is being dragged
    juce::uint32 lastVersion = 0;
    int lastResetCount = 0;

    // A fade handle being dragged (0 the fade in, 1 the fade out), and the fade waiting to be put in.
    FileFades::Draft fades;
    int fadeDragged = -1;
    FileFades::Fade fadeAtDragStart, pendingFade;
    bool fadePending = false;
    double lastFadeApplied = 0.0;
    int fadeHandleAt (juce::Point<float>) const;
    void dragFade (const juce::MouseEvent&);
    void applyPendingFade();
    void drawFades (juce::Graphics&, juce::Rectangle<int> area);

    /** The fade as drawn: the one under the mouse while its handle is dragged, ahead of the document. */
    FileFades::Fade shownFade (bool out) const
    {
        return fadeDragged == (out ? 1 : 0) && fadePending ? pendingFade : getFade (out);
    }

    // A marker being dragged: the document's markers with that one moved, shown until mouse-up.
    int draggedMarker = -1;
    juce::int64 dragGrabOffset = 0;
    Markers dragMarkers;
    bool markerWasMoved = false;

    juce::Image waveformImage;
    bool waveformValid = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};
