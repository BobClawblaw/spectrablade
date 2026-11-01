#include <JuceHeader.h>
#include "../Source/MultitrackView.h"
#include "../Source/TrackEffects.h"
#include "TestUtils.h"
#include "MultitrackViewTests.h"

namespace
{
    using TestUtils::check;

    constexpr double rate = 48000.0;
    constexpr int timelineWidth = 1000;

    AudioDocument::Snapshot silence (juce::int64 frames)
    {
        juce::AudioBuffer<float> b (1, (int) frames);
        b.clear();
        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (b)), 0, frames } };
        return std::make_shared<const AudioSnapshot> (1, std::move (segments));
    }

    /** Drives the view with the mouse, the way the app's window would. */
    struct Mouse
    {
        MultitrackView& view;

        juce::MouseEvent event (juce::Point<float> at, juce::Point<float> down, juce::ModifierKeys mods, bool dragged) const
        {
            const auto now = juce::Time::getCurrentTime();
            return { juce::Desktop::getInstance().getMainMouseSource(), at, mods, juce::MouseInputSource::defaultPressure,
                     juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                     juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY,
                     &view, &view, now, down, now, 1, dragged };
        }

        void drag (juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods = {}) const
        {
            const auto held = mods.withFlags (juce::ModifierKeys::leftButtonModifier);
            view.mouseDown (event (from, from, held, false));

            if (from != to)
                view.mouseDrag (event (to, from, held, true));

            view.mouseUp (event (to, from, mods, from != to));
            settle();
        }

        void click (juce::Point<float> at, juce::ModifierKeys mods = {}) const   { drag (at, at, mods); }

        juce::MouseCursor cursorAt (juce::Point<float> at) const
        {
            view.mouseMove (event (at, at, {}, false));
            return view.getMouseCursor();
        }

        /** Lets the view hear about the session's changes. */
        static void settle()
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        }
    };
}

