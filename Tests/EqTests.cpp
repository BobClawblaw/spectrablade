#include <JuceHeader.h>
#include "../Source/ParametricEq.h"
#include "TestUtils.h"
#include "EqTests.h"

namespace
{
    using TestUtils::check;
    using Equalizer::Band;
    using Equalizer::BandType;

    constexpr double rate = 48000.0;

    juce::AudioBuffer<float> sine (double hz, int frames, int channels = 1, float amplitude = 0.25f)
    {
        juce::AudioBuffer<float> b (channels, frames);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                b.setSample (ch, i, amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / rate + ch));

        return b;
    }

    juce::AudioBuffer<float> readAll (const AudioSnapshot& s)
    {
        juce::AudioBuffer<float> b (s.getNumChannels(), (int) s.getNumFrames());
        s.read (b, 0, 0, b.getNumSamples());
        return b;
    }

    double rmsDb (const juce::AudioBuffer<float>& b, int ch, int from, int to)
    {
        double sum = 0.0;

        for (int i = from; i < to; ++i)
            sum += (double) b.getSample (ch, i) * b.getSample (ch, i);

        return 10.0 * std::log10 (sum / (to - from));
    }

    /** The level change the bands make to a 0.25 sine at hz over its second half, in dB. */
    double measuredDb (const std::vector<Band>& bands, double hz)
    {
        const auto in = sine (hz, 96000);
        const auto out = readAll (*Equalizer::apply (*AudioEdits::fromBuffer (juce::AudioBuffer<float> (in)), 0, 96000, rate, bands));
        return rmsDb (out, 0, 48000, 96000) - rmsDb (in, 0, 48000, 96000);
    }
}

