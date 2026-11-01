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
        }

        engine.removeTrackPlugin (trackId, 0);
    }

    engine.session.reset ({}, {});
    Mouse::settle();
}