void runMultitrackViewTests (AudioEngine& engine)
{
    // Two clips on one track: A from 1 s to 3 s, B from 5 s to 6 s, in a 10 s view.
    SessionState initial;
    initial.sampleRate = rate;
    auto& track = initial.addTrack();
    SessionEdits::addClip (initial, 0, juce::File ("/tmp/a.wav"), silence (96000), 48000);
    SessionEdits::addClip (initial, 0, juce::File ("/tmp/b.wav"), silence (48000), 240000);
    const auto trackId = track.id;
    const auto clipA = initial.tracks[0].clips[0].id, clipB = initial.tracks[0].clips[1].id;
    engine.session.reset (initial, {});

    MultitrackView view (engine);
    view.setSize (MultitrackView::headerWidth + timelineWidth + 12, MultitrackView::rulerHeight + 3 * MultitrackView::trackHeight + 12);
    view.zoomToFit();
    Mouse mouse { view };
    Mouse::settle();

    // zoomToFit shows 10 s and a 5% margin across the timeline.
    const auto framesPerPixel = rate * 10.0 * 1.05 / timelineWidth;
    const auto x = [&] (juce::int64 frame) { return (float) MultitrackView::headerWidth + (float) ((double) frame / framesPerPixel); };
    const auto handleY = (float) MultitrackView::rulerHeight + 3.0f + 8.0f;     // the fade handles, in each clip's title bar
    const auto bodyY = (float) MultitrackView::rulerHeight + 60.0f;
    const auto clip = [&] (juce::uint32 id) -> const SessionClip&
    {
        const auto where = engine.session.getState()->findClip (id);
        return engine.session.getState()->tracks[(size_t) where->first].clips[(size_t) where->second];
    };

    // Drag A's fade-in handle half way along it: nothing to snap to there.
    mouse.drag ({ x (48000) + 4.0f, handleY }, { x (48000 + 48000), handleY });
    const auto fadeIn = clip (clipA).fadeIn;
    check (std::abs (fadeIn - 48000) <= (juce::int64) framesPerPixel && engine.session.getUndoDescription() == "Clip Fade In",
           "view: dragging a clip's fade-in handle sets its fade in (" + juce::String (fadeIn) + " frames, 48000 wanted)");

    // Drag A's fade-out handle to 3 px past the cursor at 2.5 s: it snaps to the cursor exactly.
    view.setCursor (120000);
    mouse.drag ({ x (144000) - 4.0f, handleY }, { x (120000) + 3.0f, handleY });
    check (clip (clipA).fadeOut == 24000 && clip (clipA).fadeIn == fadeIn,
           "view: a fade-out handle dragged near the cursor snaps to it (" + juce::String (clip (clipA).fadeOut) + " frames, 24000 wanted)");

    // Move B so that its start comes 3 px after A's end: with snapping it butts up exactly.
    const auto grab = juce::Point<float> (x (260000), bodyY);
    const auto offset = x (144000) + 3.0f - x (240000);
    mouse.drag (grab, grab.translated (offset, 0.0f));
    check (clip (clipB).position == 144000 && engine.session.getUndoDescription() == "Move Clip",
           "view: a clip moved near another's end snaps to it (start " + juce::String (clip (clipB).position) + ", 144000 wanted)");

    engine.session.undo();
    Mouse::settle();
    view.setSnapping (false);
    mouse.drag (grab, grab.translated (offset, 0.0f));
    check (clip (clipB).position != 144000 && std::abs (clip (clipB).position - 144000) < 4 * (juce::int64) framesPerPixel,
           "view: with snapping off it lands where it is dropped (start " + juce::String (clip (clipB).position) + ")");
    view.setSnapping (true);
    engine.session.undo();
    Mouse::settle();

    // Move B over A's end, snapping its start to the cursor at 2.5 s: they crossfade over the
    // 0.5 s they share. With crossfading off, the fades stay as they were.
    auto noFadeOut = *engine.session.getState();
    SessionEdits::setClipFades (noFadeOut, clipA, fadeIn, 0);
    engine.session.apply (std::move (noFadeOut), "Clip Fade Out");
    Mouse::settle();
    const auto overlapOffset = x (120000) + 3.0f - x (240000);
    mouse.drag (grab, grab.translated (overlapOffset, 0.0f));
    check (clip (clipB).position == 120000 && clip (clipB).fadeIn == 24000 && clip (clipA).fadeOut == 24000
               && clip (clipA).fadeIn == fadeIn && engine.session.getUndoDescription() == "Move Clip",
           "view: a clip moved over another's end crossfades with it (fade in " + juce::String (clip (clipB).fadeIn) + ", 24000 wanted)");

    engine.session.undo();
    Mouse::settle();
    view.setCrossfading (false);
    mouse.drag (grab, grab.translated (overlapOffset, 0.0f));
    check (clip (clipB).position == 120000 && clip (clipB).fadeIn == 0 && clip (clipA).fadeOut == 0,
           "view: with crossfading off, overlapping clips keep their fades");
    view.setCrossfading (true);

    // Envelopes: show the track's volume envelope, find its line by hovering down the lane.
    view.showEnvelope (trackId, AutomationParameter::volume);
    const auto pointX = x (400000);
    auto lineY = -1.0f;

    for (auto y = (float) MultitrackView::rulerHeight; y < (float) (MultitrackView::rulerHeight + MultitrackView::trackHeight); y += 1.0f)
    {
        if (mouse.cursorAt ({ pointX, y }) == juce::MouseCursor (juce::MouseCursor::CrosshairCursor))
        {
            lineY = y + 5.0f;   // the first row within 5 px of the line, so the line is 5 px below
            break;
        }
    }

    check (lineY > 0.0f, "view: a shown envelope's line can be found under the mouse");

    mouse.click ({ pointX, lineY });
    auto points = engine.session.getState()->tracks[0].volumeEnvelope.points;
    check (points.size() == 1 && points[0].value == 0.0f && std::abs (points[0].frame - 400000) <= (juce::int64) framesPerPixel
               && engine.session.getUndoDescription() == "Add Envelope Point",
           "view: a click on the line adds a point at the track's volume, where it was clicked");

    mouse.drag ({ pointX, lineY }, { pointX, lineY + 25.0f });
    points = engine.session.getState()->tracks[0].volumeEnvelope.points;
    check (points.size() == 1 && points[0].value < -3.0f && engine.session.getUndoDescription() == "Move Envelope Point",
           "view: dragging the point down lowers it (" + juce::String (points.empty() ? 0.0f : points[0].value, 1) + " dB)");

    const auto pointY = lineY + 25.0f;
    check (mouse.cursorAt ({ pointX, pointY }) == juce::MouseCursor (juce::MouseCursor::DraggingHandCursor),
           "view: the point shows a hand under the mouse");

    mouse.click ({ pointX, pointY }, juce::ModifierKeys::altModifier);
    check (engine.session.getState()->tracks[0].volumeEnvelope.isEmpty() && engine.session.getUndoDescription() == "Delete Envelope Point",
           "view: Option-click deletes the point");

    // Writing automation: a pass as the play position moves, with the control moved, held and
    // moved again. It is one undo step, and a value held still makes no extra points.
    view.setWritingAutomation (true);
    view.startAutomationPass (trackId, AutomationParameter::volume, 50000, 0.0f);
    view.continueAutomationPass (60000, -10.0f);
    view.continueAutomationPass (60000, -12.0f);
    view.continueAutomationPass (70000, -12.0f);
    view.continueAutomationPass (80000, -12.0f);
    view.continueAutomationPass (90000, -3.0f);
    view.finishAutomationPass();
    Mouse::settle();
    const auto volumePoints = [&]
    {
        points = engine.session.getState()->tracks[0].volumeEnvelope.points;
        return points;
    };
    const std::vector<AutomationPoint> wanted { { 50000, 0.0f }, { 50000, 0.0f }, { 60000, -12.0f }, { 80000, -12.0f }, { 90000, -3.0f }, { 94800, 0.0f } };
    check (volumePoints() == wanted && engine.session.getUndoDescription() == "Write Volume Automation",
           "view: a pass of automation is written as the play position moves (" + juce::String ((int) points.size()) + " points, 6 wanted)");

    view.startAutomationPass (trackId, AutomationParameter::volume, 120000, -6.0f);
    view.continueAutomationPass (130000, -6.0f);
    view.continueAutomationPass (125000, 6.0f);   // playback jumped back: the pass ends
    check (! view.isAutomationPassUnderWay (trackId, AutomationParameter::volume) && volumePoints().size() == 10,
           "view: a second pass adds to the envelope, and a jump back in time ends it");

    engine.session.undo();
    Mouse::settle();
    check (volumePoints() == wanted, "view: undo takes off the second pass alone");
    engine.session.undo();
    Mouse::settle();
    check (volumePoints().empty(), "view: undo takes off the first pass in one step");
    view.setWritingAutomation (false);

    // Drawing with fades, handles and both envelopes shown must not fail.
    SessionEdits::setEnvelope (initial, trackId, AutomationParameter::pan, { { { 0, -1.0f }, { 300000, 1.0f } } });
    engine.session.apply (initial, "Pan Envelope");
    Mouse::settle();
    view.showEnvelope (trackId, AutomationParameter::pan);
    const auto image = view.createComponentSnapshot (view.getLocalBounds(), true, 1.0f);
    auto envelopePixels = 0;

    for (int py = MultitrackView::rulerHeight; py < MultitrackView::rulerHeight + MultitrackView::trackHeight; ++py)
        for (int px = MultitrackView::headerWidth; px < image.getWidth(); ++px)
            if (image.getPixelAt (px, py) == juce::Colour (0xffff9de0))
                ++envelopePixels;

    check (envelopePixels > 50, "view: draws the pan envelope over the lane (" + juce::String (envelopePixels) + " pixels of its colour)");

    // An effect's parameter: Apple's AUDelay on the track, its first parameter's envelope shown.
    // A click on the line adds a point at the parameter's current value.
    juce::AudioPluginFormat* au = nullptr;

    for (auto* f : engine.pluginFormatManager.getFormats())
        if (f->getName() == "AudioUnit")
            au = f;

    juce::OwnedArray<juce::PluginDescription> types;

    if (au != nullptr)
        au->findAllTypesForFile (types, "AudioUnit:Effects/aufx,dely,appl");

    if (types.size() == 1)
    {
        auto added = false;
        engine.addTrackPlugin (trackId, *types[0], [&] (const juce::String&) { added = true; });
        TestUtils::runUntil ([&] { return added; }, 30000);
        const auto slotId = engine.getTrackEffects().getSlotId (trackId, 0);
        auto* plugin = engine.getTrackEffects().getPlugin (trackId, 0);
        const auto parameters = plugin != nullptr ? TrackEffects::parametersById (*plugin) : decltype (TrackEffects::parametersById (*plugin)) {};

        if (! parameters.empty())
        {
            const auto& [parameterId, parameter] = *parameters.begin();
            const auto target = AutomationTarget::effect (slotId, parameterId);
            view.showEnvelope (trackId, target);
            auto effectLineY = -1.0f;

            for (auto y = (float) MultitrackView::rulerHeight; y < (float) (MultitrackView::rulerHeight + MultitrackView::trackHeight); y += 1.0f)
            {
                if (mouse.cursorAt ({ pointX, y }) == juce::MouseCursor (juce::MouseCursor::CrosshairCursor))
                {
                    effectLineY = y + 5.0f;
                    break;
                }
            }

            mouse.click ({ pointX, effectLineY });
            const auto& effectPoints = engine.session.getState()->tracks[0].envelope (target).points;
            check (effectLineY > 0.0f && effectPoints.size() == 1 && std::abs (effectPoints[0].value - parameter->getValue()) < 1.0e-6f
                       && engine.session.getUndoDescription() == "Add Envelope Point",
                   "view: a click on an effect parameter's line adds a point at its current value (" + parameter->getName (40) + ")");

            const auto effectImage = view.createComponentSnapshot (view.getLocalBounds(), true, 1.0f);
            auto effectPixels = 0;
            // A level line is antialiased across two rows, blending it with the lane, so count
            // pixels of its yellow-green hue (green over red over blue; the clips are teal).
            const auto yellowGreen = [] (juce::Colour c)
            {
                return (int) c.getGreen() > (int) c.getRed() + 20 && (int) c.getRed() > (int) c.getBlue() + 20;
            };

            for (int py = MultitrackView::rulerHeight; py < MultitrackView::rulerHeight + MultitrackView::trackHeight; ++py)
                for (int px = MultitrackView::headerWidth; px < effectImage.getWidth(); ++px)
                    if (yellowGreen (effectImage.getPixelAt (px, py)))
                        ++effectPixels;

            check (effectPixels > 50, "view: draws the effect parameter's envelope (" + juce::String (effectPixels) + " pixels of its hue)");

            // Writing it from the effect's window: grabbed at 0.3 while playing at 10000, turned to
            // 0.6 by 20000, let go. Without playback a grab writes nothing.
            const auto clicked = engine.session.getState()->tracks[0].envelope (target).points;
            using Kind = TrackEffects::ParameterTouch::Kind;
            // No message loop between touches: with the player stopped, the view's timer would end the pass.
            const auto touch = [&] (Kind kind, float value, std::optional<juce::int64> frame)
            {
                view.effectParameterTouched ({ kind, trackId, slotId, parameterId, value }, frame);
            };

            view.setWritingAutomation (true);
            touch (Kind::begin, 0.3f, std::nullopt);
            check (! view.isAutomationPassUnderWay (trackId, target), "view: an effect's control grabbed while stopped writes nothing");

            touch (Kind::begin, 0.3f, 10000);
            touch (Kind::change, 0.6f, 20000);
            touch (Kind::end, 0.6f, 20000);
            Mouse::settle();
            const auto before = clicked[0].value;
            const std::vector<AutomationPoint> wantWritten { { 10000, before }, { 10000, 0.3f }, { 20000, 0.6f }, { 24800, before }, clicked[0] };
            check (engine.session.getState()->tracks[0].envelope (target).points == wantWritten
                       && engine.session.getUndoDescription() == "Write Effect Automation" && ! view.isAutomationPassUnderWay (trackId, target),
                   "view: turning an effect's control in its window while playing writes its envelope, as one step");

            engine.session.undo();
            Mouse::settle();
            view.setWritingAutomation (false);
        }

        engine.removeTrackPlugin (trackId, 0);
    }

    // A loop-recorded clip: choosing another take is an undo step of its own.
    {
        SessionState withTakes;
        withTakes.sampleRate = rate;
        withTakes.addTrack();
        const auto id = SessionEdits::recordTakes (withTakes, 0, juce::File ("/tmp/takes.wav"), silence (96000), 48000, 24000, { 0, 24000, 48000 });
        engine.session.reset (withTakes, {});
        Mouse::settle();
        view.chooseTake (id, 0);
        check (clip (id).activeTake == 0 && clip (id).offset == 0 && engine.session.getUndoDescription() == "Choose Take 1",
               "view: choosing a clip's take is one undo step");

        engine.session.undo();
        check (clip (id).activeTake == 2 && clip (id).offset == 48000, "view: undo goes back to the take that was playing");
    }

    // Several tracks armed at once, each with its own input; buses can't be armed.
    {
        SessionState several;
        several.sampleRate = rate;
        const auto one = several.addTrack().id;
        const auto two = several.addTrack().id;
        const auto three = several.addTrack().id;
        const auto bus = several.addBus().id;
        engine.session.reset (several, {});
        Mouse::settle();

        view.setArmed (three, true);
        view.setArmed (one, true);
        view.setArmed (bus, true);
        check (view.getArmedTracks() == std::vector<juce::uint32> { one, three } && ! view.isArmed (two) && ! view.isArmed (bus),
               "view: several tracks can be armed at once, in the session's order; a bus can't");

        view.setTrackInput (one, { 2, 2 });
        const auto* t = engine.session.getState()->findTrack (one);
        check (t->input == TrackInput { 2, 2 } && engine.session.getUndoDescription() == "Track Input",
               "view: choosing a track's input is one undo step");

        auto without = *engine.session.getState();
        SessionEdits::removeTrack (without, 2);
        engine.session.apply (std::move (without), "Delete Track");
        Mouse::settle();
        check (view.getArmedTracks() == std::vector<juce::uint32> { one }, "view: a deleted track is no longer armed");

        view.setArmed (one, false);
    }

    // Bars and beats at 100 a minute (a beat every 28800 frames): a clip dragged near beat 2
    // lands on it; with seconds on the ruler it lands where it was dropped.
    {
        SessionState timed;
        timed.sampleRate = rate;
        timed.tempo = 100.0;
        timed.addTrack();
        SessionEdits::addClip (timed, 0, juce::File ("/tmp/beat.wav"), silence (48000), 48000);
        const auto id = timed.tracks[0].clips[0].id;
        engine.session.reset (timed, {});
        view.setTimeSelection ({});
        view.setCursor (0);
        Mouse::settle();

        view.setShowBars (true);
        mouse.drag ({ x (48000) + 40.0f, bodyY }, { x (57000) + 40.0f, bodyY });
        const auto onBeat = clip (id).position;
        engine.session.undo();
        Mouse::settle();

        view.setShowBars (false);
        mouse.drag ({ x (48000) + 40.0f, bodyY }, { x (57000) + 40.0f, bodyY });
        const auto inSeconds = clip (id).position;
        engine.session.undo();
        Mouse::settle();

        check (onBeat == 57600 && inSeconds != 57600 && view.isSnapping(),
               "view: in bars and beats a dragged clip snaps to the beat (" + juce::String (onBeat) + "), not otherwise ("
                   + juce::String (inSeconds) + ")");
    }

    // Markers on the session's time: added at the cursor and over the time range, gone to, clicked,
    // dragged with snapping, snapped to, and saved.
    {
        SessionState marked;
        marked.sampleRate = rate;
        marked.addTrack();
        SessionEdits::addClip (marked, 0, juce::File ("/tmp/marked.wav"), silence (96000), 48000);
        const auto id = marked.tracks[0].clips[0].id;
        engine.session.reset (marked, {});
        view.setTimeSelection ({});
        view.setCursor (200000);
        Mouse::settle();

        view.addMarker();
        view.setTimeSelection ({ 300000, 360000 });
        view.addMarker();
        view.setTimeSelection ({});
        Mouse::settle();
        auto markers = engine.session.getState()->markers;
        check (markers.size() == 2 && markers[0] == Marker { 200000, 0, "Marker 01" } && markers[1] == Marker { 300000, 60000, "Region 01" }
                   && engine.session.getUndoDescription() == "Add Region 01",
               "view: Add Marker marks the cursor, or makes a region of the time range, as undo steps");

        view.setCursor (0);
        std::vector<juce::int64> stops;

        while (view.goToMarker (true))
            stops.push_back (view.getCursor());

        view.goToMarker (false);
        check (stops == std::vector<juce::int64> { 200000, 300000, 360000 } && view.getCursor() == 300000,
               "view: Next Marker stops at each marker and region edge, Previous Marker goes back");

        const auto laneY = (float) MultitrackView::timeRulerHeight + (float) MultitrackView::markerLaneHeight * 0.5f;
        view.setCursor (0);
        mouse.click ({ x (200000) + 2.0f, laneY });
        const auto clickedMarker = view.getCursor();
        mouse.click ({ x (330000), laneY });
        check (clickedMarker == 200000 && view.getCursor() == 300000 && view.getTimeSelection() == juce::Range<juce::int64> (300000, 360000)
                   && engine.session.getUndoDescription() == "Add Region 01"
                   && mouse.cursorAt ({ x (200000), laneY }) == juce::MouseCursor::DraggingHandCursor,
               "view: clicking a marker moves the cursor to it, clicking a region selects it, and neither is an edit");

        // Grabbed 2 px right of its flag and dropped 3 px past the clip's end: it snaps to the end.
        view.setTimeSelection ({});
        mouse.drag ({ x (200000) + 2.0f, laneY }, { x (144000) + 5.0f, laneY });
        markers = engine.session.getState()->markers;
        check (markers[0].start == 144000 && markers[0].name == "Marker 01" && markers[1].start == 300000 && view.getCursor() == 144000
                   && engine.session.getUndoDescription() == "Move Marker 01",
               "view: a dragged marker snaps to a clip's end, as an undo step (" + juce::String (markers[0].start) + ")");

        // A clip moved so its start comes 3 px after the region's start snaps to it; without the
        // markers it lands where it is dropped.
        const auto clipGrab = juce::Point<float> (x (60000), bodyY);
        const auto toRegion = x (300000) + 3.0f - x (48000);
        mouse.drag (clipGrab, clipGrab.translated (toRegion, 0.0f));
        const auto onRegion = clip (id).position;
        engine.session.undo();
        auto unmarked = *engine.session.getState();
        unmarked.markers.clear();
        engine.session.apply (std::move (unmarked), "Delete All Markers");
        Mouse::settle();
        mouse.drag (clipGrab, clipGrab.translated (toRegion, 0.0f));
        const auto withoutMarkers = clip (id).position;
        check (onRegion == 300000 && withoutMarkers != 300000,
               "view: a clip dragged near a region's start snaps to it (" + juce::String (onRegion) + "), and not without it ("
                   + juce::String (withoutMarkers) + ")");
        engine.session.undo();
        engine.session.undo();
        Mouse::settle();

        // The marker being dragged is not a target for itself.
        SessionState one;
        one.markers = { { 1000, 0, "Marker 01" } };
        SessionSnap::Targets targets, ignoring;
        ignoring.ignoreMarker = 0;
        check (SessionSnap::snap (one, 1003, 10, targets) == 1000 && SessionSnap::snap (one, 1003, 10, ignoring) == 1003
                   && SessionSnap::snap (one, 1100, 10, targets) == 1100,
               "snapping: markers are targets, except the one being dragged");

        // Session files keep them, sorted, at their frames.
        auto saved = *engine.session.getState();
        saved.markers = { { 5000000000, 0, "Late" }, { 300000, 60000, "Chorus" }, { 300000, 0, "Drop" } };
        const auto file = juce::File::createTempFile (".sbsession");
        const auto error = SessionFile::save (saved, file);
        const auto loaded = SessionFile::load (file, engine.formatManager);
        file.deleteFile();
        check (error.isEmpty() && loaded.state.markers == Markers { { 300000, 0, "Drop" }, { 300000, 60000, "Chorus" }, { 5000000000, 0, "Late" } },
               "session files: markers and regions are saved and loaded, in order");

        check (MarkerEdits::nextStop ({ { 10, 5, "R" }, { 12, 0, "M" } }, 10, true) == 12
                   && MarkerEdits::nextStop ({ { 10, 5, "R" }, { 12, 0, "M" } }, 12, true) == 15
                   && ! MarkerEdits::nextStop ({ { 10, 5, "R" } }, 15, true).has_value()
                   && MarkerEdits::nextStop ({ { 10, 5, "R" } }, 15, false) == 10,
               "markers: the next stop is the nearest marker or region edge in that direction");
    }

    // Clip gain: normalizing to the peak of the part a clip plays, its limits, and Command-dragging.
    {
        // Two channels of a slow sine, with a louder spike outside the part the clip plays.
        juce::AudioBuffer<float> b (2, 96000);

        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            b.setSample (0, i, 0.3f * std::sin ((float) i * 0.001f));
            b.setSample (1, i, -0.45f * std::sin ((float) i * 0.0007f + 0.2f));
        }

        b.setSample (0, 100, 0.99f);
        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (juce::AudioBuffer<float> (b)), 0, 96000 } };
        auto audio = std::make_shared<const AudioSnapshot> (2, std::move (segments));

        SessionState gained;
        gained.sampleRate = rate;
        gained.addTrack();
        const auto id = SessionEdits::addClip (gained, 0, juce::File ("/tmp/gain.wav"), audio, 48000);
        SessionEdits::trimClip (gained, id, 48000 + 1000, 48000 + 96000);

        auto peak = 0.0f;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 1000; i < 96000; ++i)
                peak = juce::jmax (peak, std::abs (b.getSample (ch, i)));

        auto normalized = gained;
        const auto changed = SessionEdits::normalizeClip (normalized, id, -1.0f);
        const auto& c = normalized.tracks[0].clips[0];
        const auto normalGain = -1.0f - juce::Decibels::gainToDecibels (peak, -1000.0f);
        check (changed && juce::exactlyEqual (c.gainDb, normalGain) && normalGain > 5.9f && normalGain < 6.0f,
               "clip gain: normalizing sets the gain from the loudest sample the clip plays, not what it leaves out ("
                   + juce::String (c.gainDb, 3) + " dB)");

        auto silent = gained;
        silent.tracks[0].clips[0].audio = silence (96000);
        auto limited = gained;
        SessionEdits::setClipGain (limited, id, 40.0f);
        const auto high = limited.tracks[0].clips[0].gainDb;
        SessionEdits::setClipGain (limited, id, -200.0f);
        check (! SessionEdits::normalizeClip (silent, id, 0.0f) && juce::exactlyEqual (silent.tracks[0].clips[0].gainDb, 0.0f)
                   && juce::exactlyEqual (high, SessionClip::maxGainDb) && juce::exactlyEqual (limited.tracks[0].clips[0].gainDb, SessionClip::minGainDb)
                   && ! SessionEdits::setClipGain (limited, 9999, 1.0f),
               "clip gain: a silent clip is left alone, and the gain stays within its limits");

        // Command-dragged 20 px up and then 8 px down from where it started: +5 dB, then -2 dB.
        engine.session.reset (gained, {});
        Mouse::settle();
        const auto at = juce::Point<float> (x (100000), bodyY);
        mouse.drag (at, at.translated (0.0f, -20.0f), juce::ModifierKeys::commandModifier);
        const auto up = engine.session.getState()->tracks[0].clips[0].gainDb;
        const auto upDescription = engine.session.getUndoDescription();
        const auto upPosition = engine.session.getState()->tracks[0].clips[0].position;
        engine.session.undo();
        Mouse::settle();
        mouse.drag (at, at.translated (30.0f, 8.0f), juce::ModifierKeys::commandModifier);
        const auto down = engine.session.getState()->tracks[0].clips[0];
        check (juce::exactlyEqual (up, 5.0f) && upDescription == "Clip Gain" && upPosition == 49000 && juce::exactlyEqual (down.gainDb, -2.0f)
                   && down.position == 49000
                   && mouse.cursorAt (at) == juce::MouseCursor::DraggingHandCursor,
               "view: Command-dragging a clip up or down changes its gain by 0.25 dB a pixel, without moving it ("
                   + juce::String (up, 2) + ", " + juce::String (down.gainDb, 2) + ")");
    }

    engine.session.reset ({}, {});
    Mouse::settle();
}
