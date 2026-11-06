#include <JuceHeader.h>
#include "../Source/EditorBar.h"
#include "../Source/AudioEdits.h"
#include "../Source/Theme.h"
#include "TestUtils.h"
#include "EditorBarTests.h"

namespace
{
    using TestUtils::check;

    /** A mono file holding a sine at a level, 0.1 s at 48 kHz. */
    void loadSine (AudioEngine& engine, float level)
    {
        juce::AudioBuffer<float> b (1, 4800);

        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, level * std::sin ((float) i * 0.05f));

        engine.document.reset (AudioEdits::fromBuffer (std::move (b)), 48000.0, {}, 24);
    }

    /** How far above the wave area's middle the waveform's colour reaches, at most, over 60
        columns from x (more than a cycle of the sine). */
    int reach (WaveformView& view, int x)
    {
        const auto image = view.createComponentSnapshot (view.getLocalBounds());
        const auto area = view.getWaveArea();
        const auto mid = area.getCentreY();
        int most = 0;

        for (int column = x; column < x + 60; ++column)
            for (int y = area.getY(); y < mid; ++y)
                if (image.getPixelAt (column, y) == Theme::waveform)
                {
                    most = juce::jmax (most, mid - y);
                    break;
                }

        return most;
    }

    void testVerticalZoom()
    {
        check (std::abs (WaveformView::edgeLevelDb (1.0)) < 1.0e-9 && std::abs (WaveformView::edgeLevelDb (2.0) + 6.0206) < 1.0e-3
                   && std::abs (WaveformView::edgeLevelDb (64.0) + 36.1236) < 1.0e-3,
               "vertical zoom: a lane's edges stand for 0 dB at 1x, -6.02 dB at 2x and -36.12 dB at 64x");

        AudioEngine engine;
        loadSine (engine, 0.25f);   // -12 dB
        WaveformView view (engine);
        view.setDisplay (WaveformView::Display::waveform);
        view.setBounds (0, 0, 800, 400);
        view.zoomToFit();

        const auto x = view.getWaveArea().getCentreX();
        const auto at1 = reach (view, x);
        view.zoomInVertically();
        const auto at2 = reach (view, x);
        view.zoomInVertically();
        const auto at4 = reach (view, x);
        view.setVerticalZoom (8.0);
        const auto at8 = reach (view, x);
        view.setVerticalZoom (16.0);
        const auto at16 = reach (view, x);

        check (at1 > 10 && std::abs (at2 - 2 * at1) <= 2 && std::abs (at4 - 4 * at1) <= 4 && at8 == at16 && at8 >= at4,
               "vertical zoom: a -12 dB sine reaches 2 and 4 times as far from the middle at 2x and 4x (" + juce::String (at1) + ", "
                   + juce::String (at2) + ", " + juce::String (at4) + " pixels), where it fills the lane, and beyond that stops at the lane's edge ("
                   + juce::String (at8) + " at 8x and 16x)");

        view.setVerticalZoom (500.0);
        const auto high = view.getVerticalZoom();
        view.setVerticalZoom (0.1);
        check (juce::exactlyEqual (high, WaveformView::maxVerticalZoom) && juce::exactlyEqual (view.getVerticalZoom(), 1.0),
               "vertical zoom: kept from 1x to 64x");

        const auto now = juce::Time::getCurrentTime();
        const auto at = view.getWaveArea().getCentre().toFloat();
        const juce::MouseEvent plain (juce::Desktop::getInstance().getMainMouseSource(), at, {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                      &view, &view, now, at, now, 1, false);
        juce::MouseWheelDetails up {};
        up.deltaY = 0.5f;
        const auto framesBefore = view.getVisibleFrames().getLength();
        view.mouseWheelMove (plain, up);
        const auto plainZoom = view.getVerticalZoom();
        const auto framesAfter = view.getVisibleFrames().getLength();
        const juce::MouseEvent withOption (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys::altModifier, 1.0f, 0.0f, 0.0f,
                                     0.0f, 0.0f, &view, &view, now, at, now, 1, false);
        view.mouseWheelMove (withOption, up);
        check (juce::exactlyEqual (plainZoom, 1.0) && framesAfter < framesBefore && std::abs (view.getVerticalZoom() - 2.0) < 1.0e-9,
               "vertical zoom: Option-scrolling zooms vertically (here to 2x), plain scrolling zooms in time only");
    }

    void testBar()
    {
        AudioEngine engine;
        loadSine (engine, 0.5f);
        WaveformView view (engine);
        view.setBounds (0, 0, 800, 300);
        juce::ApplicationCommandManager commands;
        EditorBar bar (commands, view, { 1, 2, 3, 4, 5, 6 });
        juce::Array<int> ran;
        bar.run = [&] (int id) { ran.add (id); };
        bar.setBounds (0, 0, 800, EditorBar::height);
        view.setVerticalZoom (2.0);   // so that every button is on
        juce::MessageManager::getInstance()->runDispatchLoopUntil (150);

        for (int id = 1; id <= 6; ++id)
            bar.getButton (id).triggerClick();

        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
        bool leftToRight = true;

        for (int id = 1; id < 6; ++id)
            leftToRight = leftToRight && bar.getButton (id).getRight() <= bar.getButton (id + 1).getX();

        check (ran == juce::Array<int> { 1, 2, 3, 4, 5, 6 } && leftToRight && bar.getButton (6).getRight() <= bar.getWidth(),
               "editor bar: zoom out, in and fit, then vertical out, in and full scale, left to right, each running its command");

        view.setVerticalZoom (1.0);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (150);
        const auto atOne = bar.getShownVertical() == "1x" && ! bar.getButton (4).isEnabled() && ! bar.getButton (6).isEnabled() && bar.getButton (5).isEnabled();
        view.setVerticalZoom (8.0);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (150);
        const auto atEight = bar.getShownVertical() == "8x" && bar.getButton (4).isEnabled() && bar.getButton (6).isEnabled();
        view.setVerticalZoom (64.0);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (150);
        check (atOne && atEight && bar.getShownVertical() == "64x" && ! bar.getButton (5).isEnabled(),
               "editor bar: shows the vertical zoom as it changes (1x, 8x, 64x), with out and full scale off at 1x and in off at 64x");
        check (EditorBar::verticalText (1.5) == "1.5x" && EditorBar::verticalText (2.0) == "2x" && EditorBar::verticalText (12.0) == "12x",
               "editor bar: zooms read 1.5x, 2x, 12x");
    }
}

void runEditorBarTests()
{
    testVerticalZoom();
    testBar();
}
