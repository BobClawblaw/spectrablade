#pragma once

#include "AudioEngine.h"

/** The multitrack session: a ruler, a header per track or bus (name, effects, mute, solo, gain,
    pan, and on tracks their sends) and the tracks' clips with their waveforms. Drag a clip to
    move it (onto another track, or below the last for a new one); drag its edges to trim it;
    double-click it to open its file in the editor. Click to place the cursor, drag across the
    timeline to select a time range; drop audio files to add them as clips. A track's R button arms
    it for recording. */
class MultitrackView final : public juce::Component,
                             public juce::FileDragAndDropTarget,
                             private juce::ChangeListener,
                             private juce::ScrollBar::Listener,
                             private juce::Timer
{
public:
    explicit MultitrackView (AudioEngine&);
    ~MultitrackView() override;

    std::function<void (const juce::File&)> onOpenInEditor;
    std::function<void (const juce::StringArray& files, int track, juce::int64 frame)> onFilesDropped;
    std::function<void()> onSelectionChanged;

    /** Asks for a track effect's editor window. */
    std::function<void (juce::AudioPluginInstance&)> onEditPlugin;

    juce::int64 getCursor() const noexcept        { return cursor; }
    void setCursor (juce::int64 frame);

    /** The time range dragged across the timeline (empty when there is none). */
    juce::Range<juce::int64> getTimeSelection() const noexcept { return timeSelection; }
    void setTimeSelection (juce::Range<juce::int64>);

    /** The track armed for recording (its id), or 0. One track at a time. */
    juce::uint32 getArmedTrack() const noexcept   { return armedTrackId; }
    void setArmedTrack (juce::uint32 trackId);

    /** Shows the take under way: the part of the session it covers on the armed track (or a new
        track below the last), empty when nothing is recording. */
    void showRecording (juce::Range<juce::int64>);

    /** The selected track (the one last clicked), or -1. */
    int getSelectedTrack() const;
    juce::uint32 getSelectedClip() const noexcept { return selectedClip; }

    void deleteSelectedClip();
    void splitSelectedClip();      // at the cursor
    void deleteSelectedTrack();

    void zoomIn();
    void zoomOut();
    void zoomToFit();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    static constexpr int headerWidth = 220, rulerHeight = 24, trackHeight = 104;

private:
    class TrackHeader;
    class SendsPanel;

    enum class Drag { none, range, move, trimStart, trimEnd };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
    void timerCallback() override;

    const SessionState& shown() const             { return preview.has_value() ? *preview : *state; }
    void rebuildHeaders();
    void layoutHeaders();
    void updateScrollBars();
    void setView (double start, double framesPerPixel);
    void zoomAround (float x, double factor);

    juce::Rectangle<int> timelineArea() const;
    int laneTop (int track) const;
    int trackAt (int y) const;
    float xForFrame (juce::int64 frame) const;
    juce::int64 frameForX (float x) const;
    juce::Rectangle<int> clipBounds (int track, const SessionClip&) const;
    std::optional<std::pair<int, int>> clipAt (juce::Point<int>) const;

    void drawRuler (juce::Graphics&, juce::Rectangle<int>) const;
    void drawClip (juce::Graphics&, int track, const SessionClip&) const;
    void selectTrack (int track);
    void showEffectsMenu (juce::uint32 trackId, juce::Component& target);
    void showSends (juce::uint32 trackId, juce::Component& target);
    void editTrack (juce::uint32 trackId, const juce::String& description, bool merge,
                    const std::function<void (SessionTrack&)>& change);

    AudioEngine& engine;
    Session::State state;
    std::optional<SessionState> preview;   // the session as it would be after the drag under way

    double framesPerPixel = 1000.0, viewStart = 0.0;
    int scrollY = 0;
    juce::int64 cursor = 0;
    juce::uint32 selectedClip = 0, selectedTrackId = 0, armedTrackId = 0;
    juce::Range<juce::int64> timeSelection, recording;

    Drag drag = Drag::none;
    juce::int64 dragFrame = 0;             // frame under the mouse when the drag began (or the range's anchor)
    SessionClip dragClip;                  // the clip as it was
    int dragTrack = 0;

    juce::Component headerArea;
    std::vector<std::unique_ptr<TrackHeader>> headers;
    juce::ScrollBar hScroll { false }, vScroll { true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultitrackView)
};
