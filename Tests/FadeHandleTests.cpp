#include <JuceHeader.h>
#include "../Source/AudioEdits.h"
#include "../Source/AudioEngine.h"
#include "../Source/FileFades.h"
#include "../Source/WaveformView.h"
#include "TestUtils.h"
#include "FadeHandleTests.h"

namespace
{
    using TestUtils::check;

    std::shared_ptr<const AudioSnapshot> ones (int channels, int frames)
    {
        juce::AudioBuffer<float> b (channels, frames);

        for (int ch = 0; ch < channels; ++ch)
            juce::FloatVectorOperations::fill (b.getWritePointer (ch), 1.0f, frames);

        return AudioEdits::fromBuffer (std::move (b));
    }

    float sampleAt (const AudioSnapshot& s, int channel, juce::int64 frame)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), 1);
        s.read (b, 0, frame, 1);
        return b.getSample (channel, 0);
    }

    void testCurves()
    {
        using FileFades::level;
        const auto straight = std::abs (level (0.25, 0.0f) - 0.25f) < 1.0e-6f && std::abs (level (0.5, 0.0f) - 0.5f) < 1.0e-6f;
        const auto ends = juce::exactlyEqual (level (0.0, 1.0f), 0.0f) && juce::exactlyEqual (level (1.0, -1.0f), 1.0f)
                          && juce::exactlyEqual (level (-0.5, 0.3f), 0.0f) && juce::exactlyEqual (level (2.0, 0.3f), 1.0f);
        const auto fast = std::abs (level (0.5, 1.0f) - (float) std::pow (0.5, 0.25)) < 1.0e-6f;
        const auto slow = std::abs (level (0.5, -1.0f) - 0.0625f) < 1.0e-6f;
        auto rising = true;

        for (auto curve : { -1.0f, -0.4f, 0.0f, 0.6f, 1.0f })
            for (int i = 1; i <= 100; ++i)
                rising = rising && level (i / 100.0, curve) >= level ((i - 1) / 100.0, curve);

        check (straight && ends && fast && slow && rising,
               "fade handles: curve 0 is a straight line, +1 rises quickly (0.84 halfway), -1 slowly (0.06 halfway), each from 0 to 1 and never falling");
    }

    void testRender()
    {
        const auto source = ones (2, 1000);
        const auto faded = FileFades::render (*source, { 100, 0.0f }, { 200, 0.5f });
        const auto in = std::abs (sampleAt (*faded, 0, 0) - 0.01f) < 1.0e-6f && std::abs (sampleAt (*faded, 1, 49) - 0.5f) < 1.0e-6f
                        && juce::exactlyEqual (sampleAt (*faded, 0, 99), 1.0f);
        const auto middle = juce::exactlyEqual (sampleAt (*faded, 0, 100), 1.0f) && juce::exactlyEqual (sampleAt (*faded, 1, 799), 1.0f);
        const auto out = std::abs (sampleAt (*faded, 0, 800) - 1.0f) < 1.0e-6f
                         && std::abs (sampleAt (*faded, 1, 899) - FileFades::level (101.0 / 200.0, 0.5f)) < 1.0e-6f
                         && std::abs (sampleAt (*faded, 0, 999) - FileFades::level (1.0 / 200.0, 0.5f)) < 1.0e-6f;
        check (faded->getNumFrames() == 1000 && in && middle && out,
               "fade handles: a fade in over the first frames to full level, a fade out over the last along its curve, the middle untouched");

        const auto overlapping = FileFades::render (*source, { 700, 0.0f }, { 700, 0.0f });
        check (std::abs (sampleAt (*overlapping, 0, 699) - 1.0f) < 1.0e-6f && std::abs (sampleAt (*overlapping, 0, 700) - 1.0f) < 1.0e-6f
                   && std::abs (sampleAt (*overlapping, 0, 999) - FileFades::level (1.0 / 300.0, 0.0f)) < 1.0e-6f
                   && FileFades::render (*source, {}, {}) == nullptr,
               "fade handles: a fade out is shortened so the two never overlap, and no fades makes nothing");
    }

    void testDraft()
    {
        AudioDocument doc;
        doc.reset (ones (1, 1000), 48000.0, {}, 24);
        FileFades::Draft draft;

        draft.set (doc, false, { 100, 0.0f });
        const auto first = doc.getPosition() == 1 && doc.getUndoDescription() == "Fade In" && draft.isLive (doc);
        draft.set (doc, false, { 300, 0.5f });
        draft.set (doc, true, { 200, -0.5f });
        const auto oneStep = doc.getPosition() == 1 && doc.getUndoDescription() == "Fade In and Out" && doc.getHistory().size() == 2;
        const auto reworked = std::abs (sampleAt (*doc.getSnapshot(), 0, 149) - FileFades::level (0.5, 0.5f)) < 1.0e-6f
                              && juce::exactlyEqual (sampleAt (*doc.getSnapshot(), 0, 300), 1.0f)
                              && std::abs (sampleAt (*doc.getSnapshot(), 0, 899) - FileFades::level (101.0 / 200.0, -0.5f)) < 1.0e-6f;
        check (first && oneStep && reworked,
               "fade handles: dragging again redoes the fades from the audio before them, both in one undo step");

        const auto limited = draft.set (doc, true, { 5000, 2.0f });
        check (limited.frames == 700 && juce::exactlyEqual (limited.curve, 1.0f) && draft.maxFrames (doc, false) == 300,
               "fade handles: a fade is kept within what the other leaves, and its curve within -1 to +1");

        doc.undo();
        const auto undone = ! draft.isLive (doc) && juce::exactlyEqual (sampleAt (*doc.getSnapshot(), 0, 0), 1.0f) && draft.get (doc, false).frames == 0;
        doc.redo();
        const auto redone = ! draft.isLive (doc);
        check (undone && redone,
               "fade handles: one undo takes both fades away, and after an undo or redo they are no longer adjustable");

        doc.apply (AudioEdits::gain (*doc.getSnapshot(), 0, 1000, -6.0f), "Gain");
        draft.set (doc, false, { 50, 0.0f });
        const auto stacked = doc.getPosition() == 3 && doc.getUndoDescription() == "Fade In"
                             && std::abs (sampleAt (*doc.getSnapshot(), 0, 49) - juce::Decibels::decibelsToGain (-6.0f) * FileFades::level (50.0 / 300.0, 0.5f)) < 1.0e-5f
                             && std::abs (sampleAt (*doc.getSnapshot(), 0, 24) - juce::Decibels::decibelsToGain (-6.0f) * FileFades::level (25.0 / 300.0, 0.5f) * FileFades::level (25.0 / 50.0, 0.0f)) < 1.0e-5f
                             && std::abs (sampleAt (*doc.getSnapshot(), 0, 999) - juce::Decibels::decibelsToGain (-6.0f) * FileFades::level (1.0 / 700.0, 1.0f)) < 1.0e-5f;
        check (stacked, "fade handles: after another edit a new fade starts on the audio as it is, as a step of its own");

        draft.set (doc, false, {});
        check (doc.getPosition() == 2 && ! draft.isLive (doc) && doc.getUndoDescription() == "Gain",
               "fade handles: setting the only fade to nothing takes its step away");
    }

    juce::MouseEvent mouse (juce::Component& c, juce::Point<float> at, juce::Point<float> down, int clicks = 1)
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                                 now, down, now, clicks, at != down);
    }

    void testView()
    {
        AudioEngine engine;
        engine.document.reset (ones (2, 48000), 48000.0, {}, 24);
        WaveformView view (engine);
        view.setBounds (0, 0, 1000, 600);
        view.zoomToFit();
        const auto area = view.getWaveArea().toFloat();

        const auto inHandle = view.getFadeHandle (false), outHandle = view.getFadeHandle (true);
        check (inHandle.getY() > area.getY() && inHandle.getX() >= area.getX() && inHandle.getX() < area.getX() + 4.0f
                   && outHandle.getRight() <= area.getRight() && outHandle.getRight() > area.getRight() - 4.0f,
               "fade handles: with no fades the handles sit at the waveform's top corners");

        // Drag the fade in's handle a quarter of the way across and 60 pixels up.
        const auto down = inHandle.getCentre();
        const auto target = juce::Point<float> (area.getX() + area.getWidth() * 0.25f, down.y - 60.0f);
        view.mouseDown (mouse (view, down, down));
        view.mouseDrag (mouse (view, target, down));
        view.mouseUp (mouse (view, target, down));
        const auto fade = view.getFade (false);
        const auto expectedFrames = 12000.0;
        check (std::abs ((double) fade.frames - expectedFrames) < 100.0 && std::abs (fade.curve - 0.5f) < 1.0e-6f
                   && engine.document.getUndoDescription() == "Fade In" && std::abs (view.getFadeHandle (false).getCentreX() - target.x) < 1.5f,
               "fade handles: dragging the fade in's handle across sets its length and up bends its curve, and the handle follows");

        // The fade out's handle, dragged left and down; then the fade in again: still one step.
        const auto outDown = view.getFadeHandle (true).getCentre();
        const auto outTarget = juce::Point<float> (area.getRight() - area.getWidth() * 0.1f, outDown.y + 30.0f);
        view.mouseDown (mouse (view, outDown, outDown));
        view.mouseDrag (mouse (view, outTarget, outDown));
        view.mouseUp (mouse (view, outTarget, outDown));
        const auto inDown = view.getFadeHandle (false).getCentre();
        view.mouseDown (mouse (view, inDown, inDown));
        view.mouseDrag (mouse (view, inDown.translated (-40.0f, 0.0f), inDown));
        view.mouseUp (mouse (view, inDown.translated (-40.0f, 0.0f), inDown));
        check (std::abs ((double) view.getFade (true).frames - 4800.0) < 100.0 && std::abs (view.getFade (true).curve + 0.25f) < 1.0e-6f
                   && std::abs (view.getFade (false).curve - 0.5f) < 1.0e-6f && view.getFade (false).frames < fade.frames
                   && engine.document.getPosition() == 1 && engine.document.getUndoDescription() == "Fade In and Out"
                   && view.getSelection().isEmpty(),
               "fade handles: the fade out's handle sets its own fade, and a later drag keeps the curve and both stay one undo step, selecting nothing");

        view.createComponentSnapshot (view.getLocalBounds());   // paints the curves and handles

        const auto at = view.getFadeHandle (true).getCentre();
        view.mouseDown (mouse (view, at, at));
        view.mouseUp (mouse (view, at, at));
        view.mouseDown (mouse (view, at, at, 2));
        view.mouseDoubleClick (mouse (view, at, at, 2));
        view.mouseUp (mouse (view, at, at, 2));
        check (view.getFade (true).frames == 0 && view.getFade (false).frames > 0 && engine.document.getUndoDescription() == "Fade In"
                   && view.getSelection().isEmpty(),
               "fade handles: double-clicking a handle takes its fade away, leaving the other, without selecting the file");
    }
}

void runFadeHandleTests()
{
    testCurves();
    testRender();
    testDraft();
    testView();
}
