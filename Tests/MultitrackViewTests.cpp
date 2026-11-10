#include <JuceHeader.h>
#include "../Source/AudioEdits.h"
#include "../Source/AudioFileIO.h"
#include "../Source/MultitrackView.h"
#include "../Source/SessionOverview.h"
#include "../Source/TrackColours.h"
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

        // Select Between Markers: the time range around the cursor, to the end of the last clip
        // past the last marker; not an edit.
        auto spanned = *engine.session.getState();
        spanned.markers = { { 30000, 0, "Verse" }, { 60000, 20000, "Chorus" } };
        engine.session.apply (std::move (spanned), "Markers");
        Mouse::settle();
        const auto lastClipEnd = engine.session.getState()->getLength();
        view.setCursor (50000);
        const auto middle = view.selectBetweenMarkers() && view.getTimeSelection() == juce::Range<juce::int64> (30000, 60000);
        view.setCursor (100000);
        const auto toEnd = view.selectBetweenMarkers() && view.getTimeSelection() == juce::Range<juce::int64> (80000, lastClipEnd);
        view.setCursor (10000);
        const auto fromStart = view.selectBetweenMarkers() && view.getTimeSelection() == juce::Range<juce::int64> (0, 30000);
        view.setTimeSelection ({});
        view.setCursor (lastClipEnd + 1000);
        const auto pastEnd = ! view.selectBetweenMarkers() && view.getTimeSelection().isEmpty();
        check (lastClipEnd == 144000 && middle && toEnd && fromStart && pastEnd && engine.session.getUndoDescription() == "Markers",
               "view: Select Between Markers selects around the cursor, from the session's start or to the last clip's end; nothing past it; no edit");
        view.setTimeSelection ({});

        // Select Region at Cursor: the chorus from inside it; nothing in the verse, which is a point.
        view.setCursor (70000);
        const auto inRegion = view.selectRegionAtCursor() && view.getTimeSelection() == juce::Range<juce::int64> (60000, 80000);
        view.setTimeSelection ({});
        view.setCursor (40000);
        check (inRegion && ! view.selectRegionAtCursor() && view.getTimeSelection().isEmpty() && engine.session.getUndoDescription() == "Markers",
               "view: Select Region at Cursor selects the region the cursor is in, and nothing outside one; no edit");

        // Markers at intervals over the whole session (no time selection): 0, 1 s and 2 s, one undo step.
        const auto markersBefore = engine.session.getState()->markers;
        const auto addedEvery = view.addMarkersAtIntervals (48000);
        const auto& spacedMarkers = engine.session.getState()->markers;
        const auto atSeconds = addedEvery == 3 && spacedMarkers.size() == markersBefore.size() + 3
                               && std::count_if (spacedMarkers.begin(), spacedMarkers.end(), [] (const Marker& m)
                                                 { return ! m.isRegion() && (m.start == 0 || m.start == 48000 || m.start == 96000); }) == 3
                               && engine.session.getUndoDescription() == "Add 3 Markers";
        engine.session.undo();
        check (atSeconds && engine.session.getState()->markers == markersBefore,
               "view: markers at intervals through the whole session as one undo step, and undo takes them away");

        // Regions from markers: the verse point runs to the end of the last clip; the chorus region stays.
        const auto turnedInView = view.regionsFromMarkers();
        const auto& regioned = engine.session.getState()->markers;
        const auto verseRegion = std::find_if (regioned.begin(), regioned.end(), [] (const Marker& m) { return m.name == "Verse"; });
        check (turnedInView == 1 && verseRegion != regioned.end() && verseRegion->start == 30000 && verseRegion->getEnd() == 144000
                   && engine.session.getUndoDescription() == "Regions from Markers",
               "view: regions from markers runs the last point to the end of the last clip, as one undo step");
        engine.session.undo();

        const auto renamedInView = view.renameMarkersInSequence ("Part");
        const auto& partNames = engine.session.getState()->markers;
        check (renamedInView && partNames.size() == 2 && partNames[0].name == "Part 01" && partNames[1].name == "Part 02"
                   && engine.session.getUndoDescription() == "Rename Markers" && ! view.renameMarkersInSequence ("Part"),
               "view: renaming the markers in sequence is one undo step; the same names again add none");
        engine.session.undo();

        // Going to a time far out of view scrolls there, keeping the zoom; one in view doesn't scroll.
        const auto viewBefore = view.getVisibleFrames();
        const auto zoomBefore = viewBefore.getLength();
        view.showFrame (48000 * 300);
        const auto far = view.getVisibleFrames();
        view.showFrame (48000 * 300 + 4800);
        check (view.getCursor() == 48000 * 300 + 4800 && far.contains (48000.0 * 300) && std::abs (far.getLength() - zoomBefore) < 1.0
                   && std::abs (far.getStart() - (48000.0 * 300 - zoomBefore * 0.1)) < 1.0 && view.getVisibleFrames() == far,
               "view: showing a frame out of view scrolls it a tenth of the way in, keeping the zoom; one in view doesn't scroll, even past the session's end");
        view.scrollTo (viewBefore.getStart());
        Mouse::settle();

        // Double-clicking an empty spot in the marker lane adds a marker there, snapped with
        // snapping on (3 px past the clip's end lands on it).
        const auto wasSnapping = view.isSnapping();
        const auto doubleClick = [&] (float atX)
        {
            const juce::Point<float> at { atX, laneY };
            view.mouseDoubleClick (mouse.event (at, at, {}, false));
            Mouse::settle();
        };
        view.setSnapping (false);
        doubleClick (x (100000));
        const auto plain = engine.session.getState()->markers;
        const auto plainStep = engine.session.getUndoDescription();
        view.setSnapping (true);
        view.setCursor (0);
        doubleClick (x (144000) + 3.0f);
        const auto snappedMarkers = engine.session.getState()->markers;
        view.setSnapping (wasSnapping);
        check (plain.size() == 3 && plain[2].name == "Marker 01" && std::abs (plain[2].start - 100000) <= 1000 && ! plain[2].isRegion()
                   && plainStep == "Add Marker 01" && snappedMarkers.size() == 4 && snappedMarkers[3].start == 144000
                   && snappedMarkers[3].name == "Marker 02",
               "view: double-clicking an empty spot in the marker lane adds a marker there (" + juce::String (plain.size() == 3 ? plain[2].start : -1)
                   + "), snapped to a clip's end with snapping on, as undo steps");
        engine.session.undo();
        engine.session.undo();
        Mouse::settle();

        // Marker colours: chosen as undo steps, drawn on the ruler, kept through edits and files.
        auto coloured = *engine.session.getState();
        coloured.markers = { { 100000, 0, "Verse" }, { 200000, 50000, "Chorus" } };
        engine.session.apply (std::move (coloured), "Markers");
        Mouse::settle();
        const auto verse = engine.session.getState()->markers[0];
        const auto set = view.setMarkerColour (verse, 1);
        const auto setStep = engine.session.getUndoDescription();
        const auto again = view.setMarkerColour (engine.session.getState()->markers[0], 1);
        const auto gone = view.setMarkerColour (verse, 2);
        const auto chorus = engine.session.getState()->markers[1];
        const auto clamped = view.setMarkerColour (chorus, 99) || engine.session.getState()->markers[1].colour != -1;
        auto nowMarkers = engine.session.getState()->markers;
        check (set && setStep == "Colour Verse" && ! again && ! gone && ! clamped && nowMarkers[0].colour == 1 && nowMarkers[1].colour == -1,
               "marker colours: choosing one is an undo step; the same colour, a marker that is gone or a colour past the palette changes nothing");

        view.setTimeSelection ({});
        Mouse::settle();
        const auto picture = view.createComponentSnapshot (view.getLocalBounds());
        const auto flagX = juce::roundToInt (x (100000)) + 2;
        const auto laneMid = MultitrackView::timeRulerHeight + MultitrackView::markerLaneHeight / 2;
        const auto drawn = picture.getPixelAt (flagX, laneMid) == juce::Colour (TrackColours::palette[1].argb)
                           && TrackColours::of (nowMarkers[1]) == juce::Colour (0xff58a6ff) && TrackColours::of (Marker { 1, 0, "p" }) == juce::Colour (0xffff8a3d);

        const auto moved = MarkerEdits::afterReplace (nowMarkers, 0, 1000, 0);
        const auto cropped = MarkerEdits::afterCrop (nowMarkers, 150000, 260000);
        const auto kept = moved.size() == 2 && moved[0].colour == 1 && cropped.size() == 1 && cropped[0].colour == -1
                          && MarkerEdits::afterCrop (nowMarkers, 50000, 220000)[0].colour == 1;

        auto colouredFile = *engine.session.getState();
        colouredFile.markers[1].colour = 7;
        const auto colourFile = juce::File::createTempFile (".sbsession");
        const auto colourError = SessionFile::save (colouredFile, colourFile);
        const auto colourLoaded = SessionFile::load (colourFile, engine.formatManager);
        auto text = colourFile.loadFileAsString();
        colourFile.replaceWithText (text.replace ("colour=\"7\"", "colour=\"42\""));
        const auto pastPalette = SessionFile::load (colourFile, engine.formatManager);
        colourFile.deleteFile();
        const auto filed = colourError.isEmpty() && colourLoaded.state.markers == colouredFile.markers
                           && pastPalette.state.markers.size() == 2 && pastPalette.state.markers[1].colour == 7
                           && text.contains ("colour=\"1\"") && text.contains ("colour=\"7\"");

        engine.session.undo();
        const auto undone = engine.session.getState()->markers[0].colour == -1;
        check (drawn && kept && filed && undone,
               "marker colours: the flag drawn in its colour, defaults orange and blue, kept by time edits and crops, saved and loaded, undone");
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

    // Copying, pasting and duplicating clips: copies share the audio, keep gain and fades, and
    // replace what was under them.
    {
        SessionState copying;
        copying.sampleRate = rate;
        copying.addTrack();
        copying.addTrack();
        const auto a = SessionEdits::addClip (copying, 0, juce::File ("/tmp/a.wav"), silence (96000), 48000);
        SessionEdits::addClip (copying, 1, juce::File ("/tmp/long.wav"), silence (200000), 0);
        SessionEdits::setClipGain (copying, a, 3.0f);
        SessionEdits::setClipFades (copying, a, 1000, 2000);
        engine.session.reset (copying, {});
        view.setTimeSelection ({});
        Mouse::settle();
        const auto laneY = [] (int lane) { return (float) (MultitrackView::rulerHeight + lane * MultitrackView::trackHeight) + 60.0f; };
        const auto same = [] (const SessionClip& c, const SessionClip& o)
        {
            return c.audio == o.audio && c.offset == o.offset && c.length == o.length && juce::exactlyEqual (c.gainDb, o.gainDb)
                && c.fadeIn == o.fadeIn && c.fadeOut == o.fadeOut && c.name == o.name;
        };

        mouse.click ({ x (100000), laneY (0) });
        const auto original = engine.session.getState()->tracks[0].clips[0];
        view.duplicateSelectedClip();
        Mouse::settle();
        auto st = engine.session.getState();
        check (view.getSelectedClip() != a && st->tracks[0].clips.size() == 2 && st->tracks[0].clips[1].position == 144000
                   && st->tracks[0].clips[1].id == view.getSelectedClip() && same (st->tracks[0].clips[1], original)
                   && engine.session.getUndoDescription() == "Duplicate Clip",
               "clips: Duplicate places a copy right after the clip, sharing its audio, gain and fades, and selects it");

        // Copy A, then paste it twice onto the second track from 50000: the long clip is cut
        // around the first paste, and the second takes the rest.
        mouse.click ({ x (100000), laneY (0) });
        const auto copied = view.copySelectedClip();
        mouse.click ({ x (20000), laneY (1) });
        view.setCursor (50000);
        view.pasteClip();
        Mouse::settle();
        st = engine.session.getState();
        const auto firstCursor = view.getCursor();
        const auto& t1 = st->tracks[1].clips;
        const auto pastedOk = t1.size() == 3 && t1[0].position == 0 && t1[0].length == 50000 && t1[1].position == 146000
                              && t1[1].getEnd() == 200000 && t1[2].position == 50000 && same (t1[2], original)
                              && t1[2].id == view.getSelectedClip() && engine.session.getUndoDescription() == "Paste Clip";
        view.pasteClip();
        Mouse::settle();
        st = engine.session.getState();
        const auto& again = st->tracks[1].clips;
        check (copied && pastedOk && firstCursor == 146000 && again.size() == 3 && again[0].getEnd() == 50000
                   && again[1].position == 50000 && again[2].position == 146000 && view.getCursor() == 242000,
               "clips: Paste puts the copy at the cursor on the selected track over what is there, and the next paste follows it");

        view.cutSelectedClip();
        Mouse::settle();
        st = engine.session.getState();
        const auto cutOk = st->tracks[1].clips.size() == 2 && engine.session.getUndoDescription() == "Cut Clip" && view.getSelectedClip() == 0
                           && view.canPaste();
        auto otherRate = copying;
        otherRate.sampleRate = 44100.0;
        engine.session.reset (otherRate, {});
        Mouse::settle();
        check (cutOk && ! view.canPaste(), "clips: Cut removes the clip and keeps it to paste, but not into a session at another rate");

        auto withBus = copying;
        withBus.addBus();
        const auto onBus = SessionEdits::placeClip (withBus, 2, original, 0);
        check (withBus.tracks.size() == 4 && withBus.tracks[3].clips.size() == 1 && withBus.tracks[3].clips[0].id == onBus && withBus.tracks[2].clips.empty(),
               "clips: a clip placed on a bus goes onto a new track");
    }

    // Several clips selected with Shift-click: moved together, pasted and duplicated keeping
    // their places, deleted together.
    {
        SessionState several;
        several.sampleRate = rate;
        several.addTrack();
        several.addTrack();
        several.addTrack();
        const auto a = SessionEdits::addClip (several, 0, juce::File ("/tmp/a.wav"), silence (48000), 48000);
        const auto b = SessionEdits::addClip (several, 1, juce::File ("/tmp/b.wav"), silence (48000), 120000);
        const auto c = SessionEdits::addClip (several, 2, juce::File ("/tmp/c.wav"), silence (48000), 0);
        engine.session.reset (several, {});
        view.setTimeSelection ({});
        Mouse::settle();
        const auto laneY = [] (int lane) { return (float) (MultitrackView::rulerHeight + lane * MultitrackView::trackHeight) + 60.0f; };
        const auto shift = juce::ModifierKeys::shiftModifier;
        const auto positionOf = [&] (juce::uint32 id) { return clip (id).position; };
        const auto trackOf = [&] (juce::uint32 id) { return engine.session.getState()->findClip (id)->first; };

        mouse.click ({ x (70000), laneY (0) });
        mouse.click ({ x (140000), laneY (1) }, shift);
        const auto both = view.getSelectedClips();
        const auto primary = view.getSelectedClip();
        mouse.click ({ x (70000), laneY (0) }, shift);
        const auto justB = view.getSelectedClips();
        mouse.click ({ x (70000), laneY (0) }, shift);
        check (both == std::vector<juce::uint32> { a, b } && primary == b && justB == std::vector<juce::uint32> { b }
                   && view.getSelectedClips() == std::vector<juce::uint32> { a, b } && ! view.isClipSelected (c),
               "selection: Shift-click adds a clip to the selection and takes it out again");

        // Dragging one of them moves both by the same time, each on its own track.
        view.setSnapping (false);
        mouse.drag ({ x (70000), laneY (0) }, { x (70000) + 50.0f, laneY (0) });
        const auto movedA = positionOf (a) - 48000, movedB = positionOf (b) - 120000;
        check (movedA > 0 && movedA == movedB && trackOf (a) == 0 && trackOf (b) == 1 && positionOf (c) == 0
                   && engine.session.getUndoDescription() == "Move Clip",
               "selection: dragging a selected clip moves every selected clip in time by the same amount (" + juce::String (movedA) + ")");
        engine.session.undo();
        Mouse::settle();

        mouse.drag ({ x (70000), laneY (0) }, { x (70000) - 400.0f, laneY (2) });
        check (positionOf (a) == 0 && positionOf (b) == 72000 && trackOf (a) == 0 && trackOf (b) == 1,
               "selection: moved together they stop when the earliest reaches the start, and keep their tracks");
        engine.session.undo();
        Mouse::settle();
        view.setSnapping (true);

        // Copy both, paste with the third track selected: the copies keep their spacing, the
        // second on a new track below.
        view.copySelectedClip();
        mouse.click ({ x (20000), laneY (2) });
        view.setCursor (300000);
        view.pasteClip();
        Mouse::settle();
        auto pasted = view.getSelectedClips();
        auto st = engine.session.getState();
        check (pasted.size() == 2 && st->tracks.size() == 4 && trackOf (pasted[0]) == 2 && trackOf (pasted[1]) == 3
                   && positionOf (pasted[0]) == 300000 && positionOf (pasted[1]) == 372000 && view.getCursor() == 420000
                   && engine.session.getUndoDescription() == "Paste 2 Clips",
               "selection: pasted clips keep their places relative to each other, from the cursor and the selected track");

        view.duplicateSelectedClip();
        Mouse::settle();
        const auto duplicated = view.getSelectedClips();
        check (duplicated.size() == 2 && trackOf (duplicated[0]) == 2 && trackOf (duplicated[1]) == 3
                   && positionOf (duplicated[0]) == 420000 && positionOf (duplicated[1]) == 492000
                   && engine.session.getUndoDescription() == "Duplicate 2 Clips",
               "selection: Duplicate places the selected clips together right after them");

        view.deleteSelectedClip();
        Mouse::settle();
        st = engine.session.getState();
        const auto deleted = engine.session.getUndoDescription() == "Delete 2 Clips" && ! st->findClip (duplicated[0]).has_value()
                             && ! st->findClip (duplicated[1]).has_value() && view.getSelectedClips().empty();
        view.selectAllClips();
        check (deleted && view.getSelectedClips().size() == 5,
               "selection: deleting removes every selected clip, and Select All selects every clip");

        mouse.click ({ x (900000), laneY (0) });
        check (view.getSelectedClips().empty() && view.getSelectedClip() == 0, "selection: clicking where there is no clip selects none");
    }

    // Zoom to Time Range: the range fills the timeline, with 2.5% of it either side.
    {
        view.zoomToRange ({ 96000, 192000 });
        const auto shownFrames = view.getVisibleFrames();
        view.zoomToRange ({});
        check (std::abs (shownFrames.getStart() - 93600.0) < 1.0e-6 && std::abs (shownFrames.getEnd() - 194400.0) < 1.0e-6
                   && view.getVisibleFrames() == shownFrames,
               "view: Zoom to Time Range shows the range with a margin either side (" + juce::String (shownFrames.getStart(), 1) + " to "
                   + juce::String (shownFrames.getEnd(), 1) + "); an empty range changes nothing");
    }

    // Moving tracks: the edit, the undo step, and dragging a header by its background.
    {
        SessionState ordered;
        ordered.sampleRate = rate;

        for (auto name : { "One", "Two", "Three", "Four" })
            ordered.addTrack().name = name;

        const auto names = [] (const SessionState& st)
        {
            juce::StringArray result;

            for (auto& t : st.tracks)
                result.add (t.name);

            return result.joinIntoString (" ");
        };

        auto edited = ordered;
        const auto down = SessionEdits::moveTrack (edited, 0, 2);
        const auto afterDown = names (edited);
        const auto up = SessionEdits::moveTrack (edited, 3, 1);
        check (down && afterDown == "Two Three One Four" && up && names (edited) == "Two Four Three One"
                   && ! SessionEdits::moveTrack (edited, 1, 1) && ! SessionEdits::moveTrack (edited, 0, 4) && ! SessionEdits::moveTrack (edited, -1, 0),
               "tracks: moving a track puts it at the new place and closes up behind it (" + afterDown + ", then " + names (edited) + ")");

        engine.session.reset (ordered, {});
        Mouse::settle();
        view.moveTrack (3, 0);
        Mouse::settle();
        const auto moved = names (*engine.session.getState());
        const auto selected = view.getSelectedTrack();
        engine.session.undo();
        Mouse::settle();
        check (moved == "Four One Two Three" && selected == 0 && names (*engine.session.getState()) == "One Two Three Four",
               "tracks: Move Track is an undo step, and the moved track stays selected");

        // The header's background, below its controls: drag the first track down to the third lane.
        const auto background = juce::Point<float> (6.0f, (float) (MultitrackView::rulerHeight + MultitrackView::trackHeight) - 3.0f);
        // The view isn't on screen here, so look through the header area's children for it.
        juce::Component* header = nullptr;

        for (auto* area : view.getChildren())
            for (auto* child : area->getChildren())
                if (view.getLocalArea (child, child->getLocalBounds()).contains (background.toInt()) && child->getNumChildComponents() > 3)
                    header = child;

        const auto isHeader = header != nullptr;

        if (isHeader)
        {
            const auto event = [&] (juce::Point<float> at, bool dragged)
            {
                const auto now = juce::Time::getCurrentTime();
                const auto local = header->getLocalPoint (&view, at), pressed = header->getLocalPoint (&view, background);
                return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), local, juce::ModifierKeys::leftButtonModifier,
                                         juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                                         juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                                         juce::MouseInputSource::defaultTiltY, header, header, now, pressed, now, 1, dragged);
            };
            const auto target = background.translated (0.0f, 2.0f * MultitrackView::trackHeight);
            header->mouseDown (event (background, false));
            header->mouseDrag (event (target, true));
            header->mouseUp (event (target, true));
            Mouse::settle();
        }

        check (isHeader && names (*engine.session.getState()) == "Two Three One Four" && engine.session.getUndoDescription() == "Move Track",
               "tracks: dragging a track's header down two lanes moves it there (" + names (*engine.session.getState()) + ")");
    }

    // Nudging by the grid: at 504 frames a pixel the ruler marks every 2 s (96000 frames), and in
    // bars and beats at 120 a minute every beat (24000).
    {
        SessionState nudged;
        nudged.sampleRate = rate;
        nudged.addTrack();
        nudged.addTrack();
        const auto a = SessionEdits::addClip (nudged, 0, juce::File ("/tmp/a.wav"), silence (48000), 48000);
        const auto b = SessionEdits::addClip (nudged, 1, juce::File ("/tmp/b.wav"), silence (48000), 120000);
        engine.session.reset (nudged, {});
        view.setShowBars (false);
        view.setTimeSelection ({});
        view.zoomToRange ({ 0, 480000 });
        Mouse::settle();
        const auto laneY = [] (int lane) { return (float) (MultitrackView::rulerHeight + lane * MultitrackView::trackHeight) + 60.0f; };

        const auto seconds = std::vector<juce::int64> { view.gridStepFrom (100000, true), view.gridStepFrom (100000, false),
                                                        view.gridStepFrom (96000, false), view.gridStepFrom (96000, true), view.gridStepFrom (0, false) };
        view.setShowBars (true);
        const auto beats = std::vector<juce::int64> { view.gridStepFrom (30000, true), view.gridStepFrom (30000, false),
                                                      view.gridStepFrom (24000, false), view.gridStepFrom (0, false) };
        view.setShowBars (false);
        check (seconds == std::vector<juce::int64> { 192000, 96000, 0, 192000, 0 } && beats == std::vector<juce::int64> { 48000, 24000, 0, 0 },
               "nudge: grid points either side of a frame, in seconds and in beats, never before 0");

        mouse.click ({ x (900000), laneY (0) });
        view.setCursor (100000);
        view.nudge (true);
        const auto cursorNudged = view.getCursor();

        mouse.click ({ x (70000), laneY (0) });
        mouse.click ({ x (140000), laneY (1) }, juce::ModifierKeys::shiftModifier);
        mouse.click ({ x (70000), laneY (0) }, juce::ModifierKeys::shiftModifier);
        mouse.click ({ x (70000), laneY (0) }, juce::ModifierKeys::shiftModifier);   // a, clicked last
        view.nudge (true);
        view.nudge (true);
        Mouse::settle();
        const auto twice = std::pair (clip (a).position, clip (b).position);
        engine.session.undo();
        Mouse::settle();
        const auto undone = std::pair (clip (a).position, clip (b).position);
        view.nudge (false);
        view.nudge (false);
        Mouse::settle();
        check (cursorNudged == 192000 && twice == std::pair<juce::int64, juce::int64> (192000, 264000)
                   && undone == std::pair<juce::int64, juce::int64> (48000, 120000)
                   && clip (a).position == 0 && clip (b).position == 72000 && engine.session.getUndoDescription() == "Nudge Clips",
               "nudge: the cursor, or the selected clips together, move to the next grid point, and a run of nudges is one undo step");
        engine.session.undo();
        Mouse::settle();
    }

    // Repeating a clip to the end of the time range, and reversing one.
    {
        juce::AudioBuffer<float> counting (2, 1000);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 1000; ++i)
                counting.setSample (ch, i, (float) (i + 1) * (ch == 0 ? 0.001f : -0.0005f));

        std::vector<AudioSnapshot::Segment> segments { { std::make_shared<const AudioBlock> (std::move (counting)), 0, 1000 } };
        const auto audio = std::make_shared<const AudioSnapshot> (2, std::move (segments));

        SessionState repeated;
        repeated.sampleRate = rate;
        repeated.addTrack();
        const auto id = SessionEdits::addClip (repeated, 0, juce::File ("/tmp/count.wav"), audio, 10000);
        SessionEdits::trimClip (repeated, id, 10100, 10400);   // plays frames 100 to 399
        SessionEdits::setClipFades (repeated, id, 50, 20);
        SessionEdits::addClip (repeated, 0, juce::File ("/tmp/other.wav"), silence (5000), 10500);

        auto filled = repeated;
        const auto copies = SessionEdits::repeatClip (filled, id, 11000);
        std::vector<std::pair<juce::int64, juce::int64>> spans;

        for (auto& c : filled.tracks[0].clips)
            spans.emplace_back (c.position, c.getEnd());

        std::sort (spans.begin(), spans.end());
        check (copies == 2 && spans == std::vector<std::pair<juce::int64, juce::int64>> { { 10100, 10400 }, { 10400, 10700 }, { 10700, 11000 }, { 11000, 15500 } }
                   && SessionEdits::repeatClip (filled, id, 10300) == 0,
               "repeat: copies end to end up to the range's end, over what was there, the last cut short");

        engine.session.reset (repeated, {});
        view.zoomToRange ({ 10000, 11000 });
        Mouse::settle();
        const auto shownFrames = view.getVisibleFrames();
        const auto zoomedX = [&] (juce::int64 frame)
        {
            return (float) MultitrackView::headerWidth + (float) (((double) frame - shownFrames.getStart()) / (shownFrames.getLength() / timelineWidth));
        };
        mouse.click ({ zoomedX (10250), bodyY });
        view.setTimeSelection ({ 0, 10950 });
        const auto viaView = view.repeatSelectedClip() && engine.session.getUndoDescription() == "Repeat Clip (2 copies)";
        check (viaView, "repeat: from the view, on the selected clip to the end of the time range, as one undo step");
        view.setTimeSelection ({});

        const auto& original = repeated.tracks[0].clips[0];
        const auto reversed = SessionEdits::reversedAudio (original);
        auto backwards = reversed != nullptr && reversed->getNumFrames() == 300 && reversed->getNumChannels() == 2;

        if (backwards)
        {
            juce::AudioBuffer<float> got (2, 300);
            reversed->read (got, 0, 0, 300);

            for (int i = 0; i < 300; ++i)
                for (int ch = 0; ch < 2; ++ch)
                    backwards = backwards && juce::exactlyEqual (got.getSample (ch, i), (float) (400 - i) * (ch == 0 ? 0.001f : -0.0005f));
        }

        auto flipped = repeated;
        SessionEdits::reverseClip (flipped, id, juce::File ("/tmp/count Reversed.wav"), reversed);
        const auto& r = flipped.tracks[0].clips[0];
        check (backwards && r.position == 10100 && r.offset == 0 && r.length == 300 && r.fadeIn == 20 && r.fadeOut == 50
                   && r.source == juce::File ("/tmp/count Reversed.wav") && r.audio == reversed,
               "reverse: the part the clip plays, back to front bit for bit, in place, its fades changing ends");
    }

    // Lane height: clicks find the right lane at another height, the height stays within its
    // limits, and short headers leave out their lower rows.
    {
        SessionState tall;
        tall.sampleRate = rate;
        tall.addTrack();
        tall.addTrack();
        SessionEdits::addClip (tall, 0, juce::File ("/tmp/top.wav"), silence (96000), 0);
        const auto lower = SessionEdits::addClip (tall, 1, juce::File ("/tmp/lower.wav"), silence (96000), 0);
        engine.session.reset (tall, {});
        view.setSize (MultitrackView::headerWidth + timelineWidth + 12, MultitrackView::rulerHeight + 3 * 200 + 12);
        view.zoomToRange ({ 0, 96000 });
        view.setLaneHeight (200);
        Mouse::settle();
        const auto shownFrames = view.getVisibleFrames();
        const auto at = (float) MultitrackView::headerWidth + (float) ((48000.0 - shownFrames.getStart()) / (shownFrames.getLength() / timelineWidth));

        // 150 px down the second lane: past where the first lane would end at the usual height.
        mouse.click ({ at, (float) (MultitrackView::rulerHeight + 200 + 150) });
        const auto picked = view.getSelectedClip() == lower && view.getSelectedTrack() == 1;

        const auto headerHeights = [&]
        {
            std::vector<int> heights;

            for (auto* area : view.getChildren())
                for (auto* child : area->getChildren())
                    if (child->getNumChildComponents() > 3)
                        heights.push_back (child->getHeight());

            return heights;
        };

        const auto tallHeaders = headerHeights();
        view.setLaneHeight (1000);
        const auto highest = view.getLaneHeight();
        view.setLaneHeight (0);
        const auto lowest = view.getLaneHeight();

        // At the shortest, each header keeps its top row only: its sliders get no room.
        auto slidersHidden = true;

        for (auto* area : view.getChildren())
            for (auto* child : area->getChildren())
                if (child->getNumChildComponents() > 3)
                    for (auto* control : child->getChildren())
                        if (dynamic_cast<juce::Slider*> (control) != nullptr)
                            slidersHidden = slidersHidden && control->getBounds().isEmpty();

        view.setLaneHeight (MultitrackView::trackHeight);
        view.setSize (MultitrackView::headerWidth + timelineWidth + 12, MultitrackView::rulerHeight + 3 * MultitrackView::trackHeight + 12);
        check (picked && tallHeaders == std::vector<int> { 200, 200 } && highest == MultitrackView::maxLaneHeight
                   && lowest == MultitrackView::minLaneHeight && slidersHidden,
               "lanes: at 200 px a click finds the second lane's clip, headers follow the height, which stays within "
                   + juce::String (MultitrackView::minLaneHeight) + " to " + juce::String (MultitrackView::maxLaneHeight)
                   + " px, and short headers leave out their sliders");
    }

    // Locked clips: the edits refuse them, clips placed over them overlap, and through the view
    // they can be selected but not dragged, nudged or deleted.
    {
        SessionState locking;
        locking.sampleRate = rate;
        locking.addTrack();
        const auto fixed = SessionEdits::addClip (locking, 0, juce::File ("/tmp/fixed.wav"), silence (48000), 0);
        const auto loose = SessionEdits::addClip (locking, 0, juce::File ("/tmp/loose.wav"), silence (36000), 60000);
        const auto locked = SessionEdits::setClipLocked (locking, fixed, true) && ! SessionEdits::setClipLocked (locking, fixed, true);

        auto tried = locking;
        const auto refused = ! SessionEdits::moveClip (tried, fixed, 0, 1000) && ! SessionEdits::trimClip (tried, fixed, 100, 400)
                             && ! SessionEdits::splitClip (tried, fixed, 24000) && ! SessionEdits::setClipGain (tried, fixed, -6.0f)
                             && ! SessionEdits::setClipFades (tried, fixed, 100, 100) && ! SessionEdits::removeClip (tried, fixed)
                             && ! SessionEdits::reverseClip (tried, fixed, juce::File ("/tmp/fixed Reversed.wav"), silence (48000))
                             && ! SessionEdits::crossfade (tried, fixed);
        const auto& kept = tried.tracks[0].clips[0];
        check (locked && refused && kept.position == 0 && kept.length == 48000 && kept.fadeIn == 0 && tried.tracks[0].clips.size() == 2,
               "locking: moving, trimming, splitting, gain, fades, reversing, crossfading and removing a locked clip are refused");

        // A copy placed over it, then the unlocked clip moved onto its end and crossfaded.
        auto over = locking;
        const auto copy = SessionEdits::placeClip (over, 0, over.tracks[0].clips[0], 12000);
        SessionEdits::moveClip (over, loose, 0, 40000);
        SessionEdits::crossfade (over, loose);
        const auto* placed = &over.tracks[0].clips.back();
        const auto stillWhole = over.tracks[0].clips[0].id == fixed && over.tracks[0].clips[0].length == 48000
                                && over.tracks[0].clips[0].fadeOut == 0;
        check (stillWhole && placed->id == copy && ! placed->locked && over.tracks[0].clips.size() == 3,
               "locking: a clip placed over a locked one overlaps it, unlocked; crossfading leaves the locked one's fades alone");

        // Saved and loaded, with real audio for the clips to find.
        const auto folder = juce::File::createTempFile ("locking");
        folder.createDirectory();
        const auto wav = folder.getChildFile ("fixed.wav");
        auto onDisk = locking;

        for (auto& c : onDisk.tracks[0].clips)
            c.source = wav;

        const auto file = folder.getChildFile ("locking.sbsession");
        const auto wrote = AudioFileIO::save (*silence (48000), rate, wav, 24, engine.formatManager).isEmpty() && SessionFile::save (onDisk, file).isEmpty();
        const auto loaded = SessionFile::load (file, engine.formatManager);
        folder.deleteRecursively();
        check (wrote && loaded.state.tracks.size() == 1 && loaded.state.tracks[0].clips.size() == 2
                   && loaded.state.tracks[0].clips[0].locked && ! loaded.state.tracks[0].clips[1].locked,
               "locking: session files keep which clips are locked");

        engine.session.reset (locking, {});
        view.zoomToRange ({ 0, 96000 });
        Mouse::settle();
        const auto lockFrames = view.getVisibleFrames();
        const auto lockX = [&] (juce::int64 frame)
        {
            return (float) MultitrackView::headerWidth + (float) (((double) frame - lockFrames.getStart()) / (lockFrames.getLength() / timelineWidth));
        };
        const auto positions = [&]
        {
            std::vector<juce::int64> where;

            for (auto& c : engine.session.getState()->tracks[0].clips)
                where.push_back (c.position);

            return where;
        };

        const auto before = positions();
        const auto steps = engine.session.getUndoDescription();
        mouse.drag ({ lockX (24000), bodyY }, { lockX (30000), bodyY });
        const auto draggedLocked = positions() == before && view.getSelectedClip() == fixed && view.areSelectedClipsLocked();
        mouse.click ({ lockX (70000), bodyY }, juce::ModifierKeys::shiftModifier);
        mouse.drag ({ lockX (70000), bodyY }, { lockX (80000), bodyY });
        view.nudge (true);
        const auto groupStayed = positions() == before && engine.session.getUndoDescription() == steps && ! view.areSelectedClipsLocked();
        check (draggedLocked && groupStayed,
               "locking: through the view a locked clip is selected but not dragged, and clips selected with one don't move or nudge");

        view.deleteSelectedClip();
        const auto deletedLoose = engine.session.getState()->tracks[0].clips.size() == 1 && engine.session.getUndoDescription() == "Delete Clip";
        mouse.click ({ lockX (24000), bodyY });
        view.setSelectedClipsLocked (false);
        Mouse::settle();
        const auto unlocked = engine.session.getUndoDescription() == "Unlock Clip" && ! view.areSelectedClipsLocked();
        mouse.drag ({ lockX (24000), bodyY }, { lockX (30000), bodyY });
        check (deletedLoose && unlocked && engine.session.getState()->tracks[0].clips[0].position > 0,
               "locking: deleting the selection leaves the locked clip; unlocked (an undo step) it drags again");
    }

    // Muted clips: muting from the view, still editable, and kept in session files.
    {
        SessionState quiet;
        quiet.sampleRate = rate;
        quiet.addTrack();
        const auto hushed = SessionEdits::addClip (quiet, 0, juce::File ("/tmp/hushed.wav"), silence (48000), 0);
        engine.session.reset (quiet, {});
        view.zoomToRange ({ 0, 96000 });
        Mouse::settle();
        const auto muteFrames = view.getVisibleFrames();
        const auto muteX = [&] (juce::int64 frame)
        {
            return (float) MultitrackView::headerWidth + (float) (((double) frame - muteFrames.getStart()) / (muteFrames.getLength() / timelineWidth));
        };

        mouse.click ({ muteX (24000), bodyY });
        view.setSelectedClipsMuted (true);
        Mouse::settle();
        const auto mutedNow = engine.session.getUndoDescription() == "Mute Clip" && view.areSelectedClipsMuted()
                              && engine.session.getState()->tracks[0].clips[0].muted;
        mouse.drag ({ muteX (24000), bodyY }, { muteX (30000), bodyY });
        const auto movedMuted = engine.session.getState()->tracks[0].clips[0].position > 0 && engine.session.getState()->tracks[0].clips[0].muted;

        const auto folder = juce::File::createTempFile ("muting");
        folder.createDirectory();
        const auto wav = folder.getChildFile ("hushed.wav");
        auto onDisk = *engine.session.getState();
        onDisk.tracks[0].clips[0].source = wav;
        SessionEdits::addClip (onDisk, 0, wav, silence (48000), 60000);
        const auto file = folder.getChildFile ("muting.sbsession");
        const auto wrote = AudioFileIO::save (*silence (48000), rate, wav, 24, engine.formatManager).isEmpty()
                           && SessionFile::save (onDisk, file).isEmpty();
        const auto loaded = SessionFile::load (file, engine.formatManager);
        folder.deleteRecursively();

        view.setSelectedClipsMuted (false);
        Mouse::settle();
        check (mutedNow && movedMuted && wrote && loaded.state.tracks[0].clips.size() == 2 && loaded.state.tracks[0].clips[0].muted
                   && ! loaded.state.tracks[0].clips[1].muted && engine.session.getUndoDescription() == "Unmute Clip"
                   && ! view.areSelectedClipsMuted() && view.getSelectedClip() == hushed,
               "clip mute: muting and unmuting from the view as undo steps, a muted clip still drags, and session files keep it");
    }

    // Clip colours: their own or the track's, chosen for the selection, and kept in session files.
    {
        SessionState painted;
        painted.sampleRate = rate;
        painted.addTrack();
        const auto first = SessionEdits::addClip (painted, 0, juce::File ("/tmp/first.wav"), silence (24000), 0);
        const auto second = SessionEdits::addClip (painted, 0, juce::File ("/tmp/second.wav"), silence (24000), 48000);
        const auto& paintedTrack = painted.tracks[0];
        const auto trackColour = TrackColours::of (paintedTrack.clips[0], paintedTrack, 0) == TrackColours::of (paintedTrack, 0);
        SessionEdits::setClipLocked (painted, first, true);
        const auto own = SessionEdits::setClipColour (painted, first, 3) && ! SessionEdits::setClipColour (painted, first, 3)
                         && TrackColours::of (painted.tracks[0].clips[0], painted.tracks[0], 0) == juce::Colour (TrackColours::palette[3].argb);
        auto outside = painted;
        SessionEdits::setClipColour (outside, first, 42);
        check (trackColour && own && outside.tracks[0].clips[0].colour == -1,
               "clip colours: a clip takes its track's colour until it has its own, locked or not; one outside the palette means the track's");

        engine.session.reset (painted, {});
        view.zoomToRange ({ 0, 96000 });
        Mouse::settle();
        const auto paintFrames = view.getVisibleFrames();
        const auto paintX = [&] (juce::int64 frame)
        {
            return (float) MultitrackView::headerWidth + (float) (((double) frame - paintFrames.getStart()) / (paintFrames.getLength() / timelineWidth));
        };

        mouse.click ({ paintX (12000), bodyY });
        mouse.click ({ paintX (60000), bodyY }, juce::ModifierKeys::shiftModifier);
        view.setSelectedClipsColour (6);
        Mouse::settle();
        const auto both = engine.session.getUndoDescription() == "Clip Colour" && engine.session.getState()->tracks[0].clips[0].colour == 6
                          && engine.session.getState()->tracks[0].clips[1].colour == 6;

        const auto folder = juce::File::createTempFile ("painting");
        folder.createDirectory();
        const auto wav = folder.getChildFile ("painted.wav");
        auto onDisk = *engine.session.getState();
        onDisk.tracks[0].clips[1].colour = -1;

        for (auto& c : onDisk.tracks[0].clips)
            c.source = wav;

        const auto file = folder.getChildFile ("painting.sbsession");
        const auto wrote = AudioFileIO::save (*silence (24000), rate, wav, 24, engine.formatManager).isEmpty()
                           && SessionFile::save (onDisk, file).isEmpty();
        const auto loaded = SessionFile::load (file, engine.formatManager);
        folder.deleteRecursively();
        check (both && second != 0 && wrote && loaded.state.tracks[0].clips.size() == 2 && loaded.state.tracks[0].clips[0].colour == 6
                   && loaded.state.tracks[0].clips[1].colour == -1,
               "clip colours: chosen for every selected clip as one undo step, and kept in session files");
    }

    // Duplicating a track from the view.
    {
        SessionState two;
        two.sampleRate = rate;
        two.addTrack().name = "Lead";
        two.addTrack().name = "Pad";
        SessionEdits::addClip (two, 0, juce::File ("/tmp/lead.wav"), silence (4800), 0);
        engine.session.reset (two, {});
        Mouse::settle();
        mouse.click ({ (float) MultitrackView::headerWidth + 500.0f, bodyY });   // an empty spot in the first lane
        const auto copyId = view.duplicateSelectedTrack();
        Mouse::settle();
        const auto st = engine.session.getState();
        check (copyId != 0 && st->tracks.size() == 3 && st->tracks[1].id == copyId && st->tracks[1].name == "Lead Copy"
                   && st->tracks[1].clips.size() == 1 && view.getSelectedTrack() == 1 && engine.session.getUndoDescription() == "Duplicate Track",
               "duplicate track: from the view as one undo step, with the copy selected");
    }

    // Hidden tracks: left out of the lanes, the rest closing up, and shown again.
    {
        SessionState three;
        three.sampleRate = rate;

        for (auto trackName : { "A", "B", "C" })
            three.addTrack().name = trackName;

        std::vector<juce::uint32> clipIds;

        for (int t = 0; t < 3; ++t)
            clipIds.push_back (SessionEdits::addClip (three, t, juce::File ("/tmp/hide.wav"), silence (96000), 0));

        engine.session.reset (three, {});
        view.zoomToRange ({ 0, 96000 });
        Mouse::settle();
        const auto hideFrames = view.getVisibleFrames();
        const auto hideX = (float) MultitrackView::headerWidth + (float) ((48000.0 - hideFrames.getStart()) / (hideFrames.getLength() / timelineWidth));
        const auto rowY = [] (int row) { return (float) (MultitrackView::rulerHeight + row * MultitrackView::trackHeight + 60); };
        const auto shownHeaders = [&]
        {
            auto count = 0;

            for (auto* area : view.getChildren())
                for (auto* child : area->getChildren())
                    count += child->getNumChildComponents() > 3 && child->isVisible() ? 1 : 0;

            return count;
        };

        mouse.click ({ hideX, rowY (1) });
        const auto bId = engine.session.getState()->tracks[1].id;
        const auto pickedB = view.getSelectedClip() == clipIds[1];
        view.setTrackHidden (bId, true);
        Mouse::settle();
        const auto hiddenStep = engine.session.getUndoDescription() == "Hide Track" && view.getSelectedClips().empty() && shownHeaders() == 2;
        mouse.click ({ hideX, rowY (1) });
        check (pickedB && hiddenStep && view.getSelectedClip() == clipIds[2] && view.getSelectedTrack() == 2,
               "hiding: a hidden track leaves the lanes and the selection, and the track below takes its row");

        view.showClip (clipIds[1]);
        Mouse::settle();
        const auto backByClip = ! engine.session.getState()->tracks[1].hidden && view.getSelectedClip() == clipIds[1] && shownHeaders() == 3;
        view.setTrackHidden (bId, true);
        view.setTrackHidden (engine.session.getState()->tracks[0].id, true);
        Mouse::settle();
        const auto allShown = view.showAllTracks() && engine.session.getUndoDescription() == "Show All Tracks" && ! view.showAllTracks();
        Mouse::settle();
        check (backByClip && allShown && shownHeaders() == 3,
               "hiding: going to a hidden track's clip shows the track again, and Show All Tracks shows them all as one undo step");

        SessionState bare;
        bare.addTrack().hidden = true;
        bare.addTrack();
        const auto file = juce::File::createTempFile (".sbsession");
        const auto error = SessionFile::save (bare, file);
        const auto loaded = SessionFile::load (file, engine.formatManager);
        file.deleteFile();
        check (error.isEmpty() && loaded.state.tracks.size() == 2 && loaded.state.tracks[0].hidden && ! loaded.state.tracks[1].hidden,
               "hiding: session files keep which tracks are hidden");
    }

    // Selecting a track's clips, or those after the cursor, never on hidden tracks.
    {
        SessionState grid;
        grid.sampleRate = rate;
        std::vector<std::vector<juce::uint32>> ids (3);

        for (int t = 0; t < 3; ++t)
        {
            grid.addTrack();

            for (juce::int64 at : { 0, 48000, 96000 })
                ids[(size_t) t].push_back (SessionEdits::addClip (grid, t, juce::File ("/tmp/grid.wav"), silence (24000), at));
        }

        grid.tracks[2].hidden = true;
        engine.session.reset (grid, {});
        view.zoomToRange ({ 0, 144000 });
        Mouse::settle();
        const auto selection = [&] { auto v = view.getSelectedClips(); std::sort (v.begin(), v.end()); return v; };
        const auto sorted = [] (std::vector<juce::uint32> v) { std::sort (v.begin(), v.end()); return v; };

        view.selectAllClips();
        const auto all = selection() == sorted ({ ids[0][0], ids[0][1], ids[0][2], ids[1][0], ids[1][1], ids[1][2] });
        mouse.click ({ (float) MultitrackView::headerWidth + 5.0f, (float) (MultitrackView::rulerHeight + MultitrackView::trackHeight + 60) });
        const auto onTrack = view.selectClipsOnTrack() && selection() == sorted ({ ids[1][0], ids[1][1], ids[1][2] });
        view.setCursor (40000);
        view.selectClipsAfterCursor();
        const auto after = selection() == sorted ({ ids[0][1], ids[0][2], ids[1][1], ids[1][2] });
        check (all && onTrack && after,
               "selecting: all clips, a track's clips and the clips after the cursor, none of them on a hidden track");

        // Clips playing any part of the time range: [20000, 50000) catches the ends of the first
        // clips and the starts of the second; a range between clips, touching both, catches none.
        view.setTimeSelection ({ 20000, 50000 });
        const auto inRange = view.selectClipsInRange() && selection() == sorted ({ ids[0][0], ids[0][1], ids[1][0], ids[1][1] });
        view.setTimeSelection ({ 24000, 48000 });
        const auto between = view.selectClipsInRange() && view.getSelectedClips().empty();
        view.selectAllClips();
        view.setTimeSelection ({});
        const auto noRange = ! view.selectClipsInRange() && view.getSelectedClips().size() == 6;
        check (inRange && between && noRange,
               "selecting: the clips that play part of the time range, not those only touching it, none on a hidden track; nothing without a range");

        // Clips in the region at the cursor: the same clips as the range test, through a region.
        auto withRegion = *engine.session.getState();
        withRegion.markers = { { 20000, 30000, "Bridge" } };
        engine.session.apply (std::move (withRegion), "Region");
        view.setCursor (30000);
        const auto inRegion = view.selectClipsInRegion() && view.getTimeSelection() == juce::Range<juce::int64> (20000, 50000)
                              && selection() == sorted ({ ids[0][0], ids[0][1], ids[1][0], ids[1][1] });
        view.setCursor (60000);
        const auto outside = ! view.selectClipsInRegion() && view.getTimeSelection() == juce::Range<juce::int64> (20000, 50000);
        check (inRegion && outside, "selecting: the region at the cursor and the clips that play part of it; nothing outside a region");

        auto twoRegions = *engine.session.getState();
        twoRegions.markers = { { 20000, 30000, "Bridge" }, { 90000, 10000, "Outro" } };
        engine.session.apply (std::move (twoRegions), "Regions");
        view.setTimeSelection ({});
        view.setCursor (0);
        const auto first = view.selectAdjacentRegion (true) && view.getTimeSelection() == juce::Range<juce::int64> (20000, 50000);
        const auto second = view.selectAdjacentRegion (true) && view.getTimeSelection() == juce::Range<juce::int64> (90000, 100000);
        const auto noThird = ! view.selectAdjacentRegion (true) && view.getTimeSelection() == juce::Range<juce::int64> (90000, 100000);
        const auto back = view.selectAdjacentRegion (false) && view.getTimeSelection() == juce::Range<juce::int64> (20000, 50000);
        check (first && second && noThird && back, "selecting: next and previous region from the cursor, then from the selected region");
    }

    // Moving the selected clips to the cursor, keeping their spacing.
    {
        SessionState spaced;
        spaced.sampleRate = rate;
        spaced.addTrack();
        spaced.addTrack();
        const auto upper = SessionEdits::addClip (spaced, 0, juce::File ("/tmp/upper.wav"), silence (8000), 10000);
        const auto lower = SessionEdits::addClip (spaced, 1, juce::File ("/tmp/lower.wav"), silence (8000), 30000);
        engine.session.reset (spaced, {});
        Mouse::settle();
        const auto at = [&] (juce::uint32 id)
        {
            const auto where = engine.session.getState()->findClip (id);
            return std::pair (where->first, engine.session.getState()->tracks[(size_t) where->first].clips[(size_t) where->second].position);
        };

        view.selectAllClips();
        view.setCursor (50000);
        const auto moved = view.moveSelectedClipsToCursor() && at (upper) == std::pair (0, (juce::int64) 50000)
                           && at (lower) == std::pair (1, (juce::int64) 70000) && engine.session.getUndoDescription() == "Move Clips to Cursor";
        const auto again = ! view.moveSelectedClipsToCursor();
        Mouse::settle();
        view.setSelectedClipsLocked (true);
        Mouse::settle();
        view.setCursor (0);
        const auto refused = ! view.moveSelectedClipsToCursor() && at (upper).second == 50000;
        check (moved && again && refused,
               "move to cursor: the selected clips move together, the earliest to the cursor, as one undo step; not when there already or locked");
    }

    // Trimming the selected clips the cursor is in to the cursor.
    {
        SessionState spans;
        spans.sampleRate = rate;
        spans.addTrack();
        spans.addTrack();
        spans.addTrack();
        const auto a = SessionEdits::addClip (spans, 0, juce::File ("/tmp/a.wav"), silence (8000), 10000);   // [10000, 18000)
        const auto b = SessionEdits::addClip (spans, 1, juce::File ("/tmp/b.wav"), silence (8000), 12000);   // [12000, 20000)
        const auto c = SessionEdits::addClip (spans, 0, juce::File ("/tmp/c.wav"), silence (8000), 30000);   // past the cursor
        const auto d = SessionEdits::addClip (spans, 2, juce::File ("/tmp/d.wav"), silence (8000), 11000);   // locked
        SessionEdits::setClipLocked (spans, d, true);
        engine.session.reset (spans, {});
        Mouse::settle();
        const auto span = [&] (juce::uint32 id)
        {
            const auto& found = clip (id);
            return std::tuple (found.position, found.getEnd(), found.offset);
        };

        view.selectAllClips();
        view.setCursor (14000);
        const auto starts = view.trimSelectedClipsToCursor (true) == 2 && span (a) == std::tuple<juce::int64, juce::int64, juce::int64> (14000, 18000, 4000)
                            && span (b) == std::tuple<juce::int64, juce::int64, juce::int64> (14000, 20000, 2000)
                            && span (c) == std::tuple<juce::int64, juce::int64, juce::int64> (30000, 38000, 0)
                            && span (d) == std::tuple<juce::int64, juce::int64, juce::int64> (11000, 19000, 0)
                            && engine.session.getUndoDescription() == "Trim Start of Clips to Cursor";
        engine.session.undo();
        Mouse::settle();

        view.selectAllClips();
        view.setCursor (16000);
        const auto ends = view.trimSelectedClipsToCursor (false) == 2 && span (a) == std::tuple<juce::int64, juce::int64, juce::int64> (10000, 16000, 0)
                          && span (b) == std::tuple<juce::int64, juce::int64, juce::int64> (12000, 16000, 0)
                          && engine.session.getUndoDescription() == "Trim End of Clips to Cursor";
        engine.session.undo();
        Mouse::settle();

        // One clip alone; at a clip's edge, or outside every clip, nothing is trimmed and no step added.
        view.showClip (a);
        view.setCursor (15000);
        const auto one = view.trimSelectedClipsToCursor (true) == 1 && engine.session.getUndoDescription() == "Trim Start to Cursor";
        engine.session.undo();
        Mouse::settle();
        view.selectAllClips();
        view.setCursor (10000);
        const auto atEdge = view.trimSelectedClipsToCursor (true) == 0 && ! engine.session.canUndo();
        view.setCursor (25000);
        const auto outside = view.trimSelectedClipsToCursor (false) == 0 && ! engine.session.canUndo();
        check (starts && ends && one && atEdge && outside,
               "trim to cursor: the selected clips the cursor is in start or end there, as one undo step; not locked clips, nor clips it isn't inside");
    }

    // Duplicating the time range: a copy right after it, moving what follows on.
    {
        SessionState piece;
        piece.sampleRate = rate;
        piece.addTrack();
        piece.addTrack();
        SessionEdits::addClip (piece, 0, juce::File ("/tmp/hook.wav"), silence (8000), 10000);   // [10000, 18000)
        const auto later = SessionEdits::addClip (piece, 1, juce::File ("/tmp/later.wav"), silence (4000), 30000);
        piece.markers = { { 12000, 0, "Hook" } };
        engine.session.reset (piece, {});
        Mouse::settle();
        const auto pasteBefore = view.canPasteTime();
        const auto clipStarts = [&] (int lane)
        {
            std::vector<juce::int64> v;

            for (auto& c : engine.session.getState()->tracks[(size_t) lane].clips)
                v.push_back (c.position);

            std::sort (v.begin(), v.end());
            return v;
        };

        view.setTimeSelection ({ 8000, 20000 });
        const auto once = view.duplicateTimeSelection() && clipStarts (0) == std::vector<juce::int64> { 10000, 22000 }
                          && clip (later).position == 42000 && engine.session.getState()->markers.size() == 2
                          && engine.session.getState()->markers[1].start == 24000 && engine.session.getState()->markers[1].name == "Hook"
                          && view.getTimeSelection() == juce::Range<juce::int64> (20000, 32000) && view.getCursor() == 20000
                          && engine.session.getUndoDescription() == "Duplicate Time" && view.canPasteTime() == pasteBefore;
        const auto twice = view.duplicateTimeSelection() && clipStarts (0) == std::vector<juce::int64> { 10000, 22000, 34000 }
                           && clip (later).position == 54000;
        engine.session.undo();
        engine.session.undo();
        view.setTimeSelection ({});
        const auto without = ! view.duplicateTimeSelection() && clipStarts (0) == std::vector<juce::int64> { 10000 } && clip (later).position == 30000;
        check (once && twice && without,
               "duplicate time: the range's clips and markers repeated right after it, what follows moved on, the copy selected for the next; the copied time untouched");
    }

    // Moving the track selection up and down, past hidden tracks, the lane and its header scrolled into view.
    {
        SessionState many;
        many.sampleRate = rate;

        for (int i = 0; i < 6; ++i)
            many.addTrack();

        many.tracks[3].hidden = true;
        engine.session.reset (many, {});
        MultitrackView tall (engine);
        tall.setSize (MultitrackView::headerWidth + timelineWidth + 12, MultitrackView::rulerHeight + 3 * MultitrackView::trackHeight + 12);
        tall.setVisible (true);
        Mouse::settle();

        std::vector<int> visited;

        while (tall.selectAdjacentTrack (true))
            visited.push_back (tall.getSelectedTrack());

        // The header at the top of the lanes is now the last track's: clicking it keeps that track.
        const auto top = juce::Point<int> (40, MultitrackView::rulerHeight + 10);
        auto* header = tall.getComponentAt (top);
        auto headerOk = false;

        if (header != nullptr && header != &tall)
        {
            const auto local = header->getLocalPoint (&tall, top).toFloat();
            const auto now = juce::Time::getCurrentTime();
            header->mouseDown ({ juce::Desktop::getInstance().getMainMouseSource(), local, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                                 juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                                 juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                                 juce::MouseInputSource::defaultTiltY, header, header, now, local, now, 1, false });
            headerOk = tall.getSelectedTrack() == 5;
        }

        std::vector<int> back;

        while (tall.selectAdjacentTrack (false))
            back.push_back (tall.getSelectedTrack());

        check (visited == std::vector<int> { 0, 1, 2, 4, 5 } && headerOk && back == std::vector<int> { 4, 2, 1, 0 },
               "track selection: down and up through the shown tracks, skipping a hidden one, its lane and header scrolled into view together");
    }

    // Clearing every solo or mute at once.
    {
        SessionState loud;
        loud.sampleRate = rate;
        loud.addTrack().solo = true;
        loud.addTrack().mute = true;
        loud.addBus().solo = true;
        auto& folder = loud.addFolder();
        folder.mute = true;
        folder.solo = true;
        engine.session.reset (loud, {});
        const auto tracks = [&] () -> const std::vector<SessionTrack>& { return engine.session.getState()->tracks; };

        const auto unsoloed = view.clearAllSolos() && ! engine.session.getState()->anySolo() && tracks()[1].mute && tracks()[3].mute
                              && engine.session.getUndoDescription() == "Unsolo All";
        const auto unmuted = view.clearAllMutes() && ! tracks()[1].mute && ! tracks()[3].mute && engine.session.getUndoDescription() == "Unmute All";
        const auto nothingLeft = ! view.clearAllSolos() && ! view.clearAllMutes() && engine.session.getUndoDescription() == "Unmute All";
        engine.session.undo();
        engine.session.undo();
        const auto back = tracks()[0].solo && tracks()[1].mute && tracks()[2].solo && tracks()[3].mute && tracks()[3].solo;
        check (unsoloed && unmuted && nothingLeft && back,
               "unsolo and unmute all: every track's, bus's and folder's solo or mute cleared as one undo step each; nothing when none is set");
    }

    // A region from the selected clips.
    {
        SessionState spread;
        spread.sampleRate = rate;
        spread.addTrack();
        spread.addTrack();
        const auto early = SessionEdits::addClip (spread, 0, juce::File ("/tmp/intro.wav"), silence (8000), 10000);
        SessionEdits::addClip (spread, 1, juce::File ("/tmp/pad.wav"), silence (30000), 14000);   // ends last, at 44000
        spread.markers = { { 5000, 0, "Count" } };
        engine.session.reset (spread, {});
        Mouse::settle();

        view.selectAllClips();
        const auto both = view.addRegionFromSelectedClips() && engine.session.getState()->markers.size() == 2
                          && engine.session.getState()->markers[1] == Marker { 10000, 34000, "Region 01" }
                          && engine.session.getUndoDescription() == "Add Region 01";
        view.showClip (early);
        const auto one = view.addRegionFromSelectedClips() && engine.session.getState()->markers.size() == 3
                         && engine.session.getState()->markers[1] == Marker { 10000, 8000, "intro" };
        view.setTimeSelection ({ 60000, 70000 });
        view.selectClipsInRange();
        view.setTimeSelection ({});
        const auto none = ! view.addRegionFromSelectedClips() && engine.session.getState()->markers.size() == 3;
        check (both && one && none,
               "region from clips: spans the selected clips, named after the clip when there is one, as an undo step; nothing with none selected");
    }

    // Selecting the next and previous clip along a track.
    {
        SessionState row;
        row.sampleRate = rate;
        row.addTrack();
        row.addTrack();
        const auto middle = SessionEdits::addClip (row, 0, juce::File ("/tmp/m.wav"), silence (4000), 30000);
        const auto first = SessionEdits::addClip (row, 0, juce::File ("/tmp/f.wav"), silence (4000), 10000);
        const auto last = SessionEdits::addClip (row, 0, juce::File ("/tmp/l.wav"), silence (4000), 50000);
        SessionEdits::addClip (row, 1, juce::File ("/tmp/o.wav"), silence (4000), 20000);
        engine.session.reset (row, {});
        Mouse::settle();

        view.showClip (first);
        std::vector<juce::uint32> visited;

        while (view.selectAdjacentClip (true))
            visited.push_back (view.getSelectedClip());

        const auto back = view.selectAdjacentClip (false) && view.getSelectedClip() == middle && view.getCursor() == 30000
                          && view.getSelectedClips().size() == 1;
        check (visited == std::vector<juce::uint32> { middle, last } && back,
               "next clip: steps along the selected clip's track in time order, stopping at the last; previous steps back, cursor at its start");

        // With no clip selected: from the cursor on the selected track.
        const auto none = [&]
        {
            view.setTimeSelection ({ 60000, 70000 });
            view.selectClipsInRange();
            view.setTimeSelection ({});
        };
        none();
        view.setCursor (25000);
        const auto after = view.getSelectedClip() == 0 && view.selectAdjacentClip (true) && view.getSelectedClip() == middle;
        none();
        view.setCursor (25000);
        const auto before = view.selectAdjacentClip (false) && view.getSelectedClip() == first;
        none();
        view.setCursor (60000);
        const auto nothing = ! view.selectAdjacentClip (true) && view.getSelectedClip() == 0;
        check (after && before && nothing,
               "next clip: with none selected, the first clip at or after the cursor on the selected track, or the last before it; nothing past the end");
    }

    // Closing the gaps on a track: in time order, around a locked clip, leaving overlaps.
    {
        SessionState gappy;
        gappy.sampleRate = rate;
        gappy.addTrack();
        gappy.addTrack();
        // Added out of order: 1000-2000, 5000-6000 (locked), 3000-4000, 8000-9000 overlapping 8500-9500.
        const auto b = SessionEdits::addClip (gappy, 0, juce::File ("/tmp/b.wav"), silence (1000), 5000);
        const auto a = SessionEdits::addClip (gappy, 0, juce::File ("/tmp/a.wav"), silence (1000), 1000);
        const auto c = SessionEdits::addClip (gappy, 0, juce::File ("/tmp/c.wav"), silence (1000), 8000);
        const auto d = SessionEdits::addClip (gappy, 0, juce::File ("/tmp/d.wav"), silence (1000), 8500);
        const auto e = SessionEdits::addClip (gappy, 0, juce::File ("/tmp/e.wav"), silence (1000), 3000);
        const auto other = SessionEdits::addClip (gappy, 1, juce::File ("/tmp/other.wav"), silence (1000), 7000);
        SessionEdits::setClipLocked (gappy, b, true);
        auto closed = gappy;
        const auto moved = SessionEdits::closeGaps (closed, 0);
        const auto pos = [&closed] (juce::uint32 id)
        {
            const auto where = closed.findClip (id);
            return closed.tracks[(size_t) where->first].clips[(size_t) where->second].position;
        };
        check (moved == 3 && pos (a) == 1000 && pos (e) == 2000 && pos (b) == 5000 && pos (c) == 6000 && pos (d) == 6500 && pos (other) == 7000
                   && SessionEdits::closeGaps (closed, 0) == 0 && SessionEdits::closeGaps (closed, 7) == 0,
               "close gaps: clips close up in time order from the first, around a locked one, keeping an overlap, other tracks untouched");

        engine.session.reset (gappy, {});
        Mouse::settle();
        mouse.click ({ (float) MultitrackView::headerWidth + 900.0f, bodyY });   // the first lane, past its clips
        const auto viaView = view.closeGapsOnSelectedTrack() && engine.session.getUndoDescription() == "Close Gaps"
                             && ! view.closeGapsOnSelectedTrack();
        check (viaView, "close gaps: on the selected track as one undo step, and nothing the second time");
    }

    // Zero crossings: a square wave changing sign every 50 frames, and a channel that keeps the
    // sum from ever crossing.
    {
        juce::AudioBuffer<float> square (2, 1000);

        for (int i = 0; i < 1000; ++i)
        {
            square.setSample (0, i, (i / 50) % 2 == 0 ? 0.5f : -0.5f);
            square.setSample (1, i, 0.0f);
        }

        auto lifted = square;

        for (int i = 0; i < 1000; ++i)
            lifted.setSample (1, i, 0.6f);

        SessionClip squareClip;
        squareClip.audio = AudioEdits::fromBuffer (std::move (square));
        squareClip.position = 1000;
        squareClip.offset = 100;   // session frame 1000 plays audio frame 100
        squareClip.length = 800;
        auto steady = squareClip;
        steady.audio = AudioEdits::fromBuffer (std::move (lifted));

        // Audio sign changes at 150, 200, 250...: session frames 1050, 1100, 1150.
        check (SessionEdits::zeroCrossingNear (squareClip, 1062, 240) == 1050 && SessionEdits::zeroCrossingNear (squareClip, 1080, 240) == 1100
                   && SessionEdits::zeroCrossingNear (squareClip, 1062, 5) == 1062 && SessionEdits::zeroCrossingNear (steady, 1062, 240) == 1062
                   && SessionEdits::zeroCrossingNear (squareClip, 905, 10) == 905,
               "zero crossings: the nearest one within reach, by the channels' sum, inside the audio, else the frame itself");

        SessionState crossing;
        crossing.sampleRate = rate;
        crossing.addTrack();
        squareClip.id = crossing.nextId++;
        crossing.tracks[0].clips.push_back (squareClip);
        engine.session.reset (crossing, {});
        view.zoomToRange ({ 900, 1900 });
        Mouse::settle();
        const auto zeroFrames = view.getVisibleFrames();
        mouse.click ({ (float) MultitrackView::headerWidth + (float) ((1300.0 - zeroFrames.getStart()) / (zeroFrames.getLength() / timelineWidth)), bodyY });
        view.setCursor (1062);
        view.setFindingZeroCrossings (true);
        view.splitSelectedClip();
        Mouse::settle();
        const auto atCrossing = engine.session.getState()->tracks[0].clips.size() == 2 && engine.session.getState()->tracks[0].clips[0].getEnd() == 1050;
        engine.session.undo();
        Mouse::settle();
        view.setFindingZeroCrossings (false);
        view.splitSelectedClip();
        Mouse::settle();
        check (atCrossing && engine.session.getState()->tracks[0].clips[0].getEnd() == 1062,
               "zero crossings: with the option on a split lands on the nearest crossing, without it at the cursor");
    }

    {
        // The overview strip spans the session (at least a minute) and scrolls the lanes, keeping the zoom.
        SessionState overviewed;
        overviewed.sampleRate = rate;
        overviewed.addTrack();
        SessionEdits::addClip (overviewed, 0, juce::File ("/tmp/o.wav"), silence (480000), 0);
        engine.session.reset (overviewed, {});
        view.zoomToRange ({ 0, 96000 });
        Mouse::settle();
        SessionOverview overview (engine, view);
        overview.setSize (600, SessionOverview::height);
        const auto span = view.getVisibleFrames().getLength();
        const auto press = [&] (float px, bool dragged)
        {
            const auto now = juce::Time::getCurrentTime();
            const juce::Point<float> at { px, 10.0f };
            const juce::MouseEvent e { juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys::leftButtonModifier,
                                       juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                                       juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                                       juce::MouseInputSource::defaultTiltY, &overview, &overview, now, at, now, 1, dragged };
            dragged ? overview.mouseDrag (e) : overview.mouseDown (e);
        };

        check (juce::exactlyEqual (overview.getTotalFrames(), rate * 60.0) && overview.frameAt (300.0f) == (juce::int64) (rate * 30.0)
                   && std::abs (overview.xFor (rate * 15.0) - 150.0f) < 0.001f,
               "overview: a ten-second session spans the one-minute floor across the strip");
        press (300.0f, false);
        const auto clicked = view.getVisibleFrames();
        press (590.0f, true);
        const auto dragged = view.getVisibleFrames();
        press (0.0f, true);
        const auto atStart = view.getVisibleFrames();
        check (std::abs (clicked.getStart() - (rate * 30.0 - span / 2.0)) < 1.0 && std::abs (clicked.getLength() - span) < 1.0e-6
                   && std::abs (dragged.getStart() - (rate * 59.0 - span / 2.0)) < 1.0 && juce::exactlyEqual (atStart.getStart(), 0.0),
               "overview: clicking centres the lanes there, dragging follows, the start stops at zero, the zoom stays");

        // A software image: Direct2D's, on Windows, holds what is drawn until the Graphics goes.
        juce::Image strip (juce::Image::ARGB, 600, SessionOverview::height, true, juce::SoftwareImageType());
        juce::Graphics g (strip);
        overview.paint (g);
        check (strip.getPixelAt (30, 4) != strip.getPixelAt (300, 4),
               "overview: the clip is drawn where it lies in the session (" + strip.getPixelAt (30, 4).toDisplayString (true) + " on it, "
                   + strip.getPixelAt (300, 4).toDisplayString (true) + " past it; "
                   + juce::String ((int) engine.session.getState()->tracks.size()) + " tracks)");
    }

    {
        // Time edits on the range, from the view: one undo step each.
        SessionState timed;
        timed.sampleRate = rate;
        timed.addTrack();
        timed.addTrack();
        SessionEdits::addClip (timed, 0, juce::File ("/tmp/t1.wav"), silence (96000), 0);
        SessionEdits::addClip (timed, 1, juce::File ("/tmp/t2.wav"), silence (96000), 48000);
        engine.session.reset (timed, {});
        Mouse::settle();
        view.setTimeSelection ({});
        const auto noRange = ! view.clearTimeSelection() && ! view.deleteTimeSelection() && ! view.insertTimeAtSelection();
        view.setTimeSelection ({ 24000, 72000 });
        const auto inserted = view.insertTimeAtSelection();
        const auto insertStep = engine.session.getUndoDescription();
        const auto insertedLength = engine.session.getState()->getLength();
        const auto rangeKept = view.getTimeSelection() == juce::Range<juce::int64> (24000, 72000);
        const auto deleted = view.deleteTimeSelection();
        const auto deleteStep = engine.session.getUndoDescription();
        check (noRange && inserted && insertStep == "Insert Time" && insertedLength == 144000 + 48000 && rangeKept
                   && deleted && deleteStep == "Delete Time" && engine.session.getState()->getLength() == 144000
                   && view.getTimeSelection().isEmpty() && view.getCursor() == 24000,
               "time edits: none without a range; insert time keeps the range over the new time, delete time leaves the cursor at its start");
        view.setTimeSelection ({ 24000, 72000 });
        const auto clearedRange = view.clearTimeSelection();
        check (clearedRange && engine.session.getUndoDescription() == "Clear Range" && engine.session.getState()->tracks[0].clips.size() == 2
                   && engine.session.getState()->getLength() == 144000,
               "time edits: clear range cuts every track across the range, as one step");

        engine.session.reset (timed, {});
        Mouse::settle();
        view.setTimeSelection ({ 24000, 72000 });
        const auto cutDone = view.cutTimeSelection();
        const auto cutStep = engine.session.getUndoDescription();
        const auto afterCut = engine.session.getState()->getLength();
        view.setCursor (0);
        const auto pasteDone = view.pasteTime();
        check (cutDone && cutStep == "Cut Time" && afterCut == 96000 && view.canPasteTime() && pasteDone
                   && engine.session.getUndoDescription() == "Paste Time" && engine.session.getState()->getLength() == 144000
                   && view.getTimeSelection() == juce::Range<juce::int64> (0, 48000) && view.getCursor() == 48000,
               "time edits: cut time takes the range out, paste time puts it in at the cursor, selected, with the cursor after it");

        // On the beat grid, nudging steps follow a tempo change: 90 a minute in threes from bar 2.
        auto changed = timed;
        SessionEdits::setTempoChange (changed, { 2, 90.0, 3 });
        engine.session.reset (changed, {});
        view.zoomToRange ({ 0, 480000 });
        view.setShowBars (true);
        Mouse::settle();
        const auto steps = std::vector<juce::int64> { view.gridStepFrom (300000, true), view.gridStepFrom (300000, false),
                                                      view.gridStepFrom (288000, false), view.gridStepFrom (192000, false) };
        juce::Image ruler (juce::Image::ARGB, view.getWidth(), view.getHeight(), true);
        {
            juce::Graphics g (ruler);
            view.paintEntireComponent (g, false);
        }
        const auto changeX = (int) std::lround ((double) MultitrackView::headerWidth + (192000.0 - view.getVisibleFrames().getStart())
                                                / (view.getVisibleFrames().getLength() / timelineWidth));
        auto marked = false;

        for (auto px = changeX - 1; px <= changeX + 1; ++px)
            marked = marked || (ruler.getPixelAt (px, 2).getRed() > 200 && ruler.getPixelAt (px, 2).getBlue() < 140);

        view.setShowBars (false);
        check (steps == std::vector<juce::int64> { 320000, 288000, 256000, 168000 } && marked,
               "tempo map: nudging steps by the beats either side of a tempo change, and the ruler marks the change");

        // Quantizing: to the ruler's marks (two seconds apart here), or in bars and beats to the beats.
        SessionState loose;
        loose.sampleRate = rate;
        loose.addTrack();
        loose.addTrack();
        const auto early = SessionEdits::addClip (loose, 0, juce::File ("/tmp/q1.wav"), silence (10000), 30000);
        const auto middle = SessionEdits::addClip (loose, 1, juce::File ("/tmp/q2.wav"), silence (10000), 150000);
        const auto fixed = SessionEdits::addClip (loose, 0, juce::File ("/tmp/q3.wav"), silence (10000), 250000);
        SessionEdits::setClipLocked (loose, fixed, true);
        const auto startOf = [&] (juce::uint32 id) { return clip (id).position; };
        engine.session.reset (loose, {});
        view.zoomToRange ({ 0, 480000 });
        Mouse::settle();
        view.selectAllClips();
        const auto toSeconds = view.quantizeSelectedClips();
        Mouse::settle();
        const auto secondsStep = engine.session.getUndoDescription();
        const auto onSeconds = startOf (early) == 0 && startOf (middle) == 192000 && startOf (fixed) == 250000;
        engine.session.undo();
        view.setShowBars (true);
        Mouse::settle();
        const auto toBeats = view.quantizeSelectedClips();
        Mouse::settle();
        const auto onBeats = startOf (early) == 24000 && startOf (middle) == 144000 && startOf (fixed) == 250000;
        const auto again = view.quantizeSelectedClips();
        view.setSelectedClipsFadeShape (true, FadeShape::linear);
        Mouse::settle();
        const auto shaped = engine.session.getUndoDescription() == "Fade Out Shape" && clip (early).fadeOutShape == FadeShape::linear
                            && clip (middle).fadeOutShape == FadeShape::linear && clip (fixed).fadeOutShape == FadeShape::sCurve
                            && clip (early).fadeInShape == FadeShape::sCurve;
        view.setShowBars (false);
        check (toSeconds == 2 && secondsStep == "Quantize 2 Clips" && onSeconds && toBeats == 2 && onBeats && again == 0,
               "quantize: selected clips start on the nearest mark, of seconds or of beats; a locked clip stays; nothing left to move the second time");
        check (shaped, "fade shapes: set on the selected clips as one undo step, not on a locked one");

        // Dropped files land on the grid when snapping is on.
        juce::int64 droppedAt = -1;
        view.onFilesDropped = [&droppedAt] (const juce::StringArray&, int, juce::int64 frame) { droppedAt = frame; };
        const auto shownFrames = view.getVisibleFrames();
        const auto dropX = (int) std::lround ((double) MultitrackView::headerWidth + (96000.0 + 900.0 - shownFrames.getStart()) / (shownFrames.getLength() / timelineWidth));
        view.setSnapping (true);
        view.filesDropped ({ "/tmp/drop.wav" }, dropX, (int) bodyY);
        const auto snappedDrop = droppedAt;
        view.setSnapping (false);
        view.filesDropped ({ "/tmp/drop.wav" }, dropX, (int) bodyY);
        view.onFilesDropped = nullptr;
        view.setSnapping (true);
        check (snappedDrop == 96000 && droppedAt != 96000 && std::abs (droppedAt - 96900) < 600,
               "dropping files: with snapping on they land on the grid, without it where they are dropped");
    }

    {
        // Grouped clips: a click on one selects the group, and dragging it moves them all.
        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        s.addTrack();
        const auto a = SessionEdits::addClip (s, 0, juce::File ("/tmp/a.wav"), silence (48000), 0);
        const auto b = SessionEdits::addClip (s, 0, juce::File ("/tmp/b.wav"), silence (48000), 240000);
        const auto c = SessionEdits::addClip (s, 1, juce::File ("/tmp/c.wav"), silence (48000), 96000);
        engine.session.reset (s, {});
        view.zoomToFit();
        view.setSnapping (false);
        Mouse::settle();
        const auto shown = view.getVisibleFrames();
        const auto at = [&] (juce::int64 frame) { return (float) ((double) MultitrackView::headerWidth + ((double) frame - shown.getStart()) / (shown.getLength() / timelineWidth)); };

        mouse.click ({ at (24000), bodyY });
        mouse.click ({ at (264000), bodyY }, juce::ModifierKeys::shiftModifier);
        const auto couldGroup = view.canGroupSelectedClips();
        view.groupSelectedClips();
        Mouse::settle();
        const auto grouped = engine.session.getUndoDescription() == "Group Clips" && ! view.canGroupSelectedClips() && view.areSelectedClipsGrouped();

        auto withC = *engine.session.getState();
        SessionEdits::groupClips (withC, { a, b, c });
        engine.session.apply (withC, "Group Clips");
        Mouse::settle();
        mouse.click ({ at (150000), bodyY });   // empty lane: nothing selected
        const auto cleared = view.getSelectedClips().empty();
        mouse.click ({ at (24000), bodyY });
        const auto selected = view.getSelectedClips();
        const auto wholeGroup = std::set<juce::uint32> (selected.begin(), selected.end()) == std::set<juce::uint32> { a, b, c };

        mouse.drag ({ at (24000), bodyY }, { at (24000 + 48000), bodyY });
        Mouse::settle();
        const auto position = [&] (juce::uint32 id)
        {
            const auto& st = *engine.session.getState();
            const auto where = st.findClip (id);
            return st.tracks[(size_t) where->first].clips[(size_t) where->second].position;
        };
        const auto moved = position (a);
        const auto together = moved > 40000 && moved < 56000 && position (b) == 240000 + moved && position (c) == 96000 + moved
                              && engine.session.getState()->findClip (c)->first == 1;

        mouse.click ({ at (264000 + moved), bodyY }, juce::ModifierKeys::shiftModifier);   // shift-click: the whole group out
        const auto deselected = view.getSelectedClips().empty();
        mouse.click ({ at (24000 + moved), bodyY });
        view.ungroupSelectedClips();
        Mouse::settle();
        const auto ungroupStep = engine.session.getUndoDescription() == "Ungroup Clips";
        mouse.click ({ at (150000 + moved), bodyY });
        mouse.click ({ at (24000 + moved), bodyY });
        const auto alone = view.getSelectedClips() == std::vector<juce::uint32> { a };
        view.setSnapping (true);

        check (couldGroup && grouped && cleared && wholeGroup && deselected && ungroupStep && alone,
               "clip groups: grouping as an undo step, a click on one clip selecting its whole group across tracks, shift-click taking it out, and ungrouping");
        check (together, "clip groups: dragging one clip moves its whole group by the same amount, each on its own track (moved "
                             + juce::String (moved) + " frames)");
    }

    {
        // Clip gain lines: shown, a click on a clip's line adds a point at the line's value, a
        // drag moves it, Alt-click removes it; hidden, a click on the line selects the clip.
        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        const auto a = SessionEdits::addClip (s, 0, juce::File ("/tmp/a.wav"), silence (240000), 48000);
        engine.session.reset (s, {});

        // A view of its own, with no track envelopes shown from earlier.
        MultitrackView gainView (engine);
        gainView.setSize (MultitrackView::headerWidth + timelineWidth + 12, MultitrackView::rulerHeight + 3 * MultitrackView::trackHeight + 12);
        gainView.zoomToFit();
        gainView.setSnapping (false);
        Mouse gainMouse { gainView };
        Mouse::settle();
        const auto shown = gainView.getVisibleFrames();
        const auto at = [&] (juce::int64 frame) { return (float) ((double) MultitrackView::headerWidth + ((double) frame - shown.getStart()) / (shown.getLength() / timelineWidth)); };
        const auto linePoints = [&] { return engine.session.getState()->tracks[0].clips[0].gainEnvelope.points; };

        // 0 dB is half way down the body, below the 16-pixel title bar and 4 pixels in.
        const auto top = (float) MultitrackView::rulerHeight + 3.0f + 17.0f + 4.0f, bottom = (float) (MultitrackView::rulerHeight + MultitrackView::trackHeight) - 3.0f - 4.0f;
        const auto zeroY = (top + bottom) / 2.0f;
        gainMouse.click ({ at (96000), zeroY });
        const auto hiddenDoesNothing = linePoints().empty() && gainView.getSelectedClips() == std::vector<juce::uint32> { a };

        gainView.setShowClipGain (true);
        Mouse::settle();
        gainMouse.click ({ at (96000), zeroY });
        const auto added = linePoints().size() == 1 && std::abs (linePoints()[0].frame - 48000) < 600 && std::abs (linePoints()[0].value) < 0.5f
                           && engine.session.getUndoDescription() == "Add Clip Gain Point";
        const auto pointAt = linePoints().empty() ? (juce::int64) 48000 : linePoints()[0].frame;   // of the clip's audio
        gainMouse.drag ({ at (pointAt + 48000), zeroY }, { at (192000), bottom });
        const auto dragged = linePoints().size() == 1 && std::abs (linePoints()[0].frame - 144000) < 600
                             && juce::exactlyEqual (linePoints()[0].value, SessionClip::minGainDb)
                             && engine.session.getUndoDescription() == "Move Clip Gain Point";
        gainMouse.click ({ at ((linePoints().empty() ? (juce::int64) 144000 : linePoints()[0].frame) + 48000), bottom }, juce::ModifierKeys::altModifier);
        const auto removed = linePoints().empty() && engine.session.getUndoDescription() == "Delete Clip Gain Point";

        check (hiddenDoesNothing && added && dragged && removed,
               "clip gain line: with the lines shown, clicking a clip's line adds a point, dragging moves it (by its clip's audio frames), Alt-click removes it");
    }

    {
        // Folders: Add Folder puts the selected track in a new folder above it; collapsed, the
        // folder's members leave the lanes and the selection, and going to one's clip opens it.
        SessionState three;
        three.sampleRate = rate;

        for (auto trackName : { "A", "B", "C" })
            three.addTrack().name = trackName;

        std::vector<juce::uint32> clipIds;

        for (int t = 0; t < 3; ++t)
            clipIds.push_back (SessionEdits::addClip (three, t, juce::File ("/tmp/folder.wav"), silence (96000), 0));

        engine.session.reset (three, {});
        MultitrackView folderView (engine);
        folderView.setSize (MultitrackView::headerWidth + timelineWidth + 12, MultitrackView::rulerHeight + 5 * MultitrackView::trackHeight + 12);
        folderView.setVisible (true);   // so a point finds the header under it
        folderView.zoomToRange ({ 0, 96000 });
        Mouse folderMouse { folderView };
        Mouse::settle();
        const auto frames = folderView.getVisibleFrames();
        const auto clipX = (float) MultitrackView::headerWidth + (float) ((48000.0 - frames.getStart()) / (frames.getLength() / timelineWidth));
        const auto rowY = [] (int row) { return (float) (MultitrackView::rulerHeight + row * MultitrackView::trackHeight + 60); };
        const auto shownHeaders = [&]
        {
            auto count = 0;

            for (auto* area : folderView.getChildren())
                for (auto* child : area->getChildren())
                    count += child->getNumChildComponents() > 3 && child->isVisible() ? 1 : 0;

            return count;
        };

        folderMouse.click ({ clipX, rowY (1) });
        const auto folderId = folderView.addFolder();
        Mouse::settle();
        const auto st = [&]() -> const SessionState& { return *engine.session.getState(); };
        const auto added = engine.session.getUndoDescription() == "Add Folder" && st().tracks.size() == 4 && st().tracks[1].id == folderId
                           && st().tracks[1].isFolder && st().tracks[1].name == "Folder 1" && st().tracks[2].name == "B"
                           && st().tracks[2].folderId == folderId && st().tracks[0].folderId == 0 && shownHeaders() == 4;

        folderMouse.click ({ clipX, rowY (2) });
        const auto pickedB = folderView.getSelectedClip() == clipIds[1];

        // A click on the folder header's triangle collapses it.
        auto* header = folderView.getComponentAt (juce::Point<int> (14, MultitrackView::rulerHeight + MultitrackView::trackHeight + 18));
        const auto now = juce::Time::getCurrentTime();
        const juce::MouseEvent press { juce::Desktop::getInstance().getMainMouseSource(), { 14.0f, 18.0f }, juce::ModifierKeys::leftButtonModifier,
                                       juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                                       juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, header, header, now, { 14.0f, 18.0f }, now, 1, false };

        if (header != nullptr)
            header->mouseDown (press);

        Mouse::settle();
        const auto collapsed = st().tracks[1].collapsed && engine.session.getUndoDescription() == "Collapse Folder" && shownHeaders() == 3
                               && folderView.getSelectedClips().empty();
        folderMouse.click ({ clipX, rowY (2) });
        const auto cTakesRow = folderView.getSelectedClip() == clipIds[2];

        folderView.showClip (clipIds[1]);
        Mouse::settle();
        const auto opened = ! st().tracks[1].collapsed && shownHeaders() == 4 && folderView.getSelectedClip() == clipIds[1];

        folderView.setTrackFolder (st().tracks[3].id, folderId);
        const auto moved = engine.session.getUndoDescription() == "Move to Folder" && st().tracks[3].folderId == folderId;
        folderView.setTrackFolder (st().tracks[3].id, 0);
        const auto out = engine.session.getUndoDescription() == "Take Out of Folder" && st().tracks[3].folderId == 0;

        check (added && pickedB, "folders: Add Folder puts the selected track in a new folder just above it, as an undo step");
        check (header != nullptr && collapsed && cTakesRow && opened,
               "folders: the triangle collapses a folder, its tracks leaving the lanes and the selection; going to one's clip expands it again");
        check (moved && out, "folders: putting a track in a folder and taking it out are undo steps");

        // Dropped on a folder's header, a track goes in it, just below it, from below or above.
        const auto aId = st().tracks[0].id, cId = st().tracks[3].id;
        const auto intoFolder = folderView.dropsIntoFolder (3, 1) && ! folderView.dropsIntoFolder (1, 3) && ! folderView.dropsIntoFolder (3, 2);
        folderView.dropTrack (3, 1);
        Mouse::settle();
        const auto fromBelow = engine.session.getUndoDescription() == "Move to Folder" && st().tracks[2].id == cId && st().tracks[2].folderId == folderId
                               && st().tracks[3].name == "B";
        folderView.dropTrack (0, 1);
        Mouse::settle();
        const auto fromAbove = st().tracks[0].id == folderId && st().tracks[1].id == aId && st().tracks[1].folderId == folderId
                               && folderView.getSelectedTrack() == 1;
        folderView.dropTrack (3, 2);
        Mouse::settle();
        const auto plainMove = engine.session.getUndoDescription() == "Move Track" && st().tracks[2].name == "B" && st().tracks[2].folderId == folderId;
        check (intoFolder && fromBelow && fromAbove && plainMove,
               "folders: a header dropped on a folder's puts its track in, just below the folder, as an undo step; dropped elsewhere it just moves");
    }

    {
        // Header meters: a track's level into the mix, read without taking the mixer's; an armed
        // track's input instead; none for a folder.
        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        s.addTrack();
        s.addFolder();
        engine.session.reset (s, {});
        MultitrackView meterView (engine);
        meterView.setSize (MultitrackView::headerWidth + timelineWidth + 12, MultitrackView::rulerHeight + 4 * MultitrackView::trackHeight + 12);
        Mouse::settle();
        const auto& t = engine.session.getState()->tracks;
        auto& levels = engine.getPlayer().getLevels();

        for (int i = 0; i < 3; ++i)
        {
            levels.take (i, 0);
            levels.take (i, 1);
        }

        levels.raise (1, 0.5f, 0.25f);
        levels.raise (2, 0.9f, 0.9f);
        meterView.updateHeaderMeters();
        const auto shown = juce::exactlyEqual (meterView.getHeaderLevel (t[1].id, 0), 0.5f) && juce::exactlyEqual (meterView.getHeaderLevel (t[1].id, 1), 0.25f)
                           && juce::exactlyEqual (meterView.getHeaderLevel (t[0].id, 0), 0.0f) && juce::exactlyEqual (meterView.getHeaderLevel (t[2].id, 0), 0.0f)
                           && ! meterView.isHeaderShowingInput (t[1].id);
        const auto mixerStill = levels.take (1, 0) == std::pair (0.5f, 0.25f);
        meterView.updateHeaderMeters();
        const auto falls = std::abs (meterView.getHeaderLevel (t[1].id, 0) - 0.5f * 0.63f) < 1.0e-6f;

        meterView.setArmed (t[0].id, true);
        meterView.updateHeaderMeters();
        const auto input = meterView.isHeaderShowingInput (t[0].id) && ! meterView.isHeaderShowingInput (t[1].id);
        meterView.setArmed (t[0].id, false);
        levels.take (2, 0);

        check (shown && mixerStill && falls && input,
               "header meters: a track's level into the mix, the mixer still getting its own, falling back; an armed track's input; nothing for a folder");
    }

    {
        // Header meters' peak hold (1.5 s at 30 ticks a second, then falling) and clip light (on
        // at 0 dB until the meter is clicked).
        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        s.addTrack();
        engine.session.reset (s, {});
        MultitrackView holdView (engine);
        holdView.setSize (MultitrackView::headerWidth + timelineWidth + 12, MultitrackView::rulerHeight + 3 * MultitrackView::trackHeight + 12);
        holdView.setVisible (true);
        Mouse::settle();
        const auto first = engine.session.getState()->tracks[0].id, second = engine.session.getState()->tracks[1].id;
        auto& levels = engine.getPlayer().getLevels();

        for (int i = 0; i < 2; ++i)
            levels.take (i, 1);

        const auto tick = [&] (float left, float right)
        {
            levels.raise (0, left, right);
            holdView.updateHeaderMeters();
        };

        tick (0.5f, 0.25f);
        auto held = true;

        for (int i = 0; i < 45; ++i)
        {
            tick (0.0f, 0.0f);
            held = held && juce::exactlyEqual (holdView.getHeaderHold (first, 0), 0.5f) && juce::exactlyEqual (holdView.getHeaderHold (first, 1), 0.25f);
        }

        const auto levelThen = holdView.getHeaderLevel (first, 0);
        tick (0.0f, 0.0f);
        const auto released = holdView.getHeaderHold (first, 0) < 0.5f && holdView.getHeaderHold (first, 0) >= holdView.getHeaderLevel (first, 0)
                              && levelThen < 1.0e-3f;
        tick (0.4f, 0.0f);
        const auto higher = juce::exactlyEqual (holdView.getHeaderHold (first, 0), 0.4f);
        check (held && released && higher && ! holdView.hasHeaderClipped (first),
               "header meters: a peak holds for 45 ticks after the bar has fallen, then falls; a new peak above the falling hold takes over");

        tick (0.99f, 1.0f);
        const auto clipped = holdView.hasHeaderClipped (first) && ! holdView.hasHeaderClipped (second);

        for (int i = 0; i < 200; ++i)
            tick (0.0f, 0.0f);

        const auto stays = holdView.hasHeaderClipped (first) && holdView.getHeaderHold (first, 1) < 1.0e-3f;

        // Clicking the meter at the header's right edge puts the light out.
        const auto meterPoint = juce::Point<int> (MultitrackView::headerWidth - 6, MultitrackView::rulerHeight + 20);
        auto* header = holdView.getComponentAt (meterPoint);
        const auto local = header != nullptr ? header->getLocalPoint (&holdView, meterPoint).toFloat() : juce::Point<float>();

        if (header != nullptr)
        {
            const auto now = juce::Time::getCurrentTime();
            const juce::MouseEvent press { juce::Desktop::getInstance().getMainMouseSource(), local, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                                           juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                                           juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                                           juce::MouseInputSource::defaultTiltY, header, header, now, local, now, 1, false };
            header->mouseDown (press);
        }

        const auto clickedOut = header != nullptr && ! holdView.hasHeaderClipped (first);
        check (clipped && stays && clickedOut,
               "header meters: a peak at 0 dB lights the clip light on that track only, which stays lit after the bars fall, until the meter is clicked");
        levels.take (0, 0);
    }

    {
        // A frozen track: an icy wash over the lane where its frozen audio plays, past its clips.
        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        SessionEdits::addClip (s, 0, juce::File ("/tmp/a.wav"), silence (48000), 0);
        engine.session.reset (s, {});
        view.zoomToFit();
        Mouse::settle();
        const auto shown = view.getVisibleFrames();
        const auto px = (int) std::lround ((double) MultitrackView::headerWidth + (96000.0 - shown.getStart()) / (shown.getLength() / timelineWidth));
        const auto py = (int) bodyY;
        const auto live = view.createComponentSnapshot (view.getLocalBounds(), true, 1.0f).getPixelAt (px, py);
        SessionEdits::freezeTrack (s, s.tracks[0].id, juce::File ("/tmp/a Frozen.wav"), silence (144000));
        engine.session.apply (s, "Freeze Track");
        Mouse::settle();
        const auto frozen = view.createComponentSnapshot (view.getLocalBounds(), true, 1.0f).getPixelAt (px, py);
        check (frozen.getBlue() > live.getBlue() + 20 && frozen.getBlue() > frozen.getRed(),
               "freeze: the lane shows an icy wash past the clips, where the frozen audio plays (blue " + juce::String (live.getBlue())
                   + " to " + juce::String (frozen.getBlue()) + ")");
    }

    {
        // Option-dragging a clip's edge stretches it: one clip from 1 s to 3 s in a 10 s view.
        SessionState s;
        s.sampleRate = rate;
        s.addTrack();
        SessionEdits::addClip (s, 0, juce::File ("/tmp/a.wav"), silence (96000), 48000);
        SessionEdits::addClip (s, 0, juce::File ("/tmp/b.wav"), silence (48000), 432000);
        const auto id = s.tracks[0].clips[0].id;
        engine.session.reset (s, {});
        view.zoomToFit();
        Mouse::settle();
        const auto shown = view.getVisibleFrames();
        const auto perPixel = shown.getLength() / timelineWidth;
        const auto at = [&] (juce::int64 frame) { return (float) MultitrackView::headerWidth + (float) (((double) frame - shown.getStart()) / perPixel); };
        juce::uint32 askedClip = 0;
        double askedRatio = 0.0;
        bool askedKeepEnd = false;
        int asked = 0;
        view.onStretchClip = [&] (juce::uint32 clipId, double ratio, bool keepEnd) { askedClip = clipId; askedRatio = ratio; askedKeepEnd = keepEnd; ++asked; };
        const auto wasSnapping = view.isSnapping();
        view.setSnapping (false);
        const auto option = juce::ModifierKeys (juce::ModifierKeys::altModifier);

        // While dragging the end to 4 s: an outline there, nothing changed yet.
        const auto from = juce::Point<float> (at (144000) - 2.0f, bodyY), to = juce::Point<float> (at (192000) - 2.0f, bodyY);
        const auto held = option.withFlags (juce::ModifierKeys::leftButtonModifier);
        const auto steps = engine.session.getUndoDescription();
        view.mouseDown (mouse.event (from, from, held, false));
        view.mouseDrag (mouse.event (to, from, held, true));
        const auto during = view.getStretchPreview();
        const auto unchanged = engine.session.getState()->tracks[0].clips[0].length == 96000;
        view.mouseUp (mouse.event (to, from, option, true));
        Mouse::settle();
        const auto endAsked = asked == 1 && askedClip == id && ! askedKeepEnd && std::abs (askedRatio - 1.5) < 2.0 * perPixel / 96000.0;

        check (std::abs (during.getEnd() - 192000) <= (juce::int64) perPixel && during.getStart() == 48000 && unchanged && endAsked
                   && view.getStretchPreview().isEmpty() && engine.session.getUndoDescription() == steps,
               "stretch by dragging: Option-dragging a clip's end shows where it would end (" + juce::String ((double) during.getEnd() / rate, 3)
                   + " s) without changing it, and letting go asks to stretch it by " + juce::String (askedRatio, 4) + ", keeping its start");

        // From the start, back to 0 s (half as long again); past the most it can stretch (twice).
        mouse.drag ({ at (48000) + 2.0f, bodyY }, { at (0) + 2.0f, bodyY }, option);
        const auto startRatio = askedRatio;
        const auto startAsked = asked == 2 && askedKeepEnd && std::abs (startRatio - 1.5) < 2.0 * perPixel / 96000.0;
        mouse.drag ({ at (144000) - 2.0f, bodyY }, { at (480000), bodyY }, option);
        const auto capped = asked == 3 && juce::exactlyEqual (askedRatio, 2.0);
        view.mouseDown (mouse.event (from, from, held, false));
        view.mouseDrag (mouse.event (to, from, held, true));
        view.mouseDrag (mouse.event (from, from, held, true));
        view.mouseUp (mouse.event (from, from, option, true));
        Mouse::settle();
        const auto sameLengthIgnored = asked == 3;
        mouse.drag ({ at (144000) - 2.0f, bodyY }, { at (160000), bodyY });
        const auto plainTrims = asked == 3 && engine.session.getUndoDescription() == "Trim Clip";

        check (startAsked && capped && sameLengthIgnored && plainTrims,
               "stretch by dragging: from the start it keeps the end (to 0 s, by " + juce::String (startRatio, 4) + "), it goes no further "
               "than twice as long, a drag that ends where it began asks nothing, and without Option an edge still trims");

        // Applied from the start: stretched audio, the clip ending where it did.
        auto next = *engine.session.getState();
        const auto before = next.tracks[0].clips[0];
        const auto end = before.getEnd();
        const auto stretched = SessionEdits::stretchedAudio (before, 1.25, rate);
        const auto kept = SessionEdits::stretchClip (next, id, juce::File ("/tmp/a Stretched.wav"), stretched, true);
        const auto& after = next.tracks[0].clips[0];
        auto tooLong = next;
        const auto refused = ! SessionEdits::stretchClip (tooLong, id, juce::File(), silence (after.getEnd() + 1), true);
        check (kept && after.getEnd() == end && after.length == stretched->getNumFrames() && after.length == (juce::int64) std::llround (1.25 * (double) before.length)
                   && after.position == end - after.length && refused,
               "stretch by dragging: stretched from the start, the clip ends where it did, " + juce::String ((double) after.length / rate, 3)
                   + " s long; one that would start before 0 is refused");
        view.onStretchClip = nullptr;
        view.setSnapping (wasSnapping);
    }

    engine.session.reset ({}, {});
    Mouse::settle();
}
