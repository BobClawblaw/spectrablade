#pragma once

#include "AudioEngine.h"

/** The multitrack session: a ruler, a header per track or bus (name, effects, mute, solo, gain,
    pan, and on tracks their sends) and the tracks' clips with their waveforms. Drag a clip to
    move it (onto another track, or below the last for a new one); drag its edges to trim it, and
    the handles at its top corners to set its fades; double-click it to open its file in the
    editor; right-click it to choose among its takes; Command-drag it up or down to change its gain.
    Shift-click clips to select several: dragging any of them moves them all in time, and cut,
    copy, paste, duplicate and delete work on them together. Click to place the cursor, drag across the
    timeline to select a time range; drop audio files to add them as clips. A track's R button
    arms it for recording. Drag a track's header up or down (by its background) to move the track;
    right-click it to put the track in a group, whose volume, mute and solo changes go together.

    Under the ruler, the marker lane shows the session's markers and regions: click one to move
    the cursor to it (a region selects its range), drag it to move it, double-click it to rename
    it, right-click it to rename or delete it.

    A track's A button shows its volume or pan envelope, or one for a parameter of its effects,
    over its lane: click the line to add a point, drag points to move them, Option-click one to
    delete it.

    With snapping on, whatever is dragged lines up with clip edges, markers, the cursor, the time range
    and the ruler's marks (seconds, or bars and beats) when it comes within a few pixels of one. With crossfading on, a clip
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

    /** The tracks armed for recording (their ids, in the session's order). Each records from its
        own input; buses can't be armed. */
    std::vector<juce::uint32> getArmedTracks() const;
    bool isArmed (juce::uint32 trackId) const;
    void setArmed (juce::uint32 trackId, bool armed);

    /** Sets what a track records from, as one undo step. The I button on a track offers the
        device's inputs. */
    void setTrackInput (juce::uint32 trackId, TrackInput);

    /** Shows the takes under way: the part of the session they cover on the armed tracks (or a
        new track below the last), empty when nothing is recording. */
    void showRecording (juce::Range<juce::int64>);

    /** Adds a marker where the session is playing, or else a region over the time range or a
        marker at the cursor, as an undo step. */
    void addMarker();

    /** Moves the cursor to the nearest marker or region edge after it (or before it). Returns
        false when there is none. */
    bool goToMarker (bool forwards);

    /** The selected track (the one last clicked), or -1. */
    int getSelectedTrack() const;
    juce::uint32 getSelectedClip() const noexcept { return selectedClip; }

    /** Puts one of a clip's takes in use (from loop recording), as one undo step. A right-click
        on a clip with takes offers them. */
    void chooseTake (juce::uint32 clipId, int index);

    /** Every selected clip, in the session's order. getSelectedClip() is the one clicked last,
        which commands for a single clip (split, gain, takes) work on. */
    std::vector<juce::uint32> getSelectedClips() const;
    bool isClipSelected (juce::uint32 clipId) const  { return selectedClips.count (clipId) > 0; }
    void selectAllClips();         // on the shown tracks

    /** Selects every clip on the selected track; false with none selected. */
    bool selectClipsOnTrack();

    /** Selects every clip, on the shown tracks, that starts at or after the cursor. */
    void selectClipsAfterCursor();

    void deleteSelectedClip();     // every selected clip

    /** The selected clips, copied for pasting (their audio is shared, not duplicated), with
        where they are on the timeline and the tracks relative to each other. */
    bool copySelectedClip();
    bool cutSelectedClip();
    bool canPaste() const;

    /** Pastes the copied clips at the cursor, the topmost onto the selected track (or a new one)
        and the rest onto the tracks below, over what is there. Selects them and moves the cursor
        to their end, so pastes follow each other. */
    void pasteClip();

    /** Repeats the clip clicked last end to end up to the end of the time range, as one undo
        step. False when there is no such clip or the range doesn't end after it. */
    bool repeatSelectedClip();

    /** Places copies of the selected clips right after them, each on its own track, over what is
        there, and selects the copies. */
    void duplicateSelectedClip();

    void splitSelectedClip();      // at the cursor

    /** Moves the selected clips together, each on its own track, so that the earliest starts at
        the cursor, as one undo step. False when there are none, one is locked, or they are there. */
    bool moveSelectedClipsToCursor();

    /** True when there are selected clips and every one is locked. */
    bool areSelectedClipsLocked() const;

    /** Locks (or unlocks) every selected clip as one undo step ("Lock Clip", "Unlock 2 Clips").
        Locked clips show a lock and can't be dragged, trimmed, faded, nudged, split or deleted
        until they are unlocked; see SessionEdits::setClipLocked. */
    void setSelectedClipsLocked (bool locked);

    /** True when there are selected clips and every one is muted. */
    bool areSelectedClipsMuted() const;

    /** Mutes (or unmutes) every selected clip as one undo step ("Mute Clip", "Unmute 2 Clips").
        A muted clip is drawn grey and isn't heard, but can be edited as usual. */
    void setSelectedClipsMuted (bool muted);

    /** Gives every selected clip a colour from TrackColours::palette, or -1 for its track's, as
        one undo step ("Clip Colour"). Right-click a clip for its Colour menu. */
    void setSelectedClipsColour (int colour);

    /** Gives every selected clip's fade in (or out) a shape, as one undo step ("Fade In Shape",
        "Fade Out Shape"); locked clips keep theirs. Right-click a clip for its fade shape menus. */
    void setSelectedClipsFadeShape (bool fadeOut, FadeShape);

    /** Selects a clip alone (and its track), puts the cursor at its start and scrolls it into
        view, keeping the zoom. False when there is no such clip. */
    bool showClip (juce::uint32 clipId);

    /** Puts the cursor at a marker (a region also becomes the time range, as clicking it in the
        marker lane does) and scrolls it into view, keeping the zoom. False when there is none. */
    bool showMarker (int index);

    /** Asks for a marker's new name; renaming is an undo step. */
    void renameMarker (int index);

    /** Deletes a marker as an undo step ("Delete <name>"). False when there is none. */
    bool deleteMarker (int index);

    /** The track lanes' scroll position, in pixels from the top. */
    int getScrollY() const noexcept    { return scrollY; }
    void deleteSelectedTrack();

    /** Duplicates the selected track as one undo step ("Duplicate Track") and selects the copy.
        Returns the copy's id (0 with none selected); its effects are the caller's to copy. */
    juce::uint32 duplicateSelectedTrack();

    /** Closes the gaps between the selected track's clips (SessionEdits::closeGaps) as one undo
        step ("Close Gaps"). False when nothing moved. */
    bool closeGapsOnSelectedTrack();

    /** Moves each selected clip so it starts on the nearest line of the grid snapping uses (beats
        or bars in bars and beats, else the ruler's marks), as one undo step ("Quantize Clip",
        "Quantize 3 Clips"). Locked clips stay. Returns how many moved. */
    int quantizeSelectedClips();

    /** Clears the time range on every track (SessionEdits::clearTime) as one undo step ("Clear
        Range"). False with no range or nothing there. */
    bool clearTimeSelection();

    /** Takes the time range out of the session (SessionEdits::deleteTime) as one undo step
        ("Delete Time"), then puts the cursor at its start with no range. False with no range or
        nothing changed. */
    bool deleteTimeSelection();

    /** Puts as much empty time as the range is long in at its start (SessionEdits::insertTime),
        as one undo step ("Insert Time"). The range then covers the new time. False with no range
        or nothing changed. */
    bool insertTimeAtSelection();

    /** Copies the time range from every track (SessionEdits::copyTime) for pasting as time.
        False with no range. */
    bool copyTimeSelection();

    /** Copies the time range, then deletes it as Delete Time does, as one undo step ("Cut
        Time"). False with no range. */
    bool cutTimeSelection();

    /** True when copied time can be pasted: there is some, from a session at this one's rate. */
    bool canPasteTime() const;

    /** Puts the copied time in at the cursor (SessionEdits::pasteTime) as one undo step ("Paste
        Time"), selects it as the time range and moves the cursor to its end, so pastes follow
        each other. */
    bool pasteTime();

    /** Hides a track or bus from the lanes (it still plays, and stays in the mixer) or shows it
        again, as an undo step ("Hide Track", "Show Track"). Right-click a header to hide it. */
    void setTrackHidden (juce::uint32 trackId, bool hidden);

    /** Shows every hidden track again, as one undo step. False when none was hidden. */
    bool showAllTracks();

    /** Sets a track's or bus's note (empty to clear it) as an undo step ("Track Note"). Right-
        click a header and choose Note... to type one; the header shows it as its tooltip. */
    void setTrackNote (juce::uint32 trackId, const juce::String& note);

    /** The grid point after a frame (or before it): the ruler's beats in bars and beats, otherwise
        its marks in seconds, at the current zoom. Never before 0. */
    juce::int64 gridStepFrom (juce::int64 frame, bool later) const;

    /** Moves the selected clips together so that the one clicked last starts on the next grid
        point (or the one before), as an undo step that a run of nudges shares; with no clip
        selected, moves the cursor to it. */
    void nudge (bool later);

    /** Gives a track or bus a colour from TrackColours::palette, or -1 for the automatic one, as
        an undo step. Right-click a header for its Colour menu. */
    void setTrackColour (juce::uint32 trackId, int colour);

    /** Puts a track or bus in a group (1 to SessionTrack::maxGroups) or none (0), as an undo
        step. Right-click a header for its Group menu. */
    void setTrackGroup (juce::uint32 trackId, int group);

    /** Moves a track or bus to another place in the list, as an undo step; it stays selected. */
    void moveTrack (int from, int to);

    bool isSnapping() const noexcept              { return snapping; }

    /** With this on, splits and dragged clip edges move to the nearest zero crossing of the
        clip's audio, up to zeroCrossingReach seconds away, so the cut doesn't click. */
    bool isFindingZeroCrossings() const noexcept  { return zeroCrossings; }
    void setFindingZeroCrossings (bool on)        { zeroCrossings = on; }
    static constexpr double zeroCrossingReach = 0.005;
    void setSnapping (bool on)                    { snapping = on; }
    bool isCrossfading() const noexcept           { return crossfading; }

    /** Bars and beats: the ruler counts bars and beats of the session's tempo, the lanes show
        its grid, and snapping goes to its beats (or bars, zoomed out) instead of the ruler's
        seconds. */
    void setShowBars (bool show)                  { showBars = show; repaint(); }
    bool isShowingBars() const noexcept           { return showBars; }
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

    /** The height of every track's lane (and header), from minLaneHeight to maxLaneHeight;
        trackHeight to start with. In short lanes the header shows only its top row. */
    int getLaneHeight() const noexcept               { return laneHeight; }
    void setLaneHeight (int);

    static constexpr int minLaneHeight = 56, maxLaneHeight = 312, laneHeightStep = 24;

    /** Fills the timeline with a range of the session, with a small margin either side. */
    void zoomToRange (juce::Range<juce::int64>);

    /** The session frames the timeline shows, from its left edge to its right. */
    juce::Range<double> getVisibleFrames() const;

    /** Scrolls the lanes so they start at this session frame, keeping the zoom. */
    void scrollTo (double startFrame);

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

    /** How much a clip's gain changes for each pixel it is Command-dragged. */
    static constexpr float clipGainPerPixel = 0.25f;

    static constexpr int headerWidth = 220, timeRulerHeight = 24, markerLaneHeight = 16,
                         rulerHeight = timeRulerHeight + markerLaneHeight, trackHeight = 104;

