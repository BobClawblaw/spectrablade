#include <JuceHeader.h>
#include "../Source/AudioEdits.h"
#include "../Source/AudioFileIO.h"
#include "../Source/MultitrackView.h"
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

    engine.session.reset ({}, {});
    Mouse::settle();
}
