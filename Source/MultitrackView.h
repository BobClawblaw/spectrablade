#pragma once

#include "AudioEngine.h"

/** The multitrack session: a ruler, a header per track or bus (name, effects, mute, solo, gain,
    pan, and on tracks their sends) and the tracks' clips with their waveforms. Drag a clip to
    move it (onto another track, or below the last for a new one); drag its edges to trim it, and
    the handles at its top corners to set its fades; double-click it to open its file in the
    editor. Click to place the cursor, drag across the timeline to select a time range; drop audio
    files to add them as clips. A track's R button arms it for recording.

    A track's A button shows its volume or pan envelope, or one for a parameter of its effects,
    over its lane: click the line to add a point, drag points to move them, Option-click one to
    delete it.

    With snapping on, whatever is dragged lines up with clip edges, the cursor, the time range
    and the ruler's marks when it comes within a few pixels of one. With crossfading on, a clip
    moved or trimmed so that it partly overlaps another on its track crossfades with it. */
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

    bool isSnapping() const noexcept              { return snapping; }
    void setSnapping (bool on)                    { snapping = on; }
    bool isCrossfading() const noexcept           { return crossfading; }
    void setCrossfading (bool on)                 { crossfading = on; }

    /** With automation writing on, a track's volume or pan control stays live while playing,
        even when automated: dragging it records a pass into its envelope (startAutomationPass to
        finishAutomationPass), heard as it is written and undone as one step. So does turning a
        control in a track effect's own window, from when it is grabbed to when it is let go. */
    bool isWritingAutomation() const noexcept     { return writingAutomation; }
    void setWritingAutomation (bool);

    /** A pass of automation: the control's value at play positions as it moves. Each call puts
        the pass so far into the session; when the pass ends, the envelope goes back to what it
        had over returnSeconds. A position before the last ends the pass (playback jumped). */
    static constexpr double returnSeconds = 0.1;
    void startAutomationPass (juce::uint32 trackId, const AutomationTarget&, juce::int64 frame, float value);
    void continueAutomationPass (juce::int64 frame, float value);
    void finishAutomationPass();

    /** A track effect's parameter touched in its window, with the play position when the
        session is playing. Gestures start and finish passes; values continue them. */
    void effectParameterTouched (const TrackEffects::ParameterTouch&, std::optional<juce::int64> playFrame);
    bool isAutomationPassUnderWay (juce::uint32 trackId, const AutomationTarget& p) const noexcept
    {
        return pass.has_value() && pass->trackId == trackId && pass->parameter == p;
    }

    /** The envelope shown over a track's lane, if any: volume, pan or an effect parameter. */
    std::optional<AutomationTarget> getShownEnvelope (juce::uint32 trackId) const;
    void showEnvelope (juce::uint32 trackId, std::optional<AutomationTarget>);

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

    enum class Drag { none, range, move, trimStart, trimEnd, fadeIn, fadeOut, envelopePoint };

    /** A point of a shown envelope, or a place on its line. */
    struct EnvelopeHit
    {
        int track = -1;
        AutomationTarget parameter;
        int point = -1;                    // -1: on the line between points
    };

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

    /** The fade handle under p, on the clip it belongs to: Drag::fadeIn, Drag::fadeOut or Drag::none. */
    Drag fadeHandleAt (juce::Point<int> p, int track, const SessionClip&) const;
    juce::Rectangle<float> fadeHandle (int track, const SessionClip&, bool fadeOut) const;

    std::optional<EnvelopeHit> envelopeAt (juce::Point<int>) const;
    float yForValue (int track, const AutomationTarget&, float value) const;
    float valueForY (int track, const AutomationTarget&, float y) const;
    void drawEnvelope (juce::Graphics&, int track, const AutomationTarget&) const;

    /** The effect parameter an envelope targets, or null when its effect has left the chain. */
    juce::AudioProcessorParameter* findEffectParameter (juce::uint32 trackId, const AutomationTarget&) const;

    /** Where an envelope's line is at a frame: its value, or with no points, the value now in
        charge (the track's setting, or the effect parameter's current value). */
    float lineValueAt (const SessionTrack&, const AutomationTarget&, juce::int64 frame) const;

    /** The ruler's mark spacing in seconds at the current zoom. */
    double rulerTick() const;

    /** A dragged frame, snapped when snapping is on. The cursor and time range count unless
        they are what is being dragged. */
    juce::int64 snapped (juce::int64 frame, juce::uint32 ignoreClip = 0, bool toCursorAndRange = true) const;
    SessionSnap::Targets snapTargets (juce::uint32 ignoreClip, bool toCursorAndRange) const;
    juce::int64 snapTolerance() const;

    void drawRuler (juce::Graphics&, juce::Rectangle<int>) const;
    void drawClip (juce::Graphics&, int track, const SessionClip&) const;
    void selectTrack (int track);
    void showEffectsMenu (juce::uint32 trackId, juce::Component& target);
    void showSends (juce::uint32 trackId, juce::Component& target);
    void showAutomationMenu (juce::uint32 trackId, juce::Component& target);
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
    EnvelopeHit dragEnvelope;              // the point being dragged
    Envelope dragEnvelopeBase;             // its envelope, with the point, as the drag began
    juce::String dragDescription;          // the undo step a drag makes

    bool snapping = true, crossfading = true, writingAutomation = false;

    struct AutomationPass
    {
        juce::uint32 trackId = 0;
        AutomationTarget parameter;
        SessionState base;                 // the session as the pass began
        std::vector<AutomationPoint> points;
    };

    std::optional<AutomationPass> pass;
    bool controlsFollowPlay = false;
    void applyAutomationPass (bool first);

    /** Where automated controls show their envelope's value: the play position while the session
        plays, otherwise the cursor. */
    juce::int64 controlFrame() const;
    std::map<juce::uint32, AutomationTarget> shownEnvelopes;

    juce::Component headerArea;
    std::vector<std::unique_ptr<TrackHeader>> headers;
    juce::ScrollBar hScroll { false }, vScroll { true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultitrackView)
};