private:
    class TrackHeader;
    class SendsPanel;

    enum class Drag { none, range, move, trimStart, trimEnd, fadeIn, fadeOut, envelopePoint, marker, clipGain };

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
    bool moveSelectedClips (juce::int64 delta, const juce::String& description, bool merge);
    void selectClipsWhere (const std::function<bool (const SessionTrack&, const SessionClip&)>& wanted);
    void setSelectedClipsFlag (bool (*set) (SessionState&, juce::uint32, bool), bool on, const juce::String& verb);
    void rebuildHeaders();
    void layoutHeaders();
    void updateScrollBars();
    void setView (double start, double framesPerPixel);
    void zoomAround (float x, double factor);

    juce::Rectangle<int> timelineArea() const;
    int laneTop (int track) const;
    int trackAt (int y) const;
    int rowOf (int track) const;    // its lane's place among the shown tracks
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

    /** In bars and beats: beats between ruler labels, and between marks (the snap grid). */
    std::pair<Metronome::Grid, Metronome::Grid> rulerBeats() const;
    void drawBarsRuler (juce::Graphics&, juce::Rectangle<int> area) const;
    SessionSnap::Targets snapTargets (juce::uint32 ignoreClip, bool toCursorAndRange) const;
    juce::int64 snapTolerance() const;

    void drawRuler (juce::Graphics&, juce::Rectangle<int>) const;

    /** The marker (or region) under p in the marker lane, or -1. Point markers win, then the
        shortest region, so nested regions stay reachable. */
    int markerAt (juce::Point<int>) const;
    void drawMarkers (juce::Graphics&, juce::Rectangle<int> area) const;
    void askForTrackNote (juce::uint32 trackId);
    void showMarkerMenu (int index);
    void drawClip (juce::Graphics&, int track, const SessionClip&) const;
    void selectTrack (int track);
    void selectClip (juce::uint32 clipId, bool adding);   // adding: Shift-click, which toggles it
    void showInputMenu (juce::uint32 trackId, juce::Component& target);
    void showEffectsMenu (juce::uint32 trackId, juce::Component& target);
    void showSends (juce::uint32 trackId, juce::Component& target);
    void showAutomationMenu (juce::uint32 trackId, juce::Component& target);
    void showClipMenu (juce::uint32 clipId);
    void editTrack (juce::uint32 trackId, const juce::String& description, bool merge,
                    const std::function<void (SessionTrack&)>& change);

    AudioEngine& engine;
    Session::State state;
    std::optional<SessionState> preview;   // the session as it would be after the drag under way

    double framesPerPixel = 1000.0, viewStart = 0.0;
    int laneHeight = trackHeight;
    int scrollY = 0;
    juce::int64 cursor = 0;
    juce::uint32 selectedClip = 0, selectedTrackId = 0;
    std::set<juce::uint32> selectedClips;  // selectedClip among them
    std::vector<std::pair<int, SessionClip>> copiedClips;   // with their tracks counted from the topmost
    double copiedRate = 0.0;               // the session rate the copied clips were at
    SessionTime copiedTime;
    std::set<juce::uint32> armed;
    bool showBars = false;
    juce::Range<juce::int64> timeSelection, recording;

    Drag drag = Drag::none;
    juce::int64 dragFrame = 0;             // frame under the mouse when the drag began (or the range's anchor)
    SessionClip dragClip;                  // the clip as it was
    std::vector<SessionClip> dragGroup;    // the other selected clips being moved with it, as they were
    int trackDropIndex = -1;               // where a track whose header is being dragged would go
    int dragMarker = -1;                   // the marker being dragged, and the frames from its start to the mouse
    juce::int64 dragMarkerGrab = 0;
    int dragTrack = 0;
    EnvelopeHit dragEnvelope;              // the point being dragged
    Envelope dragEnvelopeBase;             // its envelope, with the point, as the drag began
    juce::String dragDescription;          // the undo step a drag makes

    bool snapping = true, crossfading = true, writingAutomation = false, zeroCrossings = false;
    juce::int64 toZeroCrossing (const SessionClip&, juce::int64 frame) const;

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
