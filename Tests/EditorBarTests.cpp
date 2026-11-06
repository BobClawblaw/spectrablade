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
        EditorBar bar (commands, view, engine.document, { 1, 2, 3, 4, 5, 6, 7, 8 });
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

namespace
{
    void testTimes()
    {
        AudioEngine engine;
        loadSine (engine, 0.5f);   // 4,800 frames at 48 kHz: 0.1 s
        WaveformView view (engine);
        view.setBounds (0, 0, 800, 300);
        view.zoomToFit();
        juce::ApplicationCommandManager commands;
        EditorBar bar (commands, view, engine.document, { 1, 2, 3, 4, 5, 6, 7, 8 });
        juce::Array<int> ran;
        bar.run = [&] (int id) { ran.add (id); };
        bar.setBounds (0, 0, 1000, EditorBar::height);

        const auto& f = bar.getFields();
        const auto settle = [] (std::function<bool()> done)
        {
            for (int i = 0; i < 50 && ! done(); ++i)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        };

        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);   // the file's arrival, which puts the cursor at the start
        view.setCursor (1200);
        settle ([&] { return f[0].text == "0:00.025"; });
        const auto dash = juce::String::fromUTF8 ("\xe2\x80\x93");
        const auto before = f[0].text + " | " + f[1].text + " | " + f[2].text;

        view.setSelection ({ 960, 2400 });
        settle ([&] { return f[1].text != "none"; });
        check (f.size() == 3 && f[0].name == "Cursor" && before == "0:00.025 | none | 0:00.000 " + dash + " 0:00.100"
                   && f[1].text == "0:00.020 " + dash + " 0:00.050  (0:00.030)" && f[0].text == "0:00.020",
               "editor bar: shows the cursor, the selection and the view (0:00.025, none, the whole 0.1 s; then with a selection 0:00.020 to 0:00.050, "
               "0:00.030 long, and the cursor at its start): " + before + " / " + f[0].text + " | " + f[1].text);

        bool ordered = true;

        for (size_t i = 0; i < f.size(); ++i)
            ordered = ordered && ! f[i].bounds.isEmpty() && (i == 0 || f[i - 1].bounds.getRight() < f[i].bounds.getX())
                      && f[i].bounds.getRight() < bar.getButton (1).getX();

        const auto now = juce::Time::getCurrentTime();
        const auto clickAt = [&] (juce::Point<int> p)
        {
            bar.mouseUp (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(), {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                           &bar, &bar, now, p.toFloat(), now, 1, false));
        };

        clickAt (f[0].bounds.getCentre());
        clickAt (f[1].bounds.getCentre());
        clickAt (f[2].bounds.getCentre());
        check (ordered && ran == juce::Array<int> { 7, 8 },
               "editor bar: the fields sit left to right before the zoom controls; a click on the cursor runs Go to Time, on the selection Set Selection, and on the view nothing");

        // Narrowing it a pixel at a time: the fields go from the right, and those left stay clear of the zoom controls.
        int viewGone = 0, selectionGone = 0, cursorGone = 0;
        bool clear = true;

        for (int w = 1000; w > 200; --w)
        {
            bar.setSize (w, EditorBar::height);

            for (auto& field : f)
                clear = clear && (field.bounds.isEmpty() || field.bounds.getRight() < bar.getButton (1).getX());

            if (viewGone == 0 && f[2].bounds.isEmpty())      viewGone = w;
            if (selectionGone == 0 && f[1].bounds.isEmpty()) selectionGone = w;
            if (cursorGone == 0 && f[0].bounds.isEmpty())    cursorGone = w;
        }

        check (clear && viewGone > selectionGone && selectionGone > cursorGone && cursorGone > 0,
               "editor bar: narrowed, the view's field goes first (at " + juce::String (viewGone) + " pixels), then the selection's ("
                   + juce::String (selectionGone) + "), then the cursor's (" + juce::String (cursorGone) + "), and none overlaps the zoom controls");

        view.setVerticalZoom (1.0);
    }
}

void runEditorBarTests()
{
    testVerticalZoom();
    testBar();
    testTimes();
}