void runEqTests()
{
    const Band bell { BandType::peak, 1000.0, 6.0, 1.0 };
    const Band lowShelf { BandType::lowShelf, 200.0, 6.0, 0.7071067811865476 };
    const Band highShelf { BandType::highShelf, 5000.0, -9.0, 0.7071067811865476 };
    const Band highPass { BandType::highPass, 1000.0, 0.0, 0.7071067811865476 };
    const Band lowPass { BandType::lowPass, 1000.0, 0.0, 0.7071067811865476 };
    const auto near = [] (double a, double b, double tolerance) { return std::abs (a - b) <= tolerance; };
    const auto r = [] (const Band& band, double hz) { return Equalizer::responseDb (band, rate, hz); };

    check (near (r (bell, 1000.0), 6.0, 1.0e-9) && std::abs (r (bell, 20.0)) < 0.01 && std::abs (r (bell, 20000.0)) < 0.1
               && near (r (lowShelf, 200.0), 3.0, 1.0e-9) && near (r (lowShelf, 20.0), 6.0, 0.05) && std::abs (r (lowShelf, 10000.0)) < 0.01
               && near (r (highShelf, 5000.0), -4.5, 1.0e-9) && near (r (highShelf, 23000.0), -9.0, 0.05) && std::abs (r (highShelf, 100.0)) < 0.01
               && near (r (highPass, 1000.0), -3.0103, 1.0e-4) && near (r (highPass, 500.0), 10.0 * std::log10 (1.0 / 17.0), 0.03) && std::abs (r (highPass, 10000.0)) < 0.01
               && near (r (lowPass, 1000.0), -3.0103, 1.0e-4) && std::abs (r (lowPass, 50.0)) < 0.01,
           "eq responses: a bell's gain at its frequency, half a shelf's gain at its frequency, -3 dB at a pass's corner and within 0.03 dB of the analogue -12.30 an octave past it, flat far away");

    check (near (measuredDb ({ bell }, 1000.0), 6.0, 0.01) && near (measuredDb ({ bell }, 3000.0), r (bell, 3000.0), 0.01)
               && near (measuredDb ({ highPass }, 100.0), r (highPass, 100.0), 0.05) && near (measuredDb ({ lowShelf }, 50.0), r (lowShelf, 50.0), 0.01)
               && near (measuredDb ({ bell, highPass }, 700.0), r (bell, 700.0) + r (highPass, 700.0), 0.01),
           "eq on sines: each sine's level changes by the band's response at its frequency, and two bands by the sum");

    // Frames outside the range are kept bit for bit; the range starts as it would in a longer pass.
    const auto tone = sine (440.0, 144000, 2);
    const auto toneSnap = AudioEdits::fromBuffer (juce::AudioBuffer<float> (tone));
    const auto part = readAll (*Equalizer::apply (*toneSnap, 72000, 96000, rate, { bell, highShelf }));
    const auto whole = readAll (*Equalizer::apply (*toneSnap, 0, 144000, rate, { bell, highShelf }));
    auto outsideKept = true, startsLikeWhole = true;
    float worstStart = 0.0f;

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 144000; ++i)
        {
            if (i < 72000 || i >= 96000)
                outsideKept = outsideKept && juce::exactlyEqual (part.getSample (ch, i), tone.getSample (ch, i));
            else
                worstStart = juce::jmax (worstStart, std::abs (part.getSample (ch, i) - whole.getSample (ch, i)));
        }

    startsLikeWhole = worstStart < 1.0e-6f;
    std::cout << "      eq: worst difference from the whole-file pass inside the range " << worstStart << std::endl;
    check (outsideKept && startsLikeWhole, "eq on a range: every frame outside it bit for bit, the range within 1e-6 of a pass over the whole file");

    const auto flat = readAll (*Equalizer::apply (*toneSnap, 0, 144000, rate, { Band { BandType::peak, 1000.0, 0.0, 1.0 } }));
    float worstFlat = 0.0f;

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 144000; ++i)
            worstFlat = juce::jmax (worstFlat, std::abs (flat.getSample (ch, i) - tone.getSample (ch, i)));

    const auto refused = [&toneSnap] (const Band& band) { return Equalizer::apply (*toneSnap, 0, 1000, rate, { band }) == nullptr; };
    check (worstFlat < 1.0e-6f && refused ({ BandType::peak, 0.0, 6.0, 1.0 }) && refused ({ BandType::peak, 24000.0, 6.0, 1.0 })
               && refused ({ BandType::peak, 1000.0, 6.0, 0.0 }) && refused ({ BandType::peak, 1000.0, 60.0, 1.0 })
               && Equalizer::apply (*toneSnap, 0, 1000, rate, {}) == nullptr && Equalizer::apply (*toneSnap, 500, 500, rate, { bell }) == nullptr,
           "eq: a 0 dB bell gives the audio back within 1e-6; no frequency, one at Nyquist, no q, 60 dB, no bands or an empty range refused");

    // The parametric panel, driven through its own controls.
    ParametricEq panel (ParametricEq::defaultSettings(), rate);
    auto changes = 0;
    panel.onChange = [&changes] { ++changes; };
    const auto startsEmpty = panel.getActiveBands().empty() && juce::exactlyEqual (panel.responseDb (1000.0), 0.0);

    panel.getRow (2).gain.setValue (6.0, juce::sendNotificationSync);
    panel.getRow (2).q.setValue (2.0, juce::sendNotificationSync);
    const auto oneBell = panel.getActiveBands();
    const auto bellRight = oneBell.size() == 1 && oneBell[0].type == BandType::peak && near (oneBell[0].frequency, 1000.0, 1.0e-9)
                        && near (oneBell[0].gainDb, 6.0, 1.0e-9) && near (oneBell[0].q, 2.0, 1.0e-9)
                        && near (panel.responseDb (1000.0), 6.0, 1.0e-6);

    panel.getRow (0).type.setSelectedItemIndex ((int) BandType::highPass, juce::sendNotificationSync);
    panel.getRow (0).frequency.setValue (80.0, juce::sendNotificationSync);
    const auto withPass = panel.getActiveBands();
    const Band pass80 { BandType::highPass, 80.0, 0.0, 0.7071067811865476 };
    const auto passRight = withPass.size() == 2 && withPass[0].type == BandType::highPass && ! panel.getRow (0).gain.isEnabled()
                        && near (withPass[0].frequency, 80.0, 1.0e-9)
                        && near (panel.responseDb (80.0), r (pass80, 80.0) + Equalizer::responseDb (oneBell[0], rate, 80.0), 1.0e-6);

    panel.getRow (2).on.setToggleState (false, juce::sendNotificationSync);
    const auto bellOff = panel.getActiveBands().size() == 1 && ! panel.getSettings()[2].on;

    check (startsEmpty && bellRight && passRight && bellOff && changes == 5,
           "parametric eq: flat at first; a bell's gain and Q, a band made a high-pass and moved, and a band switched off, each read from its controls; the response the sum of the active bands");

    panel.setSize (640, 360);
    const auto area = panel.getCurveArea();
    check (near (panel.xForFrequency (20.0), area.getX(), 1.0e-3) && near (panel.xForFrequency (20000.0), area.getRight(), 1.0e-3)
               && near (panel.frequencyForX (panel.xForFrequency (1234.5f)), 1234.5, 0.01) && near (panel.yForDb (0.0), area.getCentreY(), 1.0e-3)
               && near (panel.yForDb (24.0), area.getY(), 1.0e-3) && near (panel.yForDb (60.0), area.getY(), 1.0e-3),
           "parametric eq drawing: 20 Hz to 20 kHz across on a log scale and back, 0 dB in the middle, +24 dB at the top and anything past it held there");

    {
        // Dragging the points, through synthesised mouse events on a 640 by 360 panel.
        ParametricEq drag (ParametricEq::defaultSettings(), rate);
        drag.setSize (640, 360);
        const auto event = [&drag] (juce::Point<float> at, juce::Point<float> down, bool dragged)
        {
            const auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys::leftButtonModifier,
                                     juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                                     juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                                     juce::MouseInputSource::defaultTiltY, &drag, &drag, now, down, now, 1, dragged);
        };
        const auto dragTo = [&drag, &event] (int band, double hz, double db)
        {
            const auto from = drag.pointFor (band);
            const juce::Point<float> to (drag.xForFrequency (hz), drag.yForDb (db));
            drag.mouseDown (event (from, from, false));
            drag.mouseDrag (event (to, from, true));
            drag.mouseUp (event (to, from, true));
        };
        const auto& s = drag.getSettings();

        dragTo (2, 2000.0, 6.0);
        const auto bellMoved = near (s[2].band.frequency, 2000.0, 0.5) && near (s[2].band.gainDb, 6.0, 0.1) && near (drag.responseDb (s[2].band.frequency), 6.0, 0.1);

        drag.getRow (1).gain.setValue (-6.0, juce::sendNotificationSync);
        dragTo (2, 500.0, 3.0);
        const auto underMouse = near (s[2].band.frequency, 500.0, 0.5) && near (drag.responseDb (s[2].band.frequency), 3.0, 0.1) && s[2].band.gainDb > 3.5;

        dragTo (4, 6000.0, 4.0);
        const auto shelfMoved = near (s[4].band.frequency, 6000.0, 1.0) && near (drag.responseDb (s[4].band.frequency), 4.0, 0.1);

        drag.getRow (0).type.setSelectedItemIndex ((int) BandType::highPass, juce::sendNotificationSync);
        dragTo (0, 60.0, 12.0);
        const auto passMoved = near (s[0].band.frequency, 60.0, 0.2) && juce::exactlyEqual (s[0].band.gainDb, 0.0);

        const auto before = s;
        const juce::Point<float> empty (drag.xForFrequency (30.0), drag.yForDb (20.0));
        drag.mouseDown (event (empty, empty, false));
        drag.mouseDrag (event (empty.translated (40.0f, 40.0f), empty, true));
        drag.mouseUp (event (empty.translated (40.0f, 40.0f), empty, true));
        drag.getRow (3).on.setToggleState (false, juce::sendNotificationSync);
        const auto offBand = drag.pointFor (3);
        const auto missesRight = drag.bandAt (empty) < 0 && drag.bandAt (offBand) != 3 && near (s[0].band.frequency, before[0].band.frequency, 0.0)
                              && near (s[2].band.gainDb, before[2].band.gainDb, 0.0);

        const auto q0 = s[2].band.q;
        juce::MouseWheelDetails up {};
        up.deltaY = 0.25f;
        auto down = up;
        down.deltaY = -0.25f;
        const auto at = drag.pointFor (2);
        drag.mouseWheelMove (event (at, at, false), up);
        const auto qUp = s[2].band.q;
        drag.mouseWheelMove (event (at, at, false), down);
        const auto wheelRight = near (qUp, q0 * 1.1, 1.0e-9) && near (s[2].band.q, q0, 1.0e-9);

        check (bellMoved && underMouse && shelfMoved && passMoved && missesRight && wheelRight,
               "parametric eq points: dragging a bell or a shelf sets its frequency and puts the curve under the mouse (to 0.1 dB), "
               "a pass moves in frequency only, a press away from the points or on a band switched off moves nothing, the wheel raises and lowers the Q by a tenth");
    }

    {
        // Double-clicking: a handle switches its band off, its ring on again, empty space places the
        // first band switched off there; pressing a handle picks its band out; the spectrum falls back.
        ParametricEq eq (ParametricEq::defaultSettings(), rate);
        eq.setSize (640, 360);
        const auto event = [&eq] (juce::Point<float> at)
        {
            const auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys::leftButtonModifier,
                                     juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                                     juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                                     juce::MouseInputSource::defaultTiltY, &eq, &eq, now, at, now, 2, false);
        };
        const auto& s = eq.getSettings();
        const auto unpicked = eq.getSelectedBand() == -1;
        eq.mouseDown (event (eq.pointFor (1)));
        eq.mouseUp (event (eq.pointFor (1)));
        const auto picked = eq.getSelectedBand() == 1;

        eq.mouseDoubleClick (event (eq.pointFor (3)));
        const auto ring = juce::Point<float> (eq.xForFrequency (s[3].band.frequency), eq.yForDb (0.0));
        const auto off = ! s[3].on && eq.offBandAt (ring) == 3 && eq.bandAt (ring) != 3;
        eq.mouseDoubleClick (event (ring));
        const auto onAgain = s[3].on && eq.offBandAt (ring) == -1 && eq.getSelectedBand() == 3;

        eq.getRow (1).on.setToggleState (false, juce::sendNotificationSync);
        const juce::Point<float> spot (eq.xForFrequency (700.0), eq.yForDb (5.0));
        eq.mouseDoubleClick (event (spot));
        const auto placed = s[1].on && std::abs (s[1].band.frequency - 700.0) < 1.0 && std::abs (eq.responseDb (s[1].band.frequency) - 5.0) < 0.1;
        eq.mouseDoubleClick (event (juce::Point<float> (eq.xForFrequency (40.0), eq.yForDb (-20.0))));
        const auto noneLeft = s[0].on && s[1].on && s[2].on && s[3].on && s[4].on && std::abs (s[1].band.frequency - 700.0) < 1.0;

        std::vector<float> loud (Analysis::fftSize / 2 + 1, -20.0f), quiet (Analysis::fftSize / 2 + 1, -100.0f);
        eq.takeSpectrum (loud, rate);
        eq.takeSpectrum (quiet, rate);
        const auto falls = std::abs (eq.getSpectrum()[100] - (-20.6f)) < 1.0e-4f;
        auto distinct = true;

        for (int a = 0; a < ParametricEq::numBands; ++a)
            for (int b = a + 1; b < ParametricEq::numBands; ++b)
                distinct = distinct && ParametricEq::bandColour (a) != ParametricEq::bandColour (b);

        check (unpicked && picked && off && onAgain && placed && noneLeft && falls && distinct,
               "parametric eq: pressing a handle picks its band out; double-clicking it switches the band off (a ring on the 0 dB line), the ring "
               "on again; double-clicking empty space puts the first band off there (700 Hz, the curve at +5 dB), nothing when none is off; "
               "the spectrum falls back 0.6 dB a frame; each band its own colour");
    }

    auto passFirst = ParametricEq::defaultSettings();
    passFirst[0].band.type = BandType::lowPass;
    check (! ParametricEq (passFirst, rate).getRow (0).gain.isEnabled() && ParametricEq (passFirst, rate).getRow (1).gain.isEnabled(),
           "parametric eq: a band that opens as a pass has its gain greyed out, a bell's not");

    ParametricEq touched (ParametricEq::defaultSettings(), rate);
    touched.getRow (2).q.setValue (3.0, juce::sendNotificationSync);
    touched.getRow (2).gain.setValue (0.0, juce::sendNotificationSync);
    check (touched.getActiveBands().empty() && juce::exactlyEqual (touched.getSettings()[2].band.gainDb, 0.0),
           "parametric eq: a bell at 0 dB whose row was changed reads exactly 0 dB and still changes nothing");

    ParametricEq slow (ParametricEq::defaultSettings(), 22050.0);
    check (slow.getRow (4).frequency.getMaximum() < 22050.0 / 2.0 && juce::exactlyEqual (slow.getSettings()[4].band.frequency, 8000.0),
           "parametric eq at 22.05 kHz: no band can be set at or past Nyquist");
}
